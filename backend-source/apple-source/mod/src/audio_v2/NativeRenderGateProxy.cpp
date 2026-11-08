#include "NativeRenderGateProxy.h"

#include <algorithm>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <new>

namespace ammod::audio_v2 {

namespace {

constexpr REFERENCE_TIME kReferenceTimePerSecond = 10000000;

// New graphs see a stable default software terminal. A local graph may have
// cached the physical mix format before injection, so accept valid stereo
// float32 rates too. This is only Apple's software scheduler: ACv2 opens the
// physical device separately at the captured source rate.
constexpr UINT32 kVirtualSampleRate = 384000u;
constexpr WORD kVirtualChannels = 2u;
constexpr WORD kVirtualBitsPerSample = 32u;
constexpr WORD kVirtualBlockAlign = 8u;
constexpr REFERENCE_TIME kVirtualEnginePeriod = 100000; // 10 ms
constexpr REFERENCE_TIME kVirtualBufferDuration = 220000; // 22 ms

UINT32 FramesForDuration(UINT32 sampleRate, REFERENCE_TIME duration) noexcept {
    if (sampleRate == 0 || duration <= 0) return 0;
    const auto frames =
        (static_cast<std::uint64_t>(sampleRate) * static_cast<std::uint64_t>(duration) +
         static_cast<std::uint64_t>(kReferenceTimePerSecond) - 1u) /
        static_cast<std::uint64_t>(kReferenceTimePerSecond);
    return static_cast<UINT32>(std::max<std::uint64_t>(1u, frames));
}

UINT32 VirtualQuantumFrames(UINT32 rate = kVirtualSampleRate) noexcept {
    return FramesForDuration(rate, kVirtualEnginePeriod);
}

UINT32 VirtualBufferFrames(UINT32 rate = kVirtualSampleRate) noexcept {
    return FramesForDuration(rate, kVirtualBufferDuration);
}

WAVEFORMATEXTENSIBLE VirtualMixFormat(UINT32 rate = kVirtualSampleRate) noexcept {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = kVirtualChannels;
    format.Format.nSamplesPerSec = rate;
    format.Format.wBitsPerSample = kVirtualBitsPerSample;
    format.Format.nBlockAlign = kVirtualBlockAlign;
    format.Format.nAvgBytesPerSec = rate * kVirtualBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = kVirtualBitsPerSample;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

HRESULT AllocateVirtualMixFormat(WAVEFORMATEX** format,
                                UINT32 rate = kVirtualSampleRate) noexcept {
    if (!format) return E_POINTER;
    *format = nullptr;
    auto* allocated = static_cast<WAVEFORMATEXTENSIBLE*>(
        CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!allocated) return E_OUTOFMEMORY;
    *allocated = VirtualMixFormat(rate);
    *format = &allocated->Format;
    return S_OK;
}

bool IsVirtualMixFormat(const WAVEFORMATEX* format) noexcept {
    if (!format || format->nChannels != kVirtualChannels ||
        format->nSamplesPerSec < 8000u || format->nSamplesPerSec > 768000u ||
        format->wBitsPerSample != kVirtualBitsPerSample ||
        format->nBlockAlign != kVirtualBlockAlign ||
        format->nAvgBytesPerSec != format->nSamplesPerSec * kVirtualBlockAlign) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return extensible->Samples.wValidBitsPerSample == kVirtualBitsPerSample &&
        (extensible->dwChannelMask == 0 ||
         extensible->dwChannelMask == (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) &&
        extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

bool IsAppleSharedGraphClient(AUDCLNT_SHAREMODE shareMode,
                              const WAVEFORMATEX* format) noexcept {
    return shareMode == AUDCLNT_SHAREMODE_SHARED && IsVirtualMixFormat(format);
}

class PumpAudioClock final : public IAudioClock {
public:
    explicit PumpAudioClock(UINT32 sampleRate) noexcept : sampleRate_(sampleRate) {
        QueryPerformanceFrequency(&frequency_);
    }

    void Start() noexcept {
        std::lock_guard lock(mutex_);
        if (!running_) { QueryPerformanceCounter(&origin_); running_ = true; }
    }
    void Stop() noexcept {
        std::lock_guard lock(mutex_);
        if (running_) {
            LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
            elapsedTicks_ += std::max<LONGLONG>(0, now.QuadPart - origin_.QuadPart);
            running_ = false;
        }
    }
    void Reset() noexcept {
        std::lock_guard lock(mutex_);
        elapsedTicks_ = 0; running_ = false;
    }
    static UINT64 Scale(UINT64 ticks, UINT64 numerator, UINT64 denominator) noexcept {
        return (ticks / denominator) * numerator + ((ticks % denominator) * numerator) / denominator;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClock)) {
            *object = static_cast<IAudioClock*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override {
        if (!frequency) return E_POINTER;
        *frequency = sampleRate_;
        return sampleRate_ != 0 ? S_OK : AUDCLNT_E_NOT_INITIALIZED;
    }

    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpcPosition) override {
        if (!position) return E_POINTER;
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now) || frequency_.QuadPart <= 0) {
            *position = 0;
            if (qpcPosition) *qpcPosition = 0;
            return S_OK;
        }
        std::lock_guard lock(mutex_);
        const auto elapsed = elapsedTicks_ + (running_
            ? std::max<LONGLONG>(0, now.QuadPart - origin_.QuadPart) : 0);
        if (sampleRate_ == 0) {
            *position = 0;
            if (qpcPosition) *qpcPosition = 0;
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        *position = Scale(elapsed, sampleRate_, frequency_.QuadPart);
        if (qpcPosition) {
            *qpcPosition = Scale(now.QuadPart, 10000000ULL, frequency_.QuadPart);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override {
        if (!characteristics) return E_POINTER;
        // The SDK used by the Apple Music package does not expose a named
        // fixed-clock characteristic; zero is the documented neutral value
        // and the graph only consumes the position/frequency pair here.
        *characteristics = 0;
        return S_OK;
    }

private:
    ~PumpAudioClock() = default;

    UINT32 sampleRate_{};
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER origin_{};
    std::mutex mutex_;
    UINT64 elapsedTicks_{};
    bool running_{};
    std::atomic<ULONG> references_{1};
};

class PumpStreamVolume final : public IAudioStreamVolume {
public:
    explicit PumpStreamVolume(std::shared_ptr<NativeRenderGateState> state) noexcept
        : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioStreamVolume)) {
            *object = static_cast<IAudioStreamVolume*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* channelCount) override {
        if (!channelCount) return E_POINTER;
        *channelCount = 2;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 channel, const float volume) override {
        return state_->SetChannelVolume(channel, volume);
    }

    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 channel, float* volume) override {
        return state_->GetChannelVolume(channel, volume);
    }

    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 channelCount,
                                             const float* volumes) override {
        return state_->SetAllVolumes(channelCount, volumes);
    }

    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 channelCount, float* volumes) override {
        return state_->GetAllVolumes(channelCount, volumes);
    }

private:
    ~PumpStreamVolume() = default;

    std::shared_ptr<NativeRenderGateState> state_;
    std::atomic<ULONG> references_{1};
};

class PumpSessionControl final : public IAudioSessionControl2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionControl2)) {
            *object = static_cast<IAudioSessionControl2*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(IAudioSessionControl)) {
            *object = static_cast<IAudioSessionControl*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetState(AudioSessionState* state) override {
        if (!state) return E_POINTER;
        *state = AudioSessionStateActive;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR* name) override {
        return CopyString(name, L"Apple Music");
    }

    HRESULT STDMETHODCALLTYPE SetDisplayName(LPCWSTR, LPCGUID) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetIconPath(LPWSTR* path) override {
        return CopyString(path, L"");
    }

    HRESULT STDMETHODCALLTYPE SetIconPath(LPCWSTR, LPCGUID) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetGroupingParam(GUID* grouping) override {
        if (!grouping) return E_POINTER;
        *grouping = GUID_NULL;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetGroupingParam(LPCGUID, LPCGUID) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE RegisterAudioSessionNotification(
        IAudioSessionEvents*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE UnregisterAudioSessionNotification(
        IAudioSessionEvents*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetSessionIdentifier(LPWSTR* identifier) override {
        return CopyString(identifier, L"AMExclusive/pump-session");
    }

    HRESULT STDMETHODCALLTYPE GetSessionInstanceIdentifier(LPWSTR* identifier) override {
        return CopyString(identifier, L"AMExclusive/pump-session-instance");
    }

    HRESULT STDMETHODCALLTYPE GetProcessId(DWORD* processId) override {
        if (!processId) return E_POINTER;
        *processId = GetCurrentProcessId();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsSystemSoundsSession() override { return S_FALSE; }

    HRESULT STDMETHODCALLTYPE SetDuckingPreference(BOOL) override { return S_OK; }

private:
    static HRESULT CopyString(LPWSTR* destination, LPCWSTR value) noexcept {
        if (!destination) return E_POINTER;
        *destination = nullptr;
        const auto length = value ? wcslen(value) : 0u;
        auto* copy = static_cast<LPWSTR>(CoTaskMemAlloc((length + 1u) * sizeof(wchar_t)));
        if (!copy) return E_OUTOFMEMORY;
        if (length) std::memcpy(copy, value, length * sizeof(wchar_t));
        copy[length] = L'\0';
        *destination = copy;
        return S_OK;
    }

    ~PumpSessionControl() = default;

    std::atomic<ULONG> references_{1};
};

} // namespace

NativeRenderGateState::~NativeRenderGateState() {
    if (sharedClient_) sharedClient_->Stop();
    if (sharedVolume_) sharedVolume_->Release();
    if (sharedRender_) sharedRender_->Release();
    if (sharedClient_) sharedClient_->Release();
}

HRESULT NativeRenderGateState::ConfigureShared(const std::wstring& endpoint,
                                              const WAVEFORMATEX* format) noexcept {
    if (!format || sizeof(WAVEFORMATEX) + format->cbSize > sizeof(sharedFormat_)) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    try { sharedEndpoint_ = endpoint; } catch (...) { return E_OUTOFMEMORY; }
    std::memset(&sharedFormat_, 0, sizeof(sharedFormat_));
    std::memcpy(&sharedFormat_, format, sizeof(WAVEFORMATEX) + format->cbSize);
    return S_OK;
}

HRESULT NativeRenderGateState::OpenSharedLocked() noexcept {
    if (sharedClient_) return S_OK;
    if (!sharedFormat_.Format.nSamplesPerSec) return AUDCLNT_E_NOT_INITIALIZED;
    // The existing Apple graph retains its negotiated rate and software event
    // cadence. Windows performs its normal shared-mode conversion after Off.
    ScopedNativeProxyBypass bypass;
    IMMDeviceEnumerator* enumerator{};
    IMMDevice* device{};
    IAudioClient* client{};
    IAudioRenderClient* render{};
    IAudioStreamVolume* volume{};
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) hr = sharedEndpoint_.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
        : enumerator->GetDevice(sharedEndpoint_.c_str(), &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
        nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
        AUDCLNT_STREAMFLAGS_NOPERSIST, kVirtualBufferDuration, 0, &sharedFormat_.Format, nullptr);
    UINT32 capacity{};
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&capacity);
    if (SUCCEEDED(hr) && !capacity) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioStreamVolume), reinterpret_cast<void**>(&volume));
    if (SUCCEEDED(hr)) hr = volume->SetAllVolumes(static_cast<UINT32>(volumes_.size()), volumes_.data());
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (FAILED(hr)) {
        if (volume) volume->Release();
        if (render) render->Release();
        if (client) client->Release();
        return hr;
    }
    sharedClient_ = client;
    sharedRender_ = render;
    sharedVolume_ = volume;
    sharedBufferFrames_ = capacity;
    return S_OK;
}

HRESULT NativeRenderGateState::SharedPadding(UINT32 virtualCapacity, UINT32* frames) noexcept {
    if (!frames) return E_POINTER;
    std::lock_guard lock(mutex_);
    HRESULT hr = OpenSharedLocked();
    UINT32 padding{};
    if (SUCCEEDED(hr)) hr = sharedClient_->GetCurrentPadding(&padding);
    if (FAILED(hr)) return hr;
    const auto available = sharedBufferFrames_ > padding ? sharedBufferFrames_ - padding : 0u;
    // Apple's cached capacity belongs to the software client. Translate free
    // space instead of exposing a differently sized Windows endpoint buffer.
    *frames = virtualCapacity - std::min(virtualCapacity, available);
    return S_OK;
}

HRESULT NativeRenderGateState::SharedGetBuffer(UINT32 frames, BYTE** data) noexcept {
    if (!data) return E_POINTER;
    std::lock_guard lock(mutex_);
    const HRESULT hr = OpenSharedLocked();
    return SUCCEEDED(hr) ? sharedRender_->GetBuffer(frames, data) : hr;
}

HRESULT NativeRenderGateState::SharedReleaseBuffer(UINT32 frames, DWORD flags) noexcept {
    std::lock_guard lock(mutex_);
    if (!sharedRender_) return AUDCLNT_E_NOT_INITIALIZED;
    HRESULT hr = sharedRender_->ReleaseBuffer(frames, flags);
    if (SUCCEEDED(hr) && frames) sharedHasData_ = true;
    if (SUCCEEDED(hr) && sharedHasData_ && startedWanted_ && !sharedStarted_) {
        hr = sharedClient_->Start();
        if (SUCCEEDED(hr)) sharedStarted_ = true;
    }
    return hr;
}

HRESULT NativeRenderGateState::SetStarted(bool started) noexcept {
    std::lock_guard lock(mutex_);
    if (sharedClient_ && started != sharedStarted_ && (!started || sharedHasData_)) {
        const HRESULT hr = started ? sharedClient_->Start() : sharedClient_->Stop();
        if (FAILED(hr)) return hr;
        sharedStarted_ = started;
    }
    startedWanted_ = started;
    return S_OK;
}

HRESULT NativeRenderGateState::ResetShared() noexcept {
    std::lock_guard lock(mutex_);
    if (!sharedClient_) return S_OK;
    const HRESULT hr = sharedClient_->Reset();
    if (SUCCEEDED(hr)) sharedHasData_ = false;
    return hr;
}

HRESULT NativeRenderGateState::SetChannelVolume(UINT32 channel, float volume) noexcept {
    if (channel >= volumes_.size() || !(volume >= 0.0f && volume <= 1.0f)) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    if (sharedVolume_) {
        const HRESULT hr = sharedVolume_->SetChannelVolume(channel, volume);
        if (FAILED(hr)) return hr;
    }
    volumes_[channel] = volume;
    return S_OK;
}

HRESULT NativeRenderGateState::GetChannelVolume(UINT32 channel, float* volume) noexcept {
    if (!volume) return E_POINTER;
    if (channel >= volumes_.size()) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    *volume = volumes_[channel];
    return S_OK;
}

HRESULT NativeRenderGateState::SetAllVolumes(UINT32 count, const float* volumes) noexcept {
    if (!volumes) return E_POINTER;
    if (count != volumes_.size()) return E_INVALIDARG;
    for (UINT32 i = 0; i < count; ++i)
        if (!(volumes[i] >= 0.0f && volumes[i] <= 1.0f)) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    if (sharedVolume_) {
        const HRESULT hr = sharedVolume_->SetAllVolumes(count, volumes);
        if (FAILED(hr)) return hr;
    }
    std::copy_n(volumes, count, volumes_.begin());
    return S_OK;
}

HRESULT NativeRenderGateState::GetAllVolumes(UINT32 count, float* volumes) noexcept {
    if (!volumes) return E_POINTER;
    if (count != volumes_.size()) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    std::copy_n(volumes_.begin(), count, volumes);
    return S_OK;
}

void NativeRenderGateState::Add(NativeRenderGateProxy* proxy) noexcept {
    if (!proxy) return;
    std::lock_guard lock(mutex_);
    proxies_.push_back(proxy);
}

void NativeRenderGateState::Remove(NativeRenderGateProxy* proxy) noexcept {
    if (!proxy) return;
    std::lock_guard lock(mutex_);
    const auto it = std::find(proxies_.begin(), proxies_.end(), proxy);
    if (it != proxies_.end()) proxies_.erase(it);
}

void NativeRenderGateState::DetachAll() noexcept {
    std::lock_guard lock(mutex_);
    for (auto* proxy : proxies_) {
        if (proxy) proxy->DetachInner();
    }
}

void NativeRenderGateState::EnableShadowAll() noexcept {
    std::lock_guard lock(mutex_);
    for (auto* proxy : proxies_) {
        if (proxy) proxy->EnableShadowMode();
    }
}

void NativeRenderGateProxy::DetachInner() noexcept {
    std::unique_lock lock(innerMutex_);
    if (!inner_) {
        acquiredData_ = nullptr;
        acquiredFrames_ = 0;
        shadowAcquired_ = false;
        return;
    }
    // The handoff callback can run on a control thread while Apple's render
    // thread is between GetBuffer and ReleaseBuffer. Divert new calls to the
    // private shadow surface, wait for the one real endpoint packet to return,
    // and only then release the inner render service. Releasing it earlier
    // makes the next ReleaseBuffer land on a dead ownership state and can make
    // AMPLibraryAgent fast-fail with STATUS_STACK_BUFFER_OVERRUN.
    handoffPending_ = true;
    drainCv_.wait(lock, [this] { return !nativeBufferInFlight_; });
    if (acquiredData_ && acquiredFrames_ != 0) {
        // The native render thread can be between GetBuffer and ReleaseBuffer
        // while the handoff is requested. Close that outstanding packet before
        // dropping the service reference so the endpoint has no live owner.
        (void)inner_->ReleaseBuffer(acquiredFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    acquiredData_ = nullptr;
    acquiredFrames_ = 0;
    shadowAcquired_ = false;
    inner_->Release();
    inner_ = nullptr;
    shadowMode_ = true;
    handoffPending_ = false;
}

void NativeRenderGateProxy::EnableShadowMode() noexcept {
    std::lock_guard lock(innerMutex_);
    shadowMode_ = true;
    handoffPending_ = false;
    shadowAcquired_ = false;
    acquiredData_ = nullptr;
    acquiredFrames_ = 0;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClient)) {
        *object = static_cast<IAudioClient*>(this);
    } else if (iid == __uuidof(IAudioClient2) && supports2_) {
        *object = static_cast<IAudioClient2*>(this);
    } else if (iid == __uuidof(IAudioClient3) && supports3_) {
        *object = static_cast<IAudioClient3*>(this);
    } else {
        std::lock_guard lock(innerMutex_);
        return inner_ ? inner_->QueryInterface(iid, object) : E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE NativeAudioClientProxy::Release() {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Initialize(
    AUDCLNT_SHAREMODE shareMode, DWORD flags, REFERENCE_TIME duration,
    REFERENCE_TIME periodicity, const WAVEFORMATEX* format, LPCGUID sessionGuid) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (shareMode == AUDCLNT_SHAREMODE_SHARED) {
            // Shared mode is a pure AME software terminal. Never initialize or
            // consult the physical Windows shared endpoint from this path.
            if (!IsAppleSharedGraphClient(shareMode, format)) {
                result = AUDCLNT_E_UNSUPPORTED_FORMAT;
            } else {
                pumpMode_ = true;
                if (pumpClock_) {
                    pumpClock_->Release();
                    pumpClock_ = nullptr;
                }
                pumpSampleRate_ = format->nSamplesPerSec;
                pumpClock_ = new (std::nothrow) PumpAudioClock(pumpSampleRate_);
                if (!pumpClock_) return E_OUTOFMEMORY;
                pumpQuantumFrames_ = VirtualQuantumFrames(pumpSampleRate_);
                pumpBufferFrames_ = VirtualBufferFrames(pumpSampleRate_);
                blockAlign_.store(kVirtualBlockAlign, std::memory_order_release);
                result = renderState_->ConfigureShared(endpoint_, format);
            }
        } else if (inner_) {
            result = inner_->Initialize(shareMode, flags, duration, periodicity,
                                        format, sessionGuid);
        } else if (pumpMode_) {
            result = S_OK;
        }
    }
    if (SUCCEEDED(result) && format) {
        blockAlign_.store(format->nBlockAlign, std::memory_order_release);
    }
    if (callbacks_.onInitialize) {
        callbacks_.onInitialize(callbacks_.context, this, endpoint_.c_str(), shareMode,
                                 flags, format, result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetBufferSize(UINT32* frames) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (!frames) result = E_POINTER;
            else {
                *frames = pumpBufferFrames_;
                result = pumpBufferFrames_ != 0 ? S_OK : AUDCLNT_E_NOT_INITIALIZED;
            }
        } else if (inner_) {
            result = inner_->GetBufferSize(frames);
        }
    }
    Trace(L"GetBufferSize", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetStreamLatency(REFERENCE_TIME* latency) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (!latency) result = E_POINTER;
            else {
                *latency = kVirtualBufferDuration;
                result = S_OK;
            }
        } else {
            result = inner_ ? inner_->GetStreamLatency(latency) : AUDCLNT_E_NOT_INITIALIZED;
        }
    }
    Trace(L"GetStreamLatency", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetCurrentPadding(UINT32* frames) {
    if (TerminalInvalidated()) return renderState_->SharedPadding(pumpBufferFrames_, frames);
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (!frames) result = E_POINTER;
            else {
                // Expose one fixed virtual-engine quantum as free space after
                // Start. This contract is intentionally independent of the
                // physical endpoint's Windows shared-mode format.
                *frames = pumpStarted_ && pumpBufferFrames_ > pumpQuantumFrames_
                    ? pumpBufferFrames_ - pumpQuantumFrames_
                    : 0u;
                result = pumpBufferFrames_ != 0 && pumpQuantumFrames_ != 0
                    ? S_OK : AUDCLNT_E_NOT_INITIALIZED;
            }
        } else if (inner_) {
            result = inner_->GetCurrentPadding(frames);
        }
    }
    Trace(L"GetCurrentPadding", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::IsFormatSupported(
    AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* format, WAVEFORMATEX** closest) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (mode == AUDCLNT_SHAREMODE_SHARED) {
            if (!format) {
                result = E_POINTER;
            } else if (IsVirtualMixFormat(format)) {
                if (closest) *closest = nullptr;
                result = S_OK;
            } else if (closest) {
                result = AllocateVirtualMixFormat(closest);
                if (SUCCEEDED(result)) result = S_FALSE;
            } else {
                result = S_FALSE;
            }
        } else {
            result = inner_ ? inner_->IsFormatSupported(mode, format, closest)
                            : AUDCLNT_E_NOT_INITIALIZED;
        }
    }
    Trace(L"IsFormatSupported", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetMixFormat(WAVEFORMATEX** format) {
    // GetMixFormat is the earliest point where the Windows shared endpoint used
    // to leak into Apple's graph. Always expose AME's fixed software terminal.
    const HRESULT result = AllocateVirtualMixFormat(format);
    Trace(L"GetMixFormat", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetDevicePeriod(
    REFERENCE_TIME* defaultPeriod, REFERENCE_TIME* minimumPeriod) {
    HRESULT result = S_OK;
    if (!defaultPeriod && !minimumPeriod) {
        result = E_POINTER;
    } else {
        if (defaultPeriod) *defaultPeriod = kVirtualEnginePeriod;
        if (minimumPeriod) *minimumPeriod = kVirtualEnginePeriod;
    }
    Trace(L"GetDevicePeriod", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Start() {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (pumpStarted_) return AUDCLNT_E_NOT_STOPPED;
            result = renderState_->SetStarted(true);
            if (SUCCEEDED(result)) {
                pumpStarted_ = true;
                if (pumpClock_) static_cast<PumpAudioClock*>(pumpClock_)->Start();
            }
        }
        else if (inner_) result = inner_->Start();
    }
    if (SUCCEEDED(result)) StartPumpThread();
    if (callbacks_.onStart) callbacks_.onStart(callbacks_.context, this, result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Stop() {
    // Natural streaming EOF is not guaranteed to produce a final zero-packet
    // Fill before Apple asks the software-backed client to Stop. Give the
    // existing native scheduler a brief chance to finish its producer tail
    // while the pump is still alive. Explicit pause/seek/skip paths are
    // rejected by the runtime callback and return immediately.
    if (callbacks_.beforeStop) {
        (void)callbacks_.beforeStop(callbacks_.context, this);
    }

    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            pumpStarted_ = false;
            if (pumpClock_) static_cast<PumpAudioClock*>(pumpClock_)->Stop();
            result = S_OK;
        }
        else if (inner_) result = inner_->Stop();
    }
    if (pumpMode_) StopPumpThread();
    if (SUCCEEDED(result)) result = renderState_->SetStarted(false);
    if (callbacks_.onStop) callbacks_.onStop(callbacks_.context, this, result);
    Trace(L"Stop", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Reset() {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (pumpStarted_) return AUDCLNT_E_NOT_STOPPED;
            if (pumpClock_) static_cast<PumpAudioClock*>(pumpClock_)->Reset();
            result = S_OK;
        }
        else if (inner_) result = inner_->Reset();
    }
    if (SUCCEEDED(result)) result = renderState_->ResetShared();
    if (callbacks_.onReset) callbacks_.onReset(callbacks_.context, this, result);
    Trace(L"Reset", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::SetEventHandle(HANDLE eventHandle) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            pumpEvent_.store(eventHandle, std::memory_order_release);
            result = eventHandle ? S_OK : E_INVALIDARG;
        } else if (inner_) {
            result = inner_->SetEventHandle(eventHandle);
        }
    }
    Trace(L"SetEventHandle", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetService(REFIID iid, void** service) {
    if (!service) {
        TraceService(iid, E_POINTER);
        return E_POINTER;
    }
    *service = nullptr;
    const wchar_t* serviceName =
        iid == __uuidof(IAudioRenderClient) ? L"GetService[IAudioRenderClient]" :
        iid == __uuidof(IAudioClock) ? L"GetService[IAudioClock]" :
        iid == __uuidof(IAudioStreamVolume) ? L"GetService[IAudioStreamVolume]" :
        iid == __uuidof(IAudioSessionControl) ? L"GetService[IAudioSessionControl]" :
        iid == __uuidof(IAudioSessionControl2) ? L"GetService[IAudioSessionControl2]" :
        L"GetService[other]";
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    bool softwareRender = false;
    bool softwareClock = false;
    bool softwareSession = false;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_ && iid == __uuidof(IAudioRenderClient)) {
            result = S_OK;
            softwareRender = true;
        } else if (pumpMode_ && iid == __uuidof(IAudioClock)) {
            if (!pumpClock_ && pumpSampleRate_ != 0) {
                pumpClock_ = new (std::nothrow) PumpAudioClock(pumpSampleRate_);
            }
            if (pumpClock_) {
                pumpClock_->AddRef();
                *service = pumpClock_;
                result = S_OK;
                softwareClock = true;
            } else {
                result = E_OUTOFMEMORY;
            }
        } else if (pumpMode_ && iid == __uuidof(IAudioStreamVolume)) {
            if (!pumpVolume_) pumpVolume_ = new (std::nothrow) PumpStreamVolume(renderState_);
            if (pumpVolume_) {
                pumpVolume_->AddRef();
                *service = pumpVolume_;
                result = S_OK;
            } else {
                result = E_OUTOFMEMORY;
            }
        } else if (pumpMode_ && (iid == __uuidof(IAudioSessionControl) ||
                                 iid == __uuidof(IAudioSessionControl2))) {
            if (!pumpSession_) pumpSession_ = new (std::nothrow) PumpSessionControl();
            if (pumpSession_) {
                pumpSession_->AddRef();
                *service = iid == __uuidof(IAudioSessionControl2)
                    ? static_cast<IAudioSessionControl2*>(pumpSession_)
                    : static_cast<IAudioSessionControl*>(pumpSession_);
                result = S_OK;
                softwareSession = true;
            } else {
                result = E_OUTOFMEMORY;
            }
        } else if (inner_) {
            result = inner_->GetService(iid, service);
        }
    }
    if (softwareClock) {
        TraceService(iid, S_OK);
        Trace(serviceName, S_OK);
        return S_OK;
    }
    if (softwareSession) {
        TraceService(iid, S_OK);
        Trace(serviceName, S_OK);
        return S_OK;
    }
    if (softwareRender) {
        auto* proxy = new (std::nothrow) NativeRenderGateProxy(
            nullptr, callbacks_, this, blockAlign_.load(std::memory_order_acquire), renderState_, terminalEpoch_);
        if (!proxy) {
            TraceService(iid, E_OUTOFMEMORY);
            Trace(serviceName, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        proxy->EnableShadowMode();
        *service = static_cast<IAudioRenderClient*>(proxy);
        TraceService(iid, S_OK);
        Trace(serviceName, S_OK);
        return S_OK;
    }
    if (FAILED(result) || !*service || iid != __uuidof(IAudioRenderClient)) {
        TraceService(iid, result);
        Trace(serviceName, result);
        return result;
    }

    auto* render = static_cast<IAudioRenderClient*>(*service);
    auto* proxy = new (std::nothrow) NativeRenderGateProxy(
        render, callbacks_, this, blockAlign_.load(std::memory_order_acquire), renderState_, terminalEpoch_);
    if (!proxy) {
        render->Release();
        *service = nullptr;
        TraceService(iid, E_OUTOFMEMORY);
        Trace(serviceName, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    *service = static_cast<IAudioRenderClient*>(proxy);
    TraceService(iid, S_OK);
    Trace(serviceName, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::IsOffloadCapable(
    AUDIO_STREAM_CATEGORY category, BOOL* capable) {
    auto* client = Inner2();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->IsOffloadCapable(category, capable);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::SetClientProperties(
    const AudioClientProperties* properties) {
    auto* client = Inner2();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->SetClientProperties(properties);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetBufferSizeLimits(
    const WAVEFORMATEX* format, BOOL eventDriven, REFERENCE_TIME* minimum,
    REFERENCE_TIME* maximum) {
    if (IsVirtualMixFormat(format)) {
        if (!minimum || !maximum) return E_POINTER;
        (void)eventDriven;
        *minimum = kVirtualEnginePeriod;
        *maximum = kVirtualBufferDuration;
        return S_OK;
    }
    auto* client = Inner2();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->GetBufferSizeLimits(format, eventDriven, minimum, maximum);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetSharedModeEnginePeriod(
    const WAVEFORMATEX* format, UINT32* defaultFrames, UINT32* fundamentalFrames,
    UINT32* minimumFrames, UINT32* maximumFrames) {
    if (!format || !defaultFrames || !fundamentalFrames || !minimumFrames || !maximumFrames) {
        return E_POINTER;
    }
    if (!IsVirtualMixFormat(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    const auto quantum = VirtualQuantumFrames(format->nSamplesPerSec);
    *defaultFrames = quantum;
    *fundamentalFrames = quantum;
    *minimumFrames = quantum;
    *maximumFrames = quantum;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetCurrentSharedModeEnginePeriod(
    WAVEFORMATEX** format, UINT32* frames) {
    if (!format || !frames) return E_POINTER;
    std::lock_guard lock(innerMutex_);
    const auto rate = pumpMode_ ? pumpSampleRate_ : kVirtualSampleRate;
    const HRESULT result = AllocateVirtualMixFormat(format, rate);
    if (FAILED(result)) return result;
    *frames = VirtualQuantumFrames(rate);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::InitializeSharedAudioStream(
    DWORD flags, UINT32 periodFrames, const WAVEFORMATEX* format, LPCGUID sessionGuid) {
    if (!format) return E_POINTER;
    if (!IsVirtualMixFormat(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (periodFrames != VirtualQuantumFrames(format->nSamplesPerSec)) return AUDCLNT_E_INVALID_DEVICE_PERIOD;
    // Route IAudioClient3 shared initialization through the same software-only
    // contract as IAudioClient::Initialize. No physical shared client is touched.
    return Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kVirtualBufferDuration, 0,
                      format, sessionGuid);
}

IAudioClient2* NativeAudioClientProxy::Inner2() const noexcept {
    std::lock_guard lock(innerMutex_);
    if (!inner_ || !supports2_) return nullptr;
    IAudioClient2* client{};
    return SUCCEEDED(inner_->QueryInterface(
        __uuidof(IAudioClient2), reinterpret_cast<void**>(&client))) ? client : nullptr;
}

IAudioClient3* NativeAudioClientProxy::Inner3() const noexcept {
    std::lock_guard lock(innerMutex_);
    if (!inner_ || !supports3_) return nullptr;
    IAudioClient3* client{};
    return SUCCEEDED(inner_->QueryInterface(
        __uuidof(IAudioClient3), reinterpret_cast<void**>(&client))) ? client : nullptr;
}

void NativeAudioClientProxy::Trace(const wchar_t* method, HRESULT result) noexcept {
    if (callbacks_.onMethod) callbacks_.onMethod(callbacks_.context, this, method, result);
}

void NativeAudioClientProxy::TraceService(REFIID iid, HRESULT result) noexcept {
    if (callbacks_.onService) callbacks_.onService(callbacks_.context, this, iid, result);
}

NativeAudioClientProxy::~NativeAudioClientProxy() {
    StopPumpThread();
    IAudioClient* inner{};
    {
        std::lock_guard lock(innerMutex_);
        inner = inner_;
        inner_ = nullptr;
    }
    if (inner) inner->Release();
    if (pumpClock_) {
        pumpClock_->Release();
        pumpClock_ = nullptr;
    }
    if (pumpVolume_) {
        pumpVolume_->Release();
        pumpVolume_ = nullptr;
    }
    if (pumpSession_) {
        pumpSession_->Release();
        pumpSession_ = nullptr;
    }
}

DWORD WINAPI NativeAudioClientProxy::PumpThreadThunk(void* context) noexcept {
    auto* self = static_cast<NativeAudioClientProxy*>(context);
    if (!self) return 0;

    const HANDLE stop = self->pumpStopEvent_;
    if (!stop) return 0;

    DWORD mmcssIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssIndex);
    if (!mmcss) mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssIndex);
    if (mmcss) (void)AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) {
        WaitForSingleObject(stop, INFINITE);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        return 0;
    }

    const HANDLE waitHandles[] = {stop, timer};
    for (;;) {
        const auto requestedMs = self->callbacks_.pumpPeriodMs
            ? self->callbacks_.pumpPeriodMs(self->callbacks_.context)
            : 10u;
        const auto periodMs = std::clamp<std::uint32_t>(requestedMs, 8u, 12u);
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -static_cast<LONGLONG>(periodMs) * 10'000;
        if (!SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)) break;

        const DWORD waitResult = WaitForMultipleObjects(
            ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult != WAIT_OBJECT_0 + 1) break;
        const auto eventHandle = self->pumpEvent_.load(std::memory_order_acquire);
        if (eventHandle) SetEvent(eventHandle);
    }

    CancelWaitableTimer(timer);
    CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return 0;
}

void NativeAudioClientProxy::StartPumpThread() noexcept {
    std::lock_guard lock(innerMutex_);
    if (!pumpMode_ || pumpThread_ || !pumpEvent_.load(std::memory_order_acquire)) return;
    pumpStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pumpStopEvent_) return;
    pumpThread_ = CreateThread(nullptr, 0, &NativeAudioClientProxy::PumpThreadThunk,
                               this, 0, nullptr);
    if (!pumpThread_) {
        CloseHandle(pumpStopEvent_);
        pumpStopEvent_ = nullptr;
    }
}

void NativeAudioClientProxy::StopPumpThread() noexcept {
    HANDLE stop{};
    HANDLE thread{};
    {
        std::lock_guard lock(innerMutex_);
        stop = pumpStopEvent_;
        thread = pumpThread_;
    }
    if (stop) SetEvent(stop);
    if (thread) WaitForSingleObject(thread, INFINITE);
    {
        std::lock_guard lock(innerMutex_);
        if (pumpThread_ == thread) pumpThread_ = nullptr;
        if (pumpStopEvent_ == stop) pumpStopEvent_ = nullptr;
    }
    if (thread) CloseHandle(thread);
    if (stop) CloseHandle(stop);
}

HRESULT NativeAudioClientProxy::PrepareForExclusiveHandoff() noexcept {
    {
        std::lock_guard lock(innerMutex_);
        // Preserve the scheduler rate accepted by Initialize, including a
        // cached local graph format. Changing it here corrupts frame/clock
        // accounting even though the physical DAC uses an independent rate.
        if (!pumpMode_ || pumpSampleRate_ == 0 || pumpQuantumFrames_ == 0 ||
            pumpBufferFrames_ == 0) return AUDCLNT_E_NOT_INITIALIZED;
    }

    // The parent AudioUnit remains Started and the native client is not
    // Stop/Reset-ed here: those calls tear down the decoder's source clock.
    // Drop the inner client/service references while the proxy stays alive,
    // then expose a bounded no-endpoint render surface so the parent can keep
    // driving Apple's decoder without retaining the physical endpoint.
    renderState_->DetachAll();
    FinalizeExclusiveHandoff();
    {
        std::lock_guard lock(innerMutex_);
        pumpMode_ = true;
    }
    renderState_->EnableShadowAll();
    return S_OK;
}

void NativeAudioClientProxy::FinalizeExclusiveHandoff() noexcept {
    renderState_->DetachAll();
    IAudioClient* inner{};
    {
        std::lock_guard lock(innerMutex_);
        inner = inner_;
        inner_ = nullptr;
    }
    if (inner) inner->Release();
}

} // namespace ammod::audio_v2
