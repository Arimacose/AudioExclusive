// Compile the production host with an injected endpoint factory. The fake
// endpoint owns real Windows events and exposes the documented COM contract.
#include "WasapiHost.cpp"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
struct Fixture {
    std::mutex mutex;WORD bits{32};unsigned rate{44100};bool align{};
    unsigned activations{},initializations{},held{},packets{};
    HANDLE deviceEvent{},capturedEvent{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    std::vector<BYTE> firstPacket,lastPacket;
    ~Fixture(){CloseHandle(capturedEvent);}
} fixture;
class FakeDevice final:public IAudioClient,public IAudioRenderClient,public IAudioClock {
    std::atomic<ULONG> refs_{1};bool initialized_{};WAVEFORMATEXTENSIBLE format_{};UINT32 frames_{};std::vector<BYTE> buffer_;
public:
    ~FakeDevice(){if(initialized_){std::lock_guard guard(fixture.mutex);--fixture.held;}}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IAudioClient))*output=static_cast<IAudioClient*>(this);
        else if(iid==__uuidof(IAudioRenderClient))*output=static_cast<IAudioRenderClient*>(this);
        else if(iid==__uuidof(IAudioClock))*output=static_cast<IAudioClock*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode,DWORD,REFERENCE_TIME duration,REFERENCE_TIME,const WAVEFORMATEX* format,LPCGUID) override {
        std::lock_guard guard(fixture.mutex);++fixture.initializations;
        if(fixture.held)return AUDCLNT_E_DEVICE_IN_USE;
        Check(mode==AUDCLNT_SHAREMODE_EXCLUSIVE,"physical client silently switched to shared mode");
        frames_=static_cast<UINT32>((static_cast<UINT64>(format->nSamplesPerSec)*duration+5000000)/10000000);
        if(fixture.align&&frames_%128){frames_=(frames_+127)/128*128;return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;}
        format_={};memcpy(&format_,format,std::min<std::size_t>(sizeof(format_),sizeof(WAVEFORMATEX)+format->cbSize));
        if(format->wFormatTag!=WAVE_FORMAT_EXTENSIBLE)format_.Samples.wValidBitsPerSample=format->wBitsPerSample;
        initialized_=true;++fixture.held;buffer_.resize(static_cast<std::size_t>(frames_)*format->nBlockAlign);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* output) override{*output=frames_;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* output) override{*output=nce::FramesPeriod(frames_,format_.Format.nSamplesPerSec);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE,const WAVEFORMATEX* format,WAVEFORMATEX** closest) override {
        if(closest)*closest=nullptr;
        const auto valid=format->wFormatTag==WAVE_FORMAT_EXTENSIBLE?reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->Samples.wValidBitsPerSample:format->wBitsPerSample;
        return format->nSamplesPerSec==fixture.rate&&format->wBitsPerSample==fixture.bits&&valid==fixture.bits?S_OK:AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal,REFERENCE_TIME* minimum) override{if(normal)*normal=100000;if(minimum)*minimum=30000;return S_OK;}
    HRESULT STDMETHODCALLTYPE Start() override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Stop() override{return initialized_?S_OK:AUDCLNT_E_NOT_INITIALIZED;}
    HRESULT STDMETHODCALLTYPE Reset() override{return S_OK;}
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override{std::lock_guard guard(fixture.mutex);fixture.deviceEvent=event;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid,void** output) override{return QueryInterface(iid,output);}
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* output) override{*output=10000000;return S_OK;}
    // Deliberately frozen: a lagging timestamp must not suppress device events.
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position,UINT64* qpc) override{*position=0;if(qpc)*qpc=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* output) override{*output=AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames,BYTE** output) override{if(frames!=frames_)return AUDCLNT_E_BUFFER_SIZE_ERROR;*output=buffer_.data();return S_OK;}
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames,DWORD flags) override {
        if(frames!=frames_)return AUDCLNT_E_INVALID_SIZE;
        {std::lock_guard guard(fixture.mutex);fixture.lastPacket=buffer_;if(!fixture.packets&&!(flags&AUDCLNT_BUFFERFLAGS_SILENT))fixture.firstPacket=buffer_;++fixture.packets;}
        SetEvent(fixture.capturedEvent);return S_OK;
    }
};
HRESULT ActivateFake(const std::wstring&,IAudioClient** output){std::lock_guard guard(fixture.mutex);++fixture.activations;*output=new FakeDevice;return S_OK;}
unsigned Packets(){std::lock_guard guard(fixture.mutex);return fixture.packets;}
void WaitPackets(unsigned target){
    const auto until=GetTickCount64()+2000;
    while(Packets()<target){Check(GetTickCount64()<until,"device event did not produce a packet");WaitForSingleObject(fixture.capturedEvent,50);}
}
void Pulse(){HANDLE event{};{std::lock_guard guard(fixture.mutex);event=fixture.deviceEvent;}Check(event!=nullptr,"no hardware event registered");Check(SetEvent(event)!=FALSE,"hardware event closed prematurely");}
std::int32_t Sample(unsigned frame,unsigned channel){return static_cast<std::int32_t>((frame*7919u+channel*65537u)*256u)^static_cast<std::int32_t>(0x9a120000u);}
void Write(IAudioRenderClient* render,UINT32 frames,unsigned offset=0){
    BYTE* data{};Check(SUCCEEDED(render->GetBuffer(frames,&data)),"software queue rejected an available packet");
    auto* pcm=reinterpret_cast<std::int32_t*>(data);for(unsigned frame=0;frame<frames;++frame)for(unsigned channel=0;channel<2;++channel)pcm[frame*2+channel]=Sample(offset+frame,channel);
    Check(SUCCEEDED(render->ReleaseBuffer(frames,0)),"software queue submission failed");
}
void RunCase(unsigned rate,WORD bits,bool align){
    {std::lock_guard guard(fixture.mutex);Check(!fixture.held,"previous case retained exclusive endpoint");fixture.rate=rate;fixture.bits=bits;fixture.align=align;
        fixture.activations=fixture.initializations=fixture.packets=0;fixture.deviceEvent=nullptr;fixture.firstPacket.clear();}
    IAudioClient* audio=new nce::WasapiClient(new FakeDevice,L"fake-endpoint",ActivateFake);
    const WAVEFORMATEX source={WAVE_FORMAT_IEEE_FLOAT,2,rate,rate*8,8,32,0};
    Check(SUCCEEDED(audio->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0,0,&source,nullptr)),"software Initialize failed");
    {std::lock_guard guard(fixture.mutex);Check(!fixture.activations&&!fixture.initializations&&!fixture.held,"startup initialized a physical endpoint");}
    UINT32 capacity{};audio->GetBufferSize(&capacity);Check(capacity==rate/50*4,"unexpected software capacity");
    HANDLE nativeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(SUCCEEDED(audio->SetEventHandle(nativeEvent)),"native event rejected");
    IAudioRenderClient* render{};Check(SUCCEEDED(audio->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(&render))),"render interface missing");
    Write(render,capacity);Check(SUCCEEDED(audio->Start()),"exclusive Start failed");WaitPackets(1);
    std::vector<BYTE> captured;{std::lock_guard guard(fixture.mutex);Check(fixture.held==1,"Start did not hold exactly one endpoint");captured=fixture.firstPacket;}
    Check(!captured.empty(),"prime packet lost queued audio");
    const unsigned hardwareFrames=static_cast<unsigned>(captured.size()/(2*(bits/8)));
    std::vector<BYTE> expected(captured.size());
    for(unsigned frame=0;frame<hardwareFrames;++frame)for(unsigned channel=0;channel<2;++channel)
        nce::StoreWasapiInteger(expected.data()+(frame*2+channel)*(bits/8),bits,bits,Sample(frame,channel));
    Check(captured==expected,"hardware PCM differs from canonical source samples");
    for(unsigned pulse=0;pulse<8;++pulse){const auto before=Packets();Pulse();WaitPackets(before+1);
        UINT32 padding{};Check(SUCCEEDED(audio->GetCurrentPadding(&padding)),"padding failed with a lagging hardware clock");
        if(capacity-padding>=rate/50)Write(render,rate/50,capacity+pulse*(rate/50));}
    Check(SUCCEEDED(audio->Stop()),"pause failed");
    {std::lock_guard guard(fixture.mutex);Check(!fixture.held,"pause retained exclusive ownership");}
    UINT32 padding{};audio->GetCurrentPadding(&padding);Check(padding==capacity,"paused producer was allowed to keep filling");
    Check(SUCCEEDED(audio->Start()),"resume did not reacquire endpoint");
    {std::lock_guard guard(fixture.mutex);Check(fixture.lastPacket==expected,"pause/resume discarded still-unplayed PCM beyond the previous four-packet history");}
    {std::lock_guard guard(fixture.mutex);Check(fixture.held==1,"resume did not reopen physical device");}
    const auto before=Packets();Pulse();WaitPackets(before+1);Check(SUCCEEDED(audio->Stop()),"second pause failed");
    Check(SUCCEEDED(audio->Reset()),"paused reset failed");audio->GetCurrentPadding(&padding);Check(!padding,"reset retained old queued PCM");
    Write(render,capacity);Check(SUCCEEDED(audio->Start()),"Start after reset failed");Check(SUCCEEDED(audio->Stop()),"Stop after reset failed");
    render->Release();audio->Release();CloseHandle(nativeEvent);
    {std::lock_guard guard(fixture.mutex);Check(!fixture.held,"destructor leaked the physical endpoint");}
}
}
namespace nce {
void PrepareNativeAsioFormat(const WAVEFORMATEX& format){Check(format.wBitsPerSample==32&&format.nBlockAlign==format.nChannels*4,"native converter was not configured for PCM32");}
std::wstring AsioStatusJson(){return L"{}";}
std::wstring ConfigPath(){return L"unused-test-settings.ini";}
void Log(std::wstring_view,HRESULT) noexcept{}
std::wstring JsonString(std::wstring_view text){return L"\""+std::wstring(text)+L"\"";}
}
int main(){try{
    for(unsigned rate:{44100u,48000u,96000u,192000u})for(WORD bits:{WORD{16},WORD{24},WORD{32}})RunCase(rate,bits,false);
    RunCase(44100,24,true);
    std::cout<<"PASS: 13 real-host cases; 44.1/48/96/192 kHz, 16/24/32-bit PCM, aligned buffers, frozen clocks, pause/reopen/reset, exact packet bytes, no startup/pause device ownership\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
