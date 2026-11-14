#include <windows.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#define private public
#include "audio_v2/WasapiExclusiveSink.h"
#undef private
#include "../apple-source/mod/src/audio_v2/WasapiExclusiveSink.cpp"
using namespace ammod::audio_v2;
static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct FakeEndpoint final : IAudioClient, IAudioClock, IAudioRenderClient {
    std::atomic<ULONG> refs{1};std::atomic<UINT64> position{};UINT64 frequency{};
    std::atomic<unsigned> stops{},starts{},releases{};
    HANDLE stopped{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    HANDLE started{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    HANDLE released{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    UINT32 frames{};std::vector<BYTE> buffer;
    ~FakeEndpoint(){CloseHandle(stopped);CloseHandle(started);CloseHandle(released);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,LPCGUID) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* value) override{*value=frames;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE,const WAVEFORMATEX*,WAVEFORMATEX**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*,REFERENCE_TIME*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Start() override{++starts;SetEvent(started);return S_OK;}
    HRESULT STDMETHODCALLTYPE Stop() override{++stops;SetEvent(stopped);return S_OK;}
    HRESULT STDMETHODCALLTYPE Reset() override{position.store(0);return S_OK;}
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetService(REFIID,void**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* value) override{*value=frequency;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* value,UINT64* qpc) override{*value=position.load();if(qpc)*qpc=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 count,BYTE** out) override{if(count!=frames)return E_INVALIDARG;*out=buffer.data();return S_OK;}
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 count,DWORD flags) override{
        if(count!=frames||flags)return E_INVALIDARG;
        const auto* samples=reinterpret_cast<const std::int32_t*>(buffer.data());
        for(unsigned i=0;i<count*2;++i)if(samples[i]!=0x12340000)return E_FAIL;
        ++releases;SetEvent(released);return S_OK;
    }
};
struct Source final : IIntegerPcmSource {
    std::atomic<bool> ready{},exhausted{};
    bool CanProvide(UINT32) const noexcept override{return ready.load();}
    bool Exhausted() const noexcept override{return exhausted.load();}
    HRESULT Fill(std::int32_t* out,UINT32 cap,UINT32& written,bool& eos) noexcept override{
        std::fill(out,out+cap*2,0x12340000);written=cap;eos=false;ready.store(false);return S_OK;
    }
};
struct Fixture {
    Source source;WasapiExclusiveSink sink;FakeEndpoint* endpoint{new FakeEndpoint};
    explicit Fixture(UINT32 rate){
        sink.config_.format={rate,2,24,32,PcmEncoding::SignedInteger,true,24};
        sink.bufferFrames_=rate/50;sink.clockFrequency_=rate;
        endpoint->frequency=rate;endpoint->position.store(rate/2);endpoint->frames=sink.bufferFrames_;
        endpoint->buffer.resize(sink.bufferFrames_*8);
        sink.client_=endpoint;endpoint->AddRef();sink.clock_=endpoint;endpoint->AddRef();sink.render_=endpoint;
        sink.source_=&source;sink.state_.store(WasapiSinkState::Running);
        sink.submittedFrames_.store(rate);
        sink.committedClockFrames_.store(rate);
        sink.packetLedger_.Initialize(sink.bufferFrames_,2,32);
        sink.shutdownEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        sink.audioEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        sink.config_.sourceReadyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }
    ~Fixture(){const auto event=sink.config_.sourceReadyEvent;sink.Close();CloseHandle(event);}
    void Run(){
        Check(sink.RenderOneBuffer()==kAudioSourceWouldBlock,"empty source did not enter wait");
        Check(endpoint->stops.load()==0,"source wait stopped the DAC before committed audio played");
        sink.renderThread_=CreateThread(nullptr,0,&WasapiExclusiveSink::RenderThreadThunk,&sink,0,nullptr);
        Check(sink.renderThread_!=nullptr,"render worker creation failed");
        SetEvent(sink.audioEvent_);
    }
};
int main(){try{
    for(UINT32 rate:{44100u,48000u,96000u,192000u}){
        {Fixture f(rate);f.Run();
         Check(WaitForSingleObject(f.endpoint->stopped,60)==WAIT_TIMEOUT,"pending DAC audio was interrupted");
         f.source.ready.store(true);SetEvent(f.sink.config_.sourceReadyEvent);
         Check(WaitForSingleObject(f.endpoint->released,200)==WAIT_OBJECT_0,"late tail did not continue in the running device");
         Check(f.endpoint->stops.load()==0&&f.endpoint->starts.load()==0,"short producer wait restarted a running device");}
        {Fixture f(rate);f.Run();f.endpoint->position.store(rate+rate/5);SetEvent(f.sink.audioEvent_);
         Check(WaitForSingleObject(f.endpoint->stopped,60)==WAIT_TIMEOUT,"network underrun stopped the running endpoint");
         f.source.ready.store(true);SetEvent(f.sink.config_.sourceReadyEvent);
         Check(WaitForSingleObject(f.endpoint->released,200)==WAIT_OBJECT_0,"running endpoint did not accept the real packet after a long wait");
         const auto deadline=rate+rate/5+rate/50;
         Check(f.sink.Stats().committedClockFrames==deadline,"post-underrun deadline ignored the device clock head");
         Check(f.sink.Stats().submittedFrames==rate+rate/50,"device gap was counted as captured PCM");
         f.source.exhausted.store(true);SetEvent(f.sink.config_.sourceReadyEvent);
         Check(WaitForSingleObject(f.sink.renderThread_,50)==WAIT_TIMEOUT,"cumulative media count cut off the post-gap packet");
         f.endpoint->position.store(deadline);SetEvent(f.sink.audioEvent_);
         Check(WaitForSingleObject(f.sink.renderThread_,200)==WAIT_OBJECT_0&&f.sink.EndOfStreamDrained(),"post-gap packet did not finish at its mapped device deadline");
         Check(f.endpoint->releases.load()==1&&f.endpoint->starts.load()==0&&f.endpoint->stops.load()==0,"network wait restarted or duplicated a device packet");}
        {Fixture f(rate);f.Run();f.endpoint->position.store(rate);SetEvent(f.sink.audioEvent_);
         f.source.exhausted.store(true);SetEvent(f.sink.config_.sourceReadyEvent);
         Check(WaitForSingleObject(f.sink.renderThread_,200)==WAIT_OBJECT_0&&f.sink.EndOfStreamDrained(),"empty late EOF did not finish the already played generation");
         Check(f.endpoint->starts.load()==0&&f.endpoint->releases.load()==0,"empty late EOF restarted or fabricated a packet");}
        {Fixture f(rate);f.Run();f.source.exhausted.store(true);SetEvent(f.sink.config_.sourceReadyEvent);
         Check(WaitForSingleObject(f.sink.renderThread_,50)==WAIT_TIMEOUT,"EOF dropped committed audio before clock completion");
         f.endpoint->position.store(rate);SetEvent(f.sink.audioEvent_);
         Check(WaitForSingleObject(f.sink.renderThread_,200)==WAIT_OBJECT_0&&f.sink.EndOfStreamDrained(),"EOS did not complete at the device clock boundary");
         Check(f.endpoint->stops.load()==0&&f.endpoint->starts.load()==0,"EOF toggled the device while draining");}
        std::cout<<"PASS source wait, unchanged PCM, resume and late EOF at "<<rate<<" Hz\n";
    }
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
