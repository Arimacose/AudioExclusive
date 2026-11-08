#pragma once
#include <algorithm>
#include <cstdint>

namespace nce {
// WASAPI's endpoint clock, not elapsed wall time, owns render consumption.
// One complete event packet may be submitted only after the preceding one
// has played. A late callback never creates catch-up writes into one packet.
struct PacketClock {
    std::uint64_t committed{};
    static std::uint64_t Frames(std::uint64_t position, std::uint64_t frequency,
                                std::uint32_t rate) noexcept {
        if (!frequency) return 0;
        return (position / frequency) * rate + ((position % frequency) * rate + frequency / 2) / frequency;
    }
    bool Ready(std::uint64_t played) const noexcept { return played >= committed; }
    void Submit(std::uint64_t played, std::uint32_t frames) noexcept {
        // Keep deadlines on the hardware packet grid. Anchoring to the late
        // callback's position adds scheduling jitter to every next deadline
        // and can skip alternate packets at 44.1 kHz.
        if (frames) committed = std::max(committed + frames, (played / frames + 1) * frames);
    }
    void Reset() noexcept { committed = 0; }
};
}
