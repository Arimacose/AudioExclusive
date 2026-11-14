#include "audio_v2/QueuedPcmSource.h"
#include <atomic>
#include <thread>
#include <array>
#include <iostream>
using namespace ammod::audio_v2;
int main(){
    constexpr std::uint32_t blocks=12000,frames=256;
    PcmQueue queue(16);PcmFormat format{44100,2,16,16,PcmEncoding::SignedInteger,true,16};
    QueuedPcmSource source(queue,format,1);
    std::atomic<bool> producerDone{},consumerDone{},bad{};
    std::atomic<unsigned> reads{};
    std::thread producer([&]{std::array<float,frames*2> pcm;pcm.fill(0.25f);std::uint64_t sequence=0;
        for(std::uint32_t i=0;i<blocks;){if(queue.TryPush(format,1,sequence,std::uint64_t(i)*frames,pcm.data(),frames,false,false))++i;else std::this_thread::yield();}
        queue.SealEndOfStream();producerDone.store(true);
    });
    std::thread observer([&]{std::uint64_t previous=0;
        while(!consumerDone.load()){
            const auto stats=source.Stats();const auto available=source.AvailableFrames();
            if(stats.mappedFrames<previous||stats.mappedFrames>std::uint64_t(blocks)*frames||available>std::uint64_t(blocks)*frames||stats.continuityFailures||stats.mappingFailures)bad.store(true);
            previous=stats.mappedFrames;++reads;
        }
    });
    std::array<std::int32_t,200> packet{};std::uint64_t total=0;
    while(!source.Exhausted()){
        if(!source.CanProvide(100)){std::this_thread::yield();continue;}
        std::uint32_t written{};bool eos{};
        const auto hr=source.Fill(packet.data(),100,written,eos);
        if(FAILED(hr)){bad.store(true);break;}
        for(unsigned i=0;i<written*2;++i)if(packet[i]!=0x20000000)bad.store(true);
        total+=written;
    }
    consumerDone.store(true);producer.join();observer.join();
    if(bad.load()||!producerDone.load()||!reads.load()||total!=std::uint64_t(blocks)*frames||source.Stats().mappedFrames!=total||source.AvailableFrames()!=0){std::cerr<<"FAIL concurrent source snapshot/PCM accounting\n";return 1;}
    std::cout<<"PASS concurrent producer, partial-block consumer and control snapshots: "<<total<<" unchanged stereo frames; "<<reads.load()<<" snapshots\n";
}
