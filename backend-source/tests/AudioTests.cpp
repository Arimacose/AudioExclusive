#include "PacketClock.h"
#include "AsioPcm.h"
#include "AudioFormatPolicy.h"
#include "audio_v2/PcmTypes.h"
#include "audio_v2/LocalFloatIntegerizer.h"
#include <iostream>
#include <stdexcept>
#include <limits>
void check(bool v,const char* reason){if(!v)throw std::runtime_error(reason);}
int main(){try{
    for(unsigned rate:{44100u,48000u,96000u,192000u}){
        const unsigned packet=((rate/50+127)/128)*128;
        nce::PacketClock clock;clock.Submit(0,packet);
        for(std::uint64_t i=1;i<100000;++i){auto played=i*packet+(i%73);check(clock.Ready(played),"callback jitter accumulated into packet deadline");clock.Submit(played,packet);check(!clock.Ready(played),"duplicate packet accepted");}
        clock.Reset();check(clock.Ready(0),"reset did not clear clock");clock.Submit(0,0);check(clock.committed==0,"cancel advances clock");
        for(std::uint64_t frames=0;frames<rate;frames+=17){auto position=(frames*10000000+rate/2)/rate;check(nce::PacketClock::Frames(position,10000000,rate)==frames,"100ns clock frame conversion");}
        clock.Submit(0,packet);clock.Submit(9ull*packet+10,packet);check(clock.committed==10ull*packet,"late callback catch-up");
    }
    std::cout<<"PASS hardware clock: 44.1/48/96/192 kHz, 399996 jittered callbacks, reset and lateness\n";
    using namespace ammod::audio_v2;
    PcmFormat exact{48000,2,24,24,PcmEncoding::SignedInteger,true,32};check(!IsValidFormat(exact),"integer precision loss accepted");exact.allowQuantization=true;check(IsValidFormat(exact),"explicit local float 24-bit fallback rejected");
    auto candidates=ammod::audio::SelectBitPerfectFormatCandidates(32,true);check(candidates.count==3&&candidates.values[1].containerBits==24,"packed 24 fallback absent");
    for(int value=-8388608;value<8388607;value+=997){float f=static_cast<float>(value)/8388608.0f;std::int32_t canonical{};LocalFloatIntegerizerStats stats{};check(IntegerizeLocalFloatSample(f,canonical,stats),"float mapping failed");check(QuantizeCanonical(canonical,24)==static_cast<std::int64_t>(value)*256,"24-bit PCM changed through float path");}
    check(QuantizeCanonical(128,24)==0&&QuantizeCanonical(384,24)==512,"ties to even");check(QuantizeCanonical(-128,24)==0&&QuantizeCanonical(-384,24)==-512,"negative ties");check(QuantizeCanonical(INT32_MAX,24)==2147483392,"positive clipping wraps");check(QuantizeCanonical(INT32_MIN,24)==INT32_MIN,"negative clipping wraps");
    std::cout<<"PASS local float DAC negotiation and exact 24-bit sample lattice; rounding/clipping boundaries\n";
    unsigned char out[8]{};
    for(long type:{0l,1l,2l,3l,4l,8l,9l,10l,11l,16l,17l,18l,19l,20l,24l,25l,26l,27l}){check(nce::AsioSampleBytes(type)>0,"ASIO PCM type rejected");nce::StoreAsioSample(out,type,0);for(unsigned i=0;i<nce::AsioSampleBytes(type);++i)check(out[i]==0,"ASIO silence wrong");}
    nce::StoreAsioSample(out,17,0x12345678);check(out[0]==0x56&&out[1]==0x34&&out[2]==0x12,"ASIO packed24 little endian");
    nce::StoreAsioSample(out,1,0x12345678);check(out[2]==0x56&&out[1]==0x34&&out[0]==0x12,"ASIO packed24 big endian");
    nce::StoreAsioSample(out,27,INT32_MIN);check(out[0]==0&&out[1]==0&&out[2]==0x80&&out[3]==0xff,"ASIO int24 in int32 sign extension");
    nce::StoreAsioSample(out,19,1073741824);float f;memcpy(&f,out,4);check(f==0.5f,"ASIO float scale");check(!nce::AsioSampleBytes(32),"DSD mislabeled as PCM");
    std::cout<<"PASS ASIO 18 PCM layouts, byte order, sign extension and float scaling\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
