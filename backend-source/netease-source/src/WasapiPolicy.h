#pragma once
#include <audioclient.h>
#include <array>
#include <cstdint>
namespace nce {
inline std::array<WAVEFORMATEXTENSIBLE,7> WasapiPcmCandidates(unsigned rate,WORD channels) noexcept {
    std::array<WAVEFORMATEXTENSIBLE,7> result{};
    constexpr WORD containers[]={32,32,24,16,32,24,16},valid[]={32,24,24,16,32,24,16};
    for(unsigned i=0;i<result.size();++i){auto& f=result[i];
        f.Format={static_cast<WORD>(i<4?WAVE_FORMAT_EXTENSIBLE:WAVE_FORMAT_PCM),channels,rate,
            rate*channels*(containers[i]/8),static_cast<WORD>(channels*(containers[i]/8)),containers[i],static_cast<WORD>(i<4?22:0)};
        f.Samples.wValidBitsPerSample=valid[i];f.dwChannelMask=channels==2?3:0;
        f.SubFormat={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    }return result;
}
inline void StoreWasapiInteger(unsigned char* out,WORD container,WORD valid,std::int32_t sample) noexcept {
    auto value=static_cast<std::uint32_t>(sample);
    if(valid<32)value&=~((std::uint32_t{1}<<(32-valid))-1);
    value>>=32-container;
    for(unsigned i=0;i<container/8u;++i)out[i]=static_cast<unsigned char>(value>>(8*i));
}
inline REFERENCE_TIME FramesPeriod(UINT32 frames,unsigned rate) noexcept {
    return static_cast<REFERENCE_TIME>((static_cast<std::uint64_t>(frames)*10000000+rate/2)/rate);
}
// Each attempt owns a fresh client. The caller supplies raw Initialize to avoid
// recursively entering the native hook; no Start or PCM submission occurs.
template<class Activate,class Initialize>
HRESULT ProbeExclusivePeriod(Activate activate,Initialize initialize,const WAVEFORMATEX* format,
    DWORD flags,REFERENCE_TIME requested,REFERENCE_TIME& selected) {
    selected=requested;
    for(unsigned attempt=0;attempt<3;++attempt){
        IAudioClient* client{};HRESULT result=activate(&client);
        if(FAILED(result))return result;if(!client)return E_POINTER;
        result=initialize(client,flags,selected,format);
        if(SUCCEEDED(result)){client->Release();return S_OK;}
        if(result!=AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED){client->Release();return result;}
        UINT32 frames{};HRESULT sizeResult=client->GetBufferSize(&frames);client->Release();
        if(FAILED(sizeResult)||!frames)return FAILED(sizeResult)?sizeResult:E_UNEXPECTED;
        const auto aligned=FramesPeriod(frames,format->nSamplesPerSec);
        if(aligned==selected)return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;
        selected=aligned;
    }return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;
}
}
