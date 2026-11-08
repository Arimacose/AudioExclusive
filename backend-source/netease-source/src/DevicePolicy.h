#pragma once
#include <algorithm>
#include <cstdint>
namespace nce {
// ASIO fixed, continuous, stepped and power-of-two buffer constraints.
inline long SelectAsioPeriod(long minimum,long maximum,long preferred,long step) noexcept {
    if(minimum<=0||maximum<minimum||maximum>262144||step < -1)return 0;
    if(minimum==maximum)return minimum;
    const long wanted=std::clamp(preferred,minimum,maximum);
    if(step==0)return wanted;
    if(step==-1){
        long selected=0;
        for(long p=1;p<=maximum;p*=2)if(p>=minimum){selected=p;if(p>=wanted)break;}
        return selected;
    }
    const auto count=(maximum-minimum)/step;
    const auto index=std::min<std::int64_t>(count,(static_cast<std::int64_t>(wanted)-minimum+step-1)/step);
    return static_cast<long>(minimum+index*step);
}
// Mutated on the ASIO callback thread only, reset while the driver is stopped.
struct DeviceClockProgress {
    bool seen{};std::uint64_t origin{},previous{};unsigned unchanged{};
    bool Update(std::uint64_t raw,std::uint64_t& relative,unsigned stallLimit=3) noexcept {
        if(!seen){origin=previous=raw;seen=true;unchanged=0;relative=0;return true;}
        if(raw<previous)return false;
        unchanged=raw==previous?unchanged+1:0;
        previous=raw;relative=raw-origin;
        return unchanged<stallLimit;
    }
};
}
