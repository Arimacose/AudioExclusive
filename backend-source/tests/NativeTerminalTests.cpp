#include "audio_v2/NativeRenderGateProxy.h"
#include <ksmedia.h>
#include <wrl/client.h>
#include <iostream>
#include <memory>
#include <stdexcept>

using ammod::audio_v2::NativeAudioClientProxy;
using Microsoft::WRL::ComPtr;

static void Require(bool ok, const char* detail) {
    if (!ok) throw std::runtime_error(detail);
}
using Proxy = std::unique_ptr<NativeAudioClientProxy, void(*)(NativeAudioClientProxy*)>;
static Proxy MakeProxy() {
    // There is deliberately no hardware client: success must be entirely
    // software, including on the cached 192 kHz format seen in Apple Music.
    return Proxy(new NativeAudioClientProxy(nullptr, {}, L"test-no-hardware"),
                 [](NativeAudioClientProxy* value) { value->Release(); });
}
static WAVEFORMATEXTENSIBLE Format(UINT32 rate, bool extensible) {
    WAVEFORMATEXTENSIBLE value{};
    value.Format = {static_cast<WORD>(extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_IEEE_FLOAT),
                    2, rate, rate * 8, 8, 32,
                    static_cast<WORD>(extensible ? 22 : 0)};
    value.Samples.wValidBitsPerSample = 32;
    value.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    value.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return value;
}
static UINT32 Frames(UINT32 rate, UINT32 milliseconds) {
    return (rate * milliseconds + 999) / 1000;
}
static void CheckRate(UINT32 rate, bool extensible) {
    auto proxy = MakeProxy();
    auto input = Format(rate, extensible);
    WAVEFORMATEX* advertised{};
    Require(proxy->GetMixFormat(&advertised) == S_OK, "default mix query");
    Require(advertised->nSamplesPerSec == 384000, "stable advertised default");
    CoTaskMemFree(advertised);
    Require(proxy->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &input.Format, nullptr) == S_OK,
            "cached stereo float format must be accepted");
    Require(proxy->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              220000, 0, &input.Format, nullptr) == S_OK, "software initialize");
    UINT32 capacity{};
    Require(proxy->GetBufferSize(&capacity) == S_OK && capacity == Frames(rate, 22),
            "buffer frame count must match negotiated rate");
    UINT32 quantum{};
    WAVEFORMATEX* current{};
    Require(proxy->GetCurrentSharedModeEnginePeriod(&current, &quantum) == S_OK,
            "current engine query");
    Require(current->nSamplesPerSec == rate && quantum == Frames(rate, 10),
            "current engine format must match initialization");
    CoTaskMemFree(current);
    ComPtr<IAudioClock> clock;
    Require(proxy->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(clock.GetAddressOf())) == S_OK,
            "software clock service");
    UINT64 frequency{};
    Require(clock->GetFrequency(&frequency) == S_OK && frequency == rate, "software clock frequency");
    Require(proxy->PrepareForExclusiveHandoff() == S_OK, "handoff");
    Require(proxy->GetBufferSize(&capacity) == S_OK && capacity == Frames(rate, 22),
            "handoff must preserve scheduler rate");
    Require(clock->GetFrequency(&frequency) == S_OK && frequency == rate, "handoff preserves clock");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "event creation");
    Require(proxy->SetEventHandle(event) == S_OK && proxy->Start() == S_OK, "software start");
    ComPtr<IAudioRenderClient> renderer;
    Require(proxy->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(renderer.GetAddressOf())) == S_OK,
            "software render service");
    for (int packet = 0; packet < 3; ++packet) {
        Require(WaitForSingleObject(event, 500) == WAIT_OBJECT_0, "render scheduler must advance");
        BYTE* data{};
        Require(renderer->GetBuffer(quantum, &data) == S_OK && data != nullptr, "software render buffer");
        Require(renderer->ReleaseBuffer(quantum, AUDCLNT_BUFFERFLAGS_SILENT) == S_OK, "software packet release");
    }
    Require(proxy->Stop() == S_OK && proxy->Reset() == S_OK, "software stop/reset");
    CloseHandle(event);
    auto client3 = MakeProxy();
    UINT32 defaultFrames{}, fundamental{}, minimum{}, maximum{};
    Require(client3->GetSharedModeEnginePeriod(&input.Format, &defaultFrames, &fundamental,
                                             &minimum, &maximum) == S_OK, "IAudioClient3 period query");
    Require(defaultFrames == Frames(rate, 10), "IAudioClient3 rate-scaled period");
    Require(client3->InitializeSharedAudioStream(0, defaultFrames + 1, &input.Format, nullptr) ==
            AUDCLNT_E_INVALID_DEVICE_PERIOD, "invalid period rejected");
    Require(client3->InitializeSharedAudioStream(0, defaultFrames, &input.Format, nullptr) == S_OK,
            "IAudioClient3 cached-format initialize");
}
int main() {
    try {
        // Start with the exact reported failure before the broader matrix.
        for (UINT32 rate : {192000u, 44100u, 48000u, 88200u, 96000u, 176400u,
                            384000u, 8000u, 768000u}) {
            CheckRate(rate, true);
            CheckRate(rate, false);
        }
        for (int variant = 0; variant < 8; ++variant) {
            auto input = Format(192000, true);
            switch (variant) {
                case 0: input.Format.nSamplesPerSec = 0; break;
                case 1: input.Format.nSamplesPerSec = 768001; break;
                case 2: input.Format.nChannels = 1; break;
                case 3: input.Format.wBitsPerSample = 24; break;
                case 4: input.Format.nAvgBytesPerSec++; break;
                case 5: input.Samples.wValidBitsPerSample = 24; break;
                case 6: input.Format.cbSize = 0; break;
                case 7: input.SubFormat = KSDATAFORMAT_SUBTYPE_PCM; break;
            }
            auto proxy = MakeProxy();
            Require(proxy->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 220000, 0,
                                      &input.Format, nullptr) == AUDCLNT_E_UNSUPPORTED_FORMAT,
                    "malformed/non-float format rejected");
        }
        std::cout << "PASS: 18 software format/lifecycle cases, 8 rejected formats; no physical endpoint opened\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
