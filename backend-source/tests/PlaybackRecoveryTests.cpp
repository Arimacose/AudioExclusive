#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#define private public
#include "audio_v2/AudioCoreRuntime.h"
#undef private

using namespace ammod::audio_v2;
void Check(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
int main() {
    try {
        auto runtime = std::make_unique<AudioCoreRuntime>();
        runtime->OnTransportPlaybackState(false);
        Check(!runtime->UiEnabled() && !runtime->userPauseIntent_, "observer does not enable an Off runtime");
        runtime->uiEnabled_.store(true);
        runtime->OnTransportPlaybackState(false);
        void* converter = reinterpret_cast<void*>(0x1000);
        void* unit = reinterpret_cast<void*>(0x2000);
        const ApplePcmFormatView source{0x616c6163u, 0, 0, 4096, 0, 2, 24, 48000};
        const ApplePcmFormatView destination{kAppleLinearPcm, kAppleFormatFlagIsFloat,
            8, 1, 8, 2, 32, 48000};
        runtime->OnConverterCreated(converter, source, destination, 1);
        runtime->OnAudioUnitInitialized(unit, converter, 48000, 2, 24, 1);
        Check(runtime->graphBound_ && runtime->userPauseIntent_, "paused intent remains paused across new graph binding");
        const auto originalGeneration = runtime->boundMediaGeneration_.load();
        runtime->OnTransportPlaybackState(true);
        Check(!runtime->userPauseIntent_ && runtime->boundMediaGeneration_.load() == originalGeneration,
            "explicit Playing clears stale pause without replacing a healthy generation");
        runtime->seekFence_ = true;
        runtime->latestSeekEpoch_ = 17;
        runtime->OnTransportPlaybackState(true);
        Check(runtime->seekFence_ && runtime->latestSeekEpoch_ == 17 && runtime->coordinator_.SinkState() == WasapiSinkState::Closed,
            "Playing heartbeat preserves seek fence and cannot open endpoint prematurely");
        std::array<float, 2 * kMaxBlockFrames> pcm{};
        std::uint64_t cursor = 0;
        while (runtime->coordinator_.PushP0Interleaved(cursor, pcm.data(), kMaxBlockFrames, false, false)) {
            cursor += kMaxBlockFrames;
        }
        Check(runtime->coordinator_.Phase() == AudioCoreGatePhase::Faulted && runtime->coordinator_.Stats().droppedFrames != 0,
            "real bounded queue overflow reproduces capture fault");
        runtime->OnTransportPlaybackState(false);
        Check(runtime->userPauseIntent_ && runtime->coordinator_.Phase() == AudioCoreGatePhase::Faulted,
            "Paused does not silently recover or start faulted playback");
        runtime->OnTransportPlaybackState(true);
        Check(runtime->graphBound_ && runtime->coordinator_.Phase() == AudioCoreGatePhase::Capturing &&
            runtime->boundMediaGeneration_.load() > originalGeneration && runtime->coordinator_.BufferedFrames() == 0 &&
            runtime->seekFence_, "Playing retires faulted queue and binds fresh generation while retaining seek fence");
        Check(runtime->coordinator_.PushP0Interleaved(0, pcm.data(), kMaxBlockFrames, false, false),
            "fresh generation accepts PCM after overflow recovery");
        runtime->SetUiEnabled(false);
        std::cout << "All playback recovery regression checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
