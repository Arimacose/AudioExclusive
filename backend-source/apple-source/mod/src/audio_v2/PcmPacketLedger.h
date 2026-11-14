#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ammod::audio_v2 {
// Render-thread writes use preallocated slots. A paused control thread reads
// them only after joining the render worker, using the actual device clock.
class PcmPacketLedger final {
    struct Packet {std::vector<std::int32_t> samples;std::uint64_t begin{};std::uint32_t frames{};};
    std::vector<Packet> packets_;
    std::uint64_t written_{};
    std::uint32_t period_{},channels_{};
public:
    void Initialize(std::uint32_t period,std::uint32_t channels,std::size_t count){
        packets_.clear();packets_.resize(count);period_=period;channels_=channels;written_=0;
        for(auto& packet:packets_)packet.samples.resize(static_cast<std::size_t>(period)*channels);
    }
    bool CanRecord(std::uint64_t played) const noexcept {
        if(packets_.empty())return false;
        const auto& packet=packets_[written_%packets_.size()];
        return packet.frames==0||packet.begin+packet.frames<=played;
    }
    void Record(const std::int32_t* samples,std::uint32_t frames,std::uint64_t deadline) noexcept {
        auto& packet=packets_[written_%packets_.size()];
        std::copy_n(samples,static_cast<std::size_t>(frames)*channels_,packet.samples.data());
        packet.begin=deadline-period_;packet.frames=frames;++written_;
    }
    void UndoLast() noexcept {if(written_){--written_;packets_[written_%packets_.size()].frames=0;}}
    std::vector<std::int32_t> Unplayed(std::uint64_t played) const {
        std::vector<std::int32_t> result;
        result.reserve(packets_.size()*period_*channels_);
        const auto first=written_>packets_.size()?written_-packets_.size():0;
        for(auto index=first;index<written_;++index){
            const auto& packet=packets_[index%packets_.size()];
            const auto skipped=played>packet.begin?std::min<std::uint64_t>(played-packet.begin,packet.frames):0;
            result.insert(result.end(),packet.samples.begin()+static_cast<std::size_t>(skipped)*channels_,
                          packet.samples.begin()+static_cast<std::size_t>(packet.frames)*channels_);
        }
        return result;
    }
    void Clear() noexcept {std::vector<Packet>().swap(packets_);written_=0;period_=channels_=0;}
};
}
