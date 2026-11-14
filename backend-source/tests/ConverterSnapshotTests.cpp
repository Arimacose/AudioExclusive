#include "audio_v2/AudioCoreConverterRegistry.h"
#include <thread>
#include <atomic>
#include <iostream>
using namespace ammod::audio_v2;
int main(){
    AudioCoreConverterRegistry registry;
    ConverterRegistration a{};a.converter=reinterpret_cast<void*>(0x1234);a.mediaGeneration=1;
    a.outputFormat={44100,2,24,32,PcmEncoding::SignedInteger,true,16};a.sourceBitDepth=16;a.observationId=11;a.encodedFormat=0x616c6163;
    ConverterRegistration b=a;b.outputFormat.sampleRate=48000;b.outputFormat.sourceValidBits=24;b.sourceBitDepth=24;b.observationId=22;
    registry.Register(a);std::atomic<bool> done{},mixed{};std::atomic<unsigned> snapshots{};
    std::thread reader([&]{while(!done.load()){
        ConverterRegistrationSnapshot s{};if(!registry.Snapshot(a.converter,s))continue;
        const auto& r=s.registration;
        const bool isA=r.outputFormat.sampleRate==44100&&r.outputFormat.sourceValidBits==16&&r.sourceBitDepth==16&&r.observationId==11;
        const bool isB=r.outputFormat.sampleRate==48000&&r.outputFormat.sourceValidBits==24&&r.sourceBitDepth==24&&r.observationId==22;
        if(!isA&&!isB)mixed.store(true);++snapshots;
    }});
    for(unsigned i=0;i<300000;++i)registry.Register((i&1)?a:b);
    done.store(true);reader.join();
    if(mixed.load()||!snapshots.load()){std::cerr<<"FAIL same-handle registration reuse published a mixed format snapshot\n";return 1;}
    registry.Register(a);registry.Bind(a.converter,1);
    ConverterRegistrationSnapshot old{};registry.Snapshot(a.converter,old);
    registry.Register(b);registry.Bind(b.converter,1);
    std::uint64_t first{};bool discontinuity{};
    if(registry.ClaimFrames(a.converter,512,first,discontinuity,old.publication))return 2;
    ConverterRegistrationSnapshot current{};registry.Snapshot(b.converter,current);
    if(!registry.ClaimFrames(b.converter,512,first,discontinuity,current.publication)||first!=0||!discontinuity)return 3;
    std::cout<<"PASS same-handle converter reuse: "<<snapshots.load()<<" coherent snapshots across 300000 format registrations\n";
}
