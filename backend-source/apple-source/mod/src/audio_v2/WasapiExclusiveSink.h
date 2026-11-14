#pragma once

#include "PcmTypes.h"
#include "PcmPacketLedger.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ammod::audio_v2 {

// A decoder-side queue can be temporarily empty while Apple waits for a
// lossless network segment. This is a recoverable wait, not a format or
// endpoint failure. The sink keeps its exclusive client running without
// acquiring another endpoint buffer, keeps native output suppressed, and
// submits the next real packet when complete P0 data is available.
inline constexpr HRESULT kAudioSourceWouldBlock = static_cast<HRESULT>(0x88770001L);

class IIntegerPcmSource {
public:
    virtual ~IIntegerPcmSource() = default;

    // A sink must not acquire an endpoint buffer unless the source can fill
    // the complete period, or has reached an explicit end-of-stream tail.
    // This keeps a network wait from becoming fabricated output samples.
    virtual bool CanProvide(std::uint32_t capacityFrames) const noexcept {
        return capacityFrames != 0;
    }

    // Consumer-thread query: no further media remains, including a producer
    // seal arriving after the last full packet was submitted to the device.
    virtual bool Exhausted() const noexcept { return false; }

    // Approximate number of source frames that can be consumed immediately.
    // This is diagnostic only; the sink never drives the Apple graph from the
    // endpoint render thread.
    virtual std::size_t AvailableFrames() const noexcept { return SIZE_MAX; }

    // The sink always asks for a complete endpoint buffer. A source may return
    // fewer frames only when endOfStream is true. It may return
    // kAudioSourceWouldBlock when Apple is waiting for the next lossless
    // segment; the running endpoint and generation remain alive in that case.
    virtual HRESULT Fill(std::int32_t* destination,
                         std::uint32_t capacityFrames,
                         std::uint32_t& writtenFrames,
                         bool& endOfStream) noexcept = 0;

};

class ReplayPcmSource final : public IIntegerPcmSource {
    IIntegerPcmSource* live_{};std::vector<std::int32_t> pending_;std::size_t offset_{};std::uint32_t channels_{2};
    std::size_t Remaining() const noexcept{return (pending_.size()-offset_)/channels_;}
public:
    void Attach(IIntegerPcmSource& live,std::uint32_t channels) noexcept{live_=&live;channels_=channels;}
    void Prepend(std::vector<std::int32_t> prefix){prefix.insert(prefix.end(),pending_.begin()+offset_,pending_.end());pending_=std::move(prefix);offset_=0;}
    std::vector<std::int32_t> Take(){std::vector<std::int32_t> result(pending_.begin()+offset_,pending_.end());Clear();return result;}
    void Clear() noexcept{std::vector<std::int32_t>().swap(pending_);offset_=0;live_=nullptr;}
    bool CanProvide(std::uint32_t frames) const noexcept override{return Remaining()>=frames||(live_&&live_->CanProvide(static_cast<std::uint32_t>(frames-Remaining())));}
    bool Exhausted() const noexcept override{return Remaining()==0&&live_&&live_->Exhausted();}
    std::size_t AvailableFrames() const noexcept override{const auto count=live_?live_->AvailableFrames():0;return count==SIZE_MAX?SIZE_MAX:Remaining()+count;}
    HRESULT Fill(std::int32_t* destination,std::uint32_t capacity,std::uint32_t& written,bool& eos) noexcept override{
        written=0;eos=false;if(!destination||!capacity||!live_)return E_INVALIDARG;
        const auto replay=static_cast<std::uint32_t>(std::min<std::size_t>(Remaining(),capacity));
        if(replay){std::copy_n(pending_.data()+offset_,static_cast<std::size_t>(replay)*channels_,destination);offset_+=static_cast<std::size_t>(replay)*channels_;written=replay;}
        if(written==capacity){eos=Exhausted();return S_OK;}
        std::uint32_t count{};const auto hr=live_->Fill(destination+static_cast<std::size_t>(written)*channels_,capacity-written,count,eos);written+=count;return hr;
    }
};

// Lifecycle notifications are optional and diagnostic-only.  They are invoked
// on the thread that performs the corresponding sink operation and carry the
// HRESULT observed at the operation boundary.  The production hook uses them
// to build a QPC ownership timeline; tests leave the callback unset.
enum class WasapiSinkLifecycleEvent : std::uint8_t {
    OpenEnter,
    OpenExit,
    InitializeEnter,
    InitializeExit,
    StartEnter,
    StartExit,
    StopEnter,
    StopExit,
    CloseEnter,
    CloseExit,
};

using WasapiSinkLifecycleCallback = void (*)(
    void* context, WasapiSinkLifecycleEvent event, HRESULT result) noexcept;

struct WasapiSinkConfig final {
    PcmFormat format{};
    std::array<PcmFormat, kMaxFormatCandidates> formatCandidates{};
    std::uint32_t formatCandidateCount{};
    std::wstring endpointId;
    std::uint32_t periodFrames{};
    std::uint32_t alignmentRetries{2};
    // Non-owning producer notification. The coordinator owns this handle and
    // keeps it valid until the sink has stopped and closed.
    HANDLE sourceReadyEvent{};
    void* lifecycleContext{};
    WasapiSinkLifecycleCallback onLifecycle{};
};

struct WasapiSinkStats final {
    std::uint64_t submittedFrames{};
    std::uint64_t submittedBuffers{};
    std::uint64_t controlledSilenceFrames{};
    std::uint64_t sourceWaits{};
    std::uint64_t underruns{};
    std::uint64_t getBufferFailures{};
    std::uint64_t releaseBufferFailures{};
    HRESULT lastError{S_OK};
    std::uint64_t committedClockFrames{};
};

enum class WasapiSinkState : std::uint8_t {
    Closed,
    Open,
    Running,
    Faulted,
};

class WasapiExclusiveSink final {
public:
    WasapiExclusiveSink() noexcept = default;
    ~WasapiExclusiveSink();

    WasapiExclusiveSink(const WasapiExclusiveSink&) = delete;
    WasapiExclusiveSink& operator=(const WasapiExclusiveSink&) = delete;

    HRESULT Open(const WasapiSinkConfig& config) noexcept;
    HRESULT Start(IIntegerPcmSource& source) noexcept;
    void Stop() noexcept;
    void Close() noexcept;
    HRESULT PauseForRelease() noexcept;
    HRESULT ResumeAfterRelease(IIntegerPcmSource& source) noexcept;
    bool PausedForRelease() const noexcept{return pausedForRelease_;}

    WasapiSinkState State() const noexcept { return state_.load(std::memory_order_acquire); }
    WasapiSinkStats Stats() const noexcept;
    std::uint32_t BufferFrames() const noexcept { return bufferFrames_; }
    const PcmFormat& Format() const noexcept { return config_.format; }
    bool WaitingForSource() const noexcept {
        return waitingForSource_.load(std::memory_order_acquire);
    }
    bool EndOfStreamSubmitted() const noexcept {
        return endOfStreamSubmitted_.load(std::memory_order_acquire);
    }
    // The coordinator blocks the render thread from consuming a producer
    // notification while it pauses/resumes Apple's graph.  This closes the
    // small race where the source becomes ready between the worker's state
    // check and its graph-control call.
    void SetResumeBlocked(bool blocked) noexcept;
    bool EndOfStreamDrained() const noexcept {
        return endOfStreamDrained_.load(std::memory_order_acquire);
    }

private:
    static DWORD WINAPI RenderThreadThunk(void* context) noexcept;
    DWORD RenderThreadMain() noexcept;

    HRESULT ActivateClient() noexcept;
    HRESULT InitializeClient() noexcept;
    HRESULT RenderOneBuffer() noexcept;
    HRESULT CheckSubmittedAudioPlayed() noexcept;
    HRESULT ReadPlayedFrames(std::uint64_t& frames) noexcept;
    HRESULT CheckEndOfStreamDrain() noexcept;
    void RecordError(HRESULT error) noexcept;
    void NotifyLifecycle(WasapiSinkLifecycleEvent event, HRESULT result) noexcept;
    void ReleaseInterfaces() noexcept;

    WasapiSinkConfig config_{};
    WAVEFORMATEXTENSIBLE waveFormat_{};
    std::vector<std::int32_t> canonicalBuffer_;

    IMMDeviceEnumerator* enumerator_{};
    IMMDevice* device_{};
    IAudioClient* client_{};
    IAudioRenderClient* render_{};
    IAudioClock* clock_{};
    UINT64 clockFrequency_{};

    HANDLE shutdownEvent_{};
    HANDLE audioEvent_{};
    HANDLE renderThread_{};
    std::atomic<bool> stopRequested_{};
    std::atomic<bool> waitingForSource_{};
    std::atomic<bool> resumeBlocked_{};
    std::atomic<bool> endOfStreamSubmitted_{};
    std::atomic<bool> endOfStreamDrained_{};
    bool paused_{};
    bool physicalStarted_{};
    bool pausedForRelease_{};
    ReplayPcmSource replaySource_;
    PcmPacketLedger packetLedger_;
    std::atomic<std::uint64_t> committedClockFrames_{};
    std::uint64_t lastPlayedFrames_{};
    IIntegerPcmSource* source_{};
    std::uint32_t bufferFrames_{};
    CO_MTA_USAGE_COOKIE mtaCookie_{};

    std::atomic<WasapiSinkState> state_{WasapiSinkState::Closed};
    std::atomic<std::uint64_t> submittedFrames_{};
    std::atomic<std::uint64_t> submittedBuffers_{};
    std::atomic<std::uint64_t> controlledSilenceFrames_{};
    std::atomic<std::uint64_t> sourceWaits_{};
    std::atomic<std::uint64_t> underruns_{};
    std::atomic<std::uint64_t> getBufferFailures_{};
    std::atomic<std::uint64_t> releaseBufferFailures_{};
    std::atomic<LONG> lastError_{S_OK};
};

} // namespace ammod::audio_v2
