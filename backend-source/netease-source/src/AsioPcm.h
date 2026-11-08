#pragma once
#include <cstdint>
#include <cstring>
namespace nce {
inline unsigned AsioSampleBytes(long type) noexcept {
    const long base = type >= 16 ? type - 16 : type;
    if (base == 0) return 2;
    if (base == 1) return 3;
    if (base == 2 || base == 3 || (base >= 8 && base <= 11)) return 4;
    if (base == 4) return 8;
    return 0; // DSD and unknown types are deliberately unsupported.
}
inline void StoreAsioSample(void* target, long type, std::int32_t sample) noexcept {
    const bool little = type >= 16;
    const long base = little ? type - 16 : type;
    const unsigned bytes = AsioSampleBytes(type);
    std::uint64_t bits{};
    if (base == 3) { const float value = static_cast<float>(sample / 2147483648.0); std::uint32_t raw; std::memcpy(&raw,&value,4); bits=raw; }
    else if (base == 4) { const double value=sample/2147483648.0; std::memcpy(&bits,&value,8); }
    else {
        const unsigned valid = base == 0 ? 16 : base == 1 ? 24 : base >= 8 && base <= 11 ? 16 + (base-8)*2 + (base==11?2:0) : 32;
        bits=static_cast<std::uint32_t>(sample >> (32-valid));
    }
    auto* out=static_cast<unsigned char*>(target);
    for(unsigned i=0;i<bytes;++i) out[little?i:bytes-1-i]=static_cast<unsigned char>(bits>>(8*i));
}
}
