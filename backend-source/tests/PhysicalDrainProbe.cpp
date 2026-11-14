#include <windows.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#define private public
#include "audio_v2/WasapiExclusiveSink.h"
#undef private
using namespace ammod::audio_v2;
struct ClockTrace final : IAudioClock {
    std::atomic<ULONG> refs{1};IAudioClock* inner;WasapiExclusiveSink* sink;std::atomic<unsigned> calls{};
    ClockTrace(IAudioClock* clock,WasapiExclusiveSink* output):inner(clock),sink(output){}
    ~ClockTrace(){inner->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* value) override{return inner->GetFrequency(value);}
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* value) override{return inner->GetCharacteristics(value);}
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* pos,UINT64* qpc) override{
        const auto hr=inner->GetPosition(pos,qpc);const auto n=++calls;
        if(n<=4||n%25==0||*pos>=sink->Stats().submittedFrames)
            std::cout<<"clockRead="<<n<<" tid="<<GetCurrentThreadId()<<" pos="<<*pos<<" submitted="<<sink->Stats().submittedFrames<<" hr="<<hr<<std::endl;
        return hr;
    }
};
struct FiniteSource final : IIntegerPcmSource {
    UINT32 remaining;UINT32 period;unsigned delay;mutable ULONGLONG ready{};
    FiniteSource(UINT32 rate,unsigned stall):remaining(rate/2+rate/150),period(rate/50),delay(stall){}
    bool CanProvide(UINT32) const noexcept override{
        if(delay&&remaining<period){if(!ready)ready=GetTickCount64()+delay;return GetTickCount64()>=ready;}return true;
    }
    bool Exhausted() const noexcept override{return remaining==0;}
    HRESULT Fill(std::int32_t* out,UINT32 cap,UINT32& written,bool& eos) noexcept override{
        written=std::min(cap,remaining);remaining-=written;eos=remaining==0;
        std::fill(out,out+written*2,0);return S_OK;
    }
};
int wmain(int argc,wchar_t** argv){
    if(argc<2){std::cout<<"Use --hardware-test or --check-exclusive, followed by an optional endpoint ID\n";return 2;}
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const std::wstring endpoint=argc>2?argv[2]:L"";
    if(std::wstring(argv[1])==L"--check-exclusive"){
        WasapiExclusiveSink sink;WasapiSinkConfig cfg{};cfg.format={48000,2,24,24,PcmEncoding::SignedInteger,true,24};cfg.periodFrames=960;cfg.endpointId=endpoint;
        const auto hr=sink.Open(cfg);std::cout<<"exclusiveOpen="<<std::hex<<static_cast<unsigned>(hr)<<std::endl;sink.Close();CoUninitialize();return FAILED(hr)?1:0;
    }
    if(std::wstring(argv[1])!=L"--hardware-test")return 2;
    bool passed=true;
    for(UINT32 rate:{48000u,44100u})for(unsigned wait:{0u,50u,250u}){
        FiniteSource source(rate,wait);WasapiExclusiveSink sink;WasapiSinkConfig cfg{};
        cfg.format={rate,2,24,24,PcmEncoding::SignedInteger,true,24};
        cfg.periodFrames=rate/50;
        cfg.endpointId=endpoint;
        auto hr=sink.Open(cfg);if(FAILED(hr)){std::cout<<"OPEN_FAIL "<<std::hex<<hr<<std::endl;return 1;}
        sink.clock_=new ClockTrace(sink.clock_,&sink);
        hr=sink.Start(source);if(FAILED(hr)){std::cout<<"START_FAIL "<<std::hex<<hr<<std::endl;return 2;}
        const auto deadline=GetTickCount64()+1700;
        while(!sink.EndOfStreamDrained()&&GetTickCount64()<deadline)Sleep(10);
        UINT64 pos{},frequency{};UINT32 padding{};
        const auto positionHr=sink.clock_->GetPosition(&pos,nullptr);
        const auto paddingHr=sink.client_->GetCurrentPadding(&padding);
        sink.clock_->GetFrequency(&frequency);
        std::cout<<"rate="<<rate<<" wait="<<wait<<" drained="<<sink.EndOfStreamDrained()
                 <<" submitted="<<sink.Stats().submittedFrames<<" position="<<pos<<" frequency="<<frequency
                 <<" padding="<<padding<<" positionResult="<<positionHr<<" paddingResult="<<paddingHr
                 <<" deviceDeadline="<<sink.Stats().committedClockFrames
                 <<" sourceWaits="<<sink.Stats().sourceWaits<<" state="<<int(sink.State())<<std::endl;
        passed=passed&&sink.EndOfStreamDrained();
        sink.Close();
    }
    for(UINT32 rate:{44100u,48000u}){
        FiniteSource source(rate,0);WasapiExclusiveSink sink;WasapiSinkConfig cfg{};
        cfg.format={rate,2,24,24,PcmEncoding::SignedInteger,true,24};cfg.periodFrames=rate/50;cfg.endpointId=endpoint;
        auto hr=sink.Open(cfg);if(SUCCEEDED(hr))hr=sink.Start(source);Sleep(100);
        if(SUCCEEDED(hr))hr=sink.PauseForRelease();
        WasapiExclusiveSink other;const auto released=other.Open(cfg);other.Close();
        if(SUCCEEDED(hr))hr=sink.ResumeAfterRelease(source);
        const auto deadline=GetTickCount64()+1700;
        while(SUCCEEDED(hr)&&!sink.EndOfStreamDrained()&&GetTickCount64()<deadline)Sleep(10);
        std::cout<<"pauseRate="<<rate<<" released="<<std::hex<<released<<" resume="<<hr<<std::dec<<" drained="<<sink.EndOfStreamDrained()<<std::endl;
        passed=passed&&SUCCEEDED(hr)&&SUCCEEDED(released)&&sink.EndOfStreamDrained();sink.Close();
    }
    CoUninitialize();return passed?0:1;
}
