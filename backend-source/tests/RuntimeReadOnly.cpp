#include <windows.h>
#include <tlhelp32.h>
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
#include <iomanip>
#include <type_traits>
#include <cstring>
#define private public
#include "audio_v2/AudioCoreRuntime.h"
#undef private

using namespace ammod::audio_v2;
bool Read(HANDLE process, std::uintptr_t address, void* destination, std::size_t size) {
    SIZE_T copied = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), destination, size, &copied) && copied == size;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const DWORD targetPid = std::stoul(argv[1]);
    const std::uintptr_t runtimeRva = std::stoull(argv[2], nullptr, 0);
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, targetPid);
    if (modules == INVALID_HANDLE_VALUE) return 3;
    MODULEENTRY32W module{}; module.dwSize = sizeof(module);
    std::uintptr_t base = 0;
    if (Module32FirstW(modules, &module)) do {
        if (_wcsicmp(module.szModule, L"am-exclusive-hook.dll") == 0) {
            base = reinterpret_cast<std::uintptr_t>(module.modBaseAddr); break;
        }
    } while (Module32NextW(modules, &module));
    CloseHandle(modules);
    if (!base) return 4;
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
    if (!process) return 5;
    std::array<std::byte, sizeof(AudioCoreRuntime)> bytes{};
    if (!Read(process, base + runtimeRva, bytes.data(), bytes.size())) { CloseHandle(process); return 6; }
    auto number = [&](std::size_t offset, std::size_t size) {
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data() + offset, size);
        return value;
    };
    bool first = true;
    auto field = [&](const char* name, std::uint64_t value) {
        if (!first) std::cout << ",\n";
        first = false; std::cout << "  \"" << name << "\": " << value;
    };
    std::cout << "{\n";
    field("processId", targetPid); field("moduleBase", base); field("runtimeRva", runtimeRva); field("runtimeBytes", bytes.size());
#define RF(member) field(#member, number(offsetof(AudioCoreRuntime, member), sizeof(((AudioCoreRuntime*)nullptr)->member)))
    RF(running_); RF(uiEnabled_); RF(workerStop_); RF(hardwareBufferMs_);
    RF(boundConverter_); RF(nativeClient_); RF(boundMediaGeneration_);
    RF(graphUnit_); RF(graphOutputUnit_); RF(graphConverter_);
    RF(graphRate_); RF(graphChannels_); RF(graphSourceBits_); RF(graphObservationId_);
    RF(graphStarted_); RF(graphPaused_); RF(graphControlPending_); RF(userPauseIntent_);
    RF(seekFence_); RF(latestSeekEpoch_); RF(seekFenceReleaseTick_);
    RF(nativeInitialized_); RF(nativeStarted_); RF(graphBound_);
    RF(requiredPrebufferFrames_); RF(streamingProducerDrainPending_); RF(endOfStreamDrainPending_);
    RF(directSelectionArmEpoch_); RF(directSelectionArmGeneration_); RF(directSelectionSkipEpoch_);
    RF(nextMediaGeneration_); RF(nextTelemetryTick_);
#undef RF
    constexpr std::size_t coordinator = offsetof(AudioCoreRuntime, coordinator_);
    field("phase", number(coordinator + offsetof(AudioCoreCoordinator, gate_) + offsetof(AudioCoreGate, phase_), sizeof(std::atomic<AudioCoreGatePhase>)));
    field("capturedFrames", number(coordinator + offsetof(AudioCoreCoordinator, capturedFrames_), sizeof(std::atomic<std::uint64_t>)));
    field("capturedBlocks", number(coordinator + offsetof(AudioCoreCoordinator, capturedBlocks_), sizeof(std::atomic<std::uint64_t>)));
    field("droppedFrames", number(coordinator + offsetof(AudioCoreCoordinator, droppedFrames_), sizeof(std::atomic<std::uint64_t>)));
    constexpr std::size_t sink = coordinator + offsetof(AudioCoreCoordinator, sink_);
    field("sinkState", number(sink + offsetof(WasapiExclusiveSink, state_), sizeof(std::atomic<WasapiSinkState>)));
    field("submittedFrames", number(sink + offsetof(WasapiExclusiveSink, submittedFrames_), sizeof(std::atomic<std::uint64_t>)));
    field("committedClockFrames", number(sink + offsetof(WasapiExclusiveSink, committedClockFrames_), sizeof(std::atomic<std::uint64_t>)));
    field("sinkRate", number(sink + offsetof(WasapiExclusiveSink, config_) + offsetof(WasapiSinkConfig, format) + offsetof(PcmFormat, sampleRate), sizeof(std::uint32_t)));
    field("sinkClockFrequency", number(sink + offsetof(WasapiExclusiveSink, clockFrequency_), sizeof(UINT64)));
    field("sinkWaitingForSource", number(sink + offsetof(WasapiExclusiveSink, waitingForSource_), sizeof(std::atomic<bool>)));
    field("sinkEofSubmitted", number(sink + offsetof(WasapiExclusiveSink, endOfStreamSubmitted_), sizeof(std::atomic<bool>)));
    field("sinkEofDrained", number(sink + offsetof(WasapiExclusiveSink, endOfStreamDrained_), sizeof(std::atomic<bool>)));
    field("sinkRenderThread", number(sink + offsetof(WasapiExclusiveSink, renderThread_), sizeof(HANDLE)));
    const auto queueAddress = number(coordinator + offsetof(AudioCoreCoordinator, queue_), sizeof(void*));
    field("queueAddress", queueAddress);
    if (queueAddress) {
        std::size_t published = 0, consumed = 0;
        if (Read(process, queueAddress + offsetof(PcmQueue, publishedFrames_), &published, sizeof(published)) &&
            Read(process, queueAddress + offsetof(PcmQueue, consumedFrames_), &consumed, sizeof(consumed)))
            field("queuedFrames", published > consumed ? published - consumed : 0);
    }
    std::cout << "\n}\n";
    CloseHandle(process);
    return 0;
}
