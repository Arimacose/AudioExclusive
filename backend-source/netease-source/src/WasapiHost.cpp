#include "WasapiHost.h"
#include "WasapiPolicy.h"
#include "AsioHost.h"
#include "Backend.h"
#include "PacketClock.h"
#include <mmdeviceapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace nce { namespace {
using ActivateEndpoint=HRESULT(*)(const std::wstring&,IAudioClient**);
HRESULT ActivateEndpointDefault(const std::wstring& endpoint,IAudioClient** output) {
    *output=nullptr;IMMDeviceEnumerator* enumerator{};IMMDevice* device{};
    HRESULT result=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator));
    if(SUCCEEDED(result))result=enumerator->GetDevice(endpoint.c_str(),&device);
    if(SUCCEEDED(result))result=device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(output));
    if(device)device->Release();if(enumerator)enumerator->Release();return result;
}
class WasapiClient;
std::mutex wasapiRegistryMutex;
std::unordered_set<WasapiClient*> wasapiClients;
class WasapiClient final:public IAudioClient,public IAudioRenderClient,public IAudioClock {
    std::atomic<ULONG> references_{1};
    IAudioClient* query_{};std::wstring endpoint_;ActivateEndpoint activate_{};
    std::atomic<bool> initialized_{},started_{},everStarted_{},deviceOpen_{};
    WAVEFORMATEXTENSIBLE canonical_{};UINT32 periodFrames_{},capacity_{};
    REFERENCE_TIME requestedPeriod_{};GUID session_{};bool hasSession_{};
    std::mutex controlMutex_,queueMutex_,eventMutex_;
    std::vector<std::int32_t> ring_,packet_;
    std::uint64_t read_{},write_{};UINT32 acquired_{};
    HANDLE nativeEvent_{},commandEvent_{},completedEvent_{},hardwareEvent_{};
    enum class Command {Open,Close,Clear,Shutdown};
    Command command_{Command::Clear};HRESULT commandResult_{S_OK};std::thread worker_;
    std::atomic<HRESULT> fault_{S_OK};
    // All physical interfaces and packet history belong to the worker thread.
    IAudioClient* audio_{};IAudioRenderClient* render_{};IAudioClock* clock_{};
    WAVEFORMATEXTENSIBLE physical_{};UINT32 hardwareFrames_{};
    UINT64 clockFrequency_{},lastDevicePosition_{},clockOffset_{},physicalSubmitted_{};
    std::vector<std::int32_t> transfer_,history_,replay_;
    std::size_t replayRead_{};UINT32 historyCapacity_{};
    std::atomic<UINT64> clockFrames_{},clockQpc_{},clockSequence_{};
    std::atomic<std::uint64_t> hardwareEvents_{},underruns_{},consumed_{},opens_{},closes_{},hardwareSubmitted_{};
    std::atomic<UINT32> reportedHardwareFrames_{};
    std::atomic<WORD> reportedHardwareBits_{},reportedHardwareValid_{};
    static bool SupportedInput(const WAVEFORMATEX* format) {
        if(!format||!format->nChannels||format->nChannels>8||format->nSamplesPerSec<8000||format->nSamplesPerSec>768000)return false;
        if(format->wBitsPerSample!=16&&format->wBitsPerSample!=24&&format->wBitsPerSample!=32)return false;
        if(format->nBlockAlign!=format->nChannels*(format->wBitsPerSample/8)||format->nAvgBytesPerSec!=format->nSamplesPerSec*format->nBlockAlign)return false;
        if(format->wFormatTag==WAVE_FORMAT_PCM)return true;
        if(format->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)return format->wBitsPerSample==32;
        if(format->wFormatTag!=WAVE_FORMAT_EXTENSIBLE||format->cbSize<22)return false;
        const auto* ext=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        const GUID pcm={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}},floating={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        return ext->Samples.wValidBitsPerSample>0&&ext->Samples.wValidBitsPerSample<=format->wBitsPerSample&&
            (ext->SubFormat==pcm||(ext->SubFormat==floating&&format->wBitsPerSample==32&&ext->Samples.wValidBitsPerSample==32));
    }
    static HRESULT SelectFormat(IAudioClient* client,unsigned rate,WORD channels,WAVEFORMATEXTENSIBLE& selected) {
        for(const auto& candidate:WasapiPcmCandidates(rate,channels)){
            const auto result=client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&candidate.Format,nullptr);
            if(result==S_OK){selected=candidate;return S_OK;}
            if(result!=AUDCLNT_E_UNSUPPORTED_FORMAT&&result!=S_FALSE&&result!=E_INVALIDARG)return result;
        }return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    void SignalNative(){std::lock_guard guard(eventMutex_);if(nativeEvent_)SetEvent(nativeEvent_);}
    HRESULT Send(Command command) {
        command_=command;SetEvent(commandEvent_);
        if(WaitForSingleObject(completedEvent_,INFINITE)!=WAIT_OBJECT_0)return E_FAIL;
        return commandResult_;
    }
    HRESULT ReadHardwareClock() {
        if(!clock_)return AUDCLNT_E_NOT_INITIALIZED;
        UINT64 position{},qpc{};const auto result=clock_->GetPosition(&position,&qpc);
        if(FAILED(result))return result;
        if(position<lastDevicePosition_)return AUDCLNT_E_DEVICE_INVALIDATED;
        lastDevicePosition_=position;
        const auto frames=PacketClock::Frames(position,clockFrequency_,canonical_.Format.nSamplesPerSec);
        clockSequence_.fetch_add(1);clockFrames_.store(clockOffset_+frames);clockQpc_.store(qpc);clockSequence_.fetch_add(1);
        return S_OK;
    }
    HRESULT FillHardware(bool prime) {
        const auto played=std::min<UINT64>(clockFrames_.load()-clockOffset_,physicalSubmitted_);
        if(historyCapacity_ && physicalSubmitted_+hardwareFrames_-played>historyCapacity_)
            return AUDCLNT_E_BUFFER_ERROR;
        BYTE* destination{};auto result=render_->GetBuffer(hardwareFrames_,&destination);
        if(FAILED(result))return result;if(!destination)return E_POINTER;
        const auto channels=canonical_.Format.nChannels;
        UINT32 copied{};
        {
            std::lock_guard guard(queueMutex_);
            for(UINT32 frame=0;frame<hardwareFrames_;++frame){
                const bool replayAvailable=replayRead_<replay_.size()/channels;
                const bool queued=write_>read_;
                for(unsigned channel=0;channel<channels;++channel){
                    const auto sample=replayAvailable?replay_[replayRead_*channels+channel]:queued?ring_[(read_%capacity_)*channels+channel]:0;
                    transfer_[static_cast<std::size_t>(frame)*channels+channel]=sample;
                    StoreWasapiInteger(destination+static_cast<std::size_t>(frame)*physical_.Format.nBlockAlign+channel*(physical_.Format.wBitsPerSample/8),
                        physical_.Format.wBitsPerSample,physical_.Samples.wValidBitsPerSample,sample);
                }
                if(replayAvailable){++replayRead_;++copied;}
                else if(queued){++read_;++copied;}
            }
        }
        result=render_->ReleaseBuffer(hardwareFrames_,copied?0:AUDCLNT_BUFFERFLAGS_SILENT);
        if(FAILED(result))return result;
        for(UINT32 frame=0;frame<hardwareFrames_;++frame)for(unsigned channel=0;channel<channels;++channel)
            history_[((physicalSubmitted_+frame)%historyCapacity_)*channels+channel]=transfer_[static_cast<std::size_t>(frame)*channels+channel];
        physicalSubmitted_+=hardwareFrames_;hardwareSubmitted_.fetch_add(hardwareFrames_);consumed_.fetch_add(copied);
        if(!prime&&copied<hardwareFrames_)underruns_.fetch_add(1);
        SignalNative();return S_OK;
    }
    void CloseHardware(bool preserve) {
        if(!audio_)return;
        const auto stopped=audio_->Stop();
        const auto clockResult=clock_?ReadHardwareClock():E_NOINTERFACE;
        if(preserve&&SUCCEEDED(stopped)&&SUCCEEDED(clockResult)&&historyCapacity_){
            const auto played=std::min<UINT64>(clockFrames_.load()-clockOffset_,physicalSubmitted_);
            const auto earliest=physicalSubmitted_>historyCapacity_?physicalSubmitted_-historyCapacity_:0;
            const auto begin=std::max(played,earliest);const auto channels=canonical_.Format.nChannels;
            std::vector<std::int32_t> remainder;
            remainder.reserve(static_cast<std::size_t>(physicalSubmitted_-begin)*channels+replay_.size()-std::min(replay_.size(),replayRead_*channels));
            for(auto frame=begin;frame<physicalSubmitted_;++frame)for(unsigned channel=0;channel<channels;++channel)
                remainder.push_back(history_[(frame%historyCapacity_)*channels+channel]);
            remainder.insert(remainder.end(),replay_.begin()+std::min(replay_.size(),replayRead_*channels),replay_.end());
            {std::lock_guard guard(queueMutex_);replay_=std::move(remainder);replayRead_=0;}
        }
        if(clock_){clock_->Release();clock_=nullptr;}
        if(render_){render_->Release();render_=nullptr;}
        audio_->Release();audio_=nullptr;deviceOpen_.store(false);closes_.fetch_add(1);
        Log(L"wasapi_exclusive_released");
    }
    HRESULT OpenHardware() {
        if(audio_)return AUDCLNT_E_NOT_STOPPED;
        REFERENCE_TIME period=requestedPeriod_;HRESULT result=E_FAIL;
        for(unsigned attempt=0;attempt<3;++attempt){
            result=activate_(endpoint_,&audio_);if(FAILED(result)||!audio_)return FAILED(result)?result:E_POINTER;
            result=SelectFormat(audio_,canonical_.Format.nSamplesPerSec,canonical_.Format.nChannels,physical_);
            if(FAILED(result)){CloseHardware(false);return result;}
            result=audio_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,period,period,&physical_.Format,hasSession_?&session_:nullptr);
            if(result!=AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)break;
            UINT32 aligned{};const auto size=audio_->GetBufferSize(&aligned);CloseHardware(false);
            if(FAILED(size)||!aligned)return FAILED(size)?size:E_UNEXPECTED;
            const auto next=FramesPeriod(aligned,canonical_.Format.nSamplesPerSec);
            if(next==period)return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;period=next;
        }
        if(FAILED(result)){CloseHardware(false);return result;}
        result=audio_->GetBufferSize(&hardwareFrames_);
        if(FAILED(result)||!hardwareFrames_||hardwareFrames_>capacity_){CloseHardware(false);return FAILED(result)?result:AUDCLNT_E_BUFFER_SIZE_ERROR;}
        result=audio_->SetEventHandle(hardwareEvent_);
        if(SUCCEEDED(result))result=audio_->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(&render_));
        if(SUCCEEDED(result))result=audio_->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(&clock_));
        if(SUCCEEDED(result))result=clock_->GetFrequency(&clockFrequency_);
        if(FAILED(result)||!clockFrequency_){CloseHardware(false);return FAILED(result)?result:E_UNEXPECTED;}
        transfer_.assign(static_cast<std::size_t>(hardwareFrames_)*canonical_.Format.nChannels,0);
        REFERENCE_TIME latency{};const auto latencyResult=audio_->GetStreamLatency(&latency);
        const auto latencyFrames=SUCCEEDED(latencyResult)&&latency>0
            ? static_cast<UINT64>(latency)*canonical_.Format.nSamplesPerSec/10000000ULL : 0;
        historyCapacity_=static_cast<UINT32>(std::max<UINT64>(canonical_.Format.nSamplesPerSec/2,
            static_cast<UINT64>(hardwareFrames_)*4+latencyFrames));
        history_.assign(static_cast<std::size_t>(historyCapacity_)*canonical_.Format.nChannels,0);
        physicalSubmitted_=0;lastDevicePosition_=0;clockOffset_=clockFrames_.load();
        result=FillHardware(true);
        if(SUCCEEDED(result))result=audio_->Start();
        if(FAILED(result)){CloseHardware(false);return result;}
        reportedHardwareFrames_.store(hardwareFrames_);reportedHardwareBits_.store(physical_.Format.wBitsPerSample);
        reportedHardwareValid_.store(physical_.Samples.wValidBitsPerSample);deviceOpen_.store(true);opens_.fetch_add(1);
        Log(L"wasapi_exclusive_acquired_rate_"+std::to_wstring(canonical_.Format.nSamplesPerSec)+L"_bits_"+
            std::to_wstring(physical_.Format.wBitsPerSample)+L"_frames_"+std::to_wstring(hardwareFrames_));
        return S_OK;
    }
    void Run() noexcept {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        const HANDLE waits[]={commandEvent_,hardwareEvent_};
        for(;;){
            const auto wake=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
            if(wake==WAIT_OBJECT_0){
                const auto command=command_;HRESULT result=S_OK;
                try {
                    if(command==Command::Open)result=SUCCEEDED(com)?OpenHardware():com;
                    else if(command==Command::Close)CloseHardware(true);
                    else if(command==Command::Clear){CloseHardware(false);std::lock_guard guard(queueMutex_);read_=write_=0;acquired_=0;replay_.clear();replayRead_=0;
                        clockSequence_.fetch_add(1);clockFrames_.store(0);clockQpc_.store(0);clockSequence_.fetch_add(1);}
                    else CloseHardware(false);
                }catch(const std::bad_alloc&){result=E_OUTOFMEMORY;CloseHardware(false);}
                catch(...){result=E_FAIL;CloseHardware(false);}
                commandResult_=result;SetEvent(completedEvent_);
                if(command==Command::Shutdown)break;
            }else if(wake==WAIT_OBJECT_0+1&&audio_&&deviceOpen_.load()){
                hardwareEvents_.fetch_add(1);
                auto result=ReadHardwareClock();if(SUCCEEDED(result))result=FillHardware(false);
                if(FAILED(result)){fault_.store(result);Backend::Instance().OutputFault(result);Log(L"wasapi_event_render_failed",result);CloseHardware(false);SignalNative();}
            }else if(wake!=WAIT_OBJECT_0+1)break;
        }
        if(SUCCEEDED(com))CoUninitialize();
    }
public:
    WasapiClient(IAudioClient* query,std::wstring endpoint,ActivateEndpoint activate=ActivateEndpointDefault):query_(query),endpoint_(std::move(endpoint)),activate_(activate){
        std::lock_guard guard(wasapiRegistryMutex);wasapiClients.insert(this);
    }
    ~WasapiClient(){
        {std::lock_guard guard(wasapiRegistryMutex);wasapiClients.erase(this);}
        if(worker_.joinable()){Send(Command::Shutdown);worker_.join();}
        if(commandEvent_)CloseHandle(commandEvent_);if(completedEvent_)CloseHandle(completedEvent_);if(hardwareEvent_)CloseHandle(hardwareEvent_);
        if(nativeEvent_)CloseHandle(nativeEvent_);query_->Release();Backend::Instance().Closed(static_cast<IAudioClient*>(this));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IAudioClient))*output=static_cast<IAudioClient*>(this);
        else if(iid==__uuidof(IAudioRenderClient))*output=static_cast<IAudioRenderClient*>(this);
        else if(iid==__uuidof(IAudioClock))*output=static_cast<IAudioClock*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX* format,LPCGUID session) override {
        std::lock_guard guard(controlMutex_);if(initialized_.load())return AUDCLNT_E_ALREADY_INITIALIZED;
        if(mode!=AUDCLNT_SHAREMODE_EXCLUSIVE||(flags&AUDCLNT_STREAMFLAGS_LOOPBACK)||!SupportedInput(format))return AUDCLNT_E_UNSUPPORTED_FORMAT;
        WAVEFORMATEXTENSIBLE available{};auto result=SelectFormat(query_,format->nSamplesPerSec,format->nChannels,available);if(FAILED(result))return result;
        REFERENCE_TIME normal{},minimum{};result=query_->GetDevicePeriod(&normal,&minimum);if(FAILED(result))return result;
        requestedPeriod_=std::max<REFERENCE_TIME>({normal,minimum,200000});
        periodFrames_=static_cast<UINT32>((static_cast<std::uint64_t>(format->nSamplesPerSec)*requestedPeriod_+9999999)/10000000);
        if(!periodFrames_||periodFrames_>format->nSamplesPerSec)return AUDCLNT_E_BUFFER_SIZE_ERROR;
        requestedPeriod_=FramesPeriod(periodFrames_,format->nSamplesPerSec);capacity_=periodFrames_*4;
        canonical_={};canonical_.Format={WAVE_FORMAT_EXTENSIBLE,format->nChannels,format->nSamplesPerSec,format->nSamplesPerSec*format->nChannels*4,
            static_cast<WORD>(format->nChannels*4),32,22};canonical_.Samples.wValidBitsPerSample=32;
        canonical_.dwChannelMask=format->nChannels==2?3:0;canonical_.SubFormat={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        if(session){session_=*session;hasSession_=true;}
        try {
            ring_.resize(static_cast<std::size_t>(capacity_)*format->nChannels);packet_.resize(ring_.size());
            commandEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);completedEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);hardwareEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!commandEvent_||!completedEvent_||!hardwareEvent_)return HRESULT_FROM_WIN32(GetLastError());
            worker_=std::thread([this]{Run();});
        }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
        // This updates the native converter to signed interleaved PCM32; it
        // does not initialize or start any physical audio endpoint.
        PrepareNativeAsioFormat(canonical_.Format);initialized_.store(true);
        Backend::Instance().OpenBegin(static_cast<IAudioClient*>(this),true);
        Backend::Instance().Initialized(static_cast<IAudioClient*>(this),S_OK,capacity_,endpoint_);
        Backend::Instance().OpenEnd(static_cast<IAudioClient*>(this),true,static_cast<IAudioClient*>(this),static_cast<IAudioRenderClient*>(this),&canonical_);
        Log(L"wasapi_software_queue_ready_rate_"+std::to_wstring(format->nSamplesPerSec));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* output) override{if(!output)return E_POINTER;if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;*output=capacity_;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* output) override{if(!output)return E_POINTER;if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;*output=FramesPeriod(capacity_+reportedHardwareFrames_.load(),canonical_.Format.nSamplesPerSec);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* output) override {
        if(!output)return E_POINTER;if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;const auto fault=fault_.load();if(FAILED(fault))return fault;
        std::lock_guard guard(queueMutex_);*output=!started_.load()&&everStarted_.load()?capacity_:static_cast<UINT32>(std::min<std::uint64_t>(write_-read_,capacity_));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode,const WAVEFORMATEX* format,WAVEFORMATEX** closest) override{
        if(closest)*closest=nullptr;if(mode!=AUDCLNT_SHAREMODE_EXCLUSIVE||!SupportedInput(format))return AUDCLNT_E_UNSUPPORTED_FORMAT;
        WAVEFORMATEXTENSIBLE available{};return SelectFormat(query_,format->nSamplesPerSec,format->nChannels,available);
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** output) override{return query_->GetMixFormat(output);}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal,REFERENCE_TIME* minimum) override{const auto period=initialized_.load()?requestedPeriod_:200000;if(normal)*normal=period;if(minimum)*minimum=period;return S_OK;}
    HRESULT STDMETHODCALLTYPE Start() override {
        std::lock_guard guard(controlMutex_);if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;if(started_.load())return AUDCLNT_E_NOT_STOPPED;
        {std::lock_guard eventGuard(eventMutex_);if(!nativeEvent_)return AUDCLNT_E_EVENTHANDLE_NOT_SET;}
        fault_.store(S_OK);const auto result=Send(Command::Open);started_.store(SUCCEEDED(result));if(SUCCEEDED(result)){everStarted_.store(true);SignalNative();}
        Backend::Instance().Started(static_cast<IAudioClient*>(this),result);return result;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        std::lock_guard guard(controlMutex_);if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;
        started_.store(false);const auto result=Send(Command::Close);Backend::Instance().Stopped(static_cast<IAudioClient*>(this));return result;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        std::lock_guard guard(controlMutex_);if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;if(started_.load())return AUDCLNT_E_NOT_STOPPED;
        const auto result=Send(Command::Clear);if(SUCCEEDED(result)){everStarted_.store(false);fault_.store(S_OK);}return result;
    }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override {
        if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;if(!event)return E_INVALIDARG;HANDLE duplicate{};
        if(!DuplicateHandle(GetCurrentProcess(),event,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))return HRESULT_FROM_WIN32(GetLastError());
        std::lock_guard guard(eventMutex_);if(nativeEvent_)CloseHandle(nativeEvent_);nativeEvent_=duplicate;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid,void** output) override{if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;return QueryInterface(iid,output);}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames,BYTE** output) override {
        if(!output)return E_POINTER;*output=nullptr;if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;const auto fault=fault_.load();if(FAILED(fault))return fault;
        std::lock_guard guard(queueMutex_);if(acquired_)return AUDCLNT_E_OUT_OF_ORDER;if(frames>capacity_-(write_-read_))return AUDCLNT_E_BUFFER_TOO_LARGE;
        if(!frames)return S_OK;acquired_=frames;*output=reinterpret_cast<BYTE*>(packet_.data());return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames,DWORD flags) override {
        if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;bool signal=false;
        {std::lock_guard guard(queueMutex_);if(frames>acquired_)return AUDCLNT_E_INVALID_SIZE;if(!frames){acquired_=0;return S_OK;}
            const auto channels=canonical_.Format.nChannels;
            for(UINT32 frame=0;frame<frames;++frame)for(unsigned channel=0;channel<channels;++channel){
                const auto sample=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:packet_[static_cast<std::size_t>(frame)*channels+channel];
                ring_[((write_+frame)%capacity_)*channels+channel]=sample;signal|=sample!=0;
            }write_+=frames;acquired_=0;
        }
        Backend::Instance().Submitted(static_cast<IAudioRenderClient*>(this),frames,S_OK);if(signal)Backend::Instance().Content(static_cast<IAudioRenderClient*>(this));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* output) override{if(!output)return E_POINTER;if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;*output=canonical_.Format.nSamplesPerSec;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position,UINT64* qpc) override {
        if(!position)return E_POINTER;if(!initialized_.load())return AUDCLNT_E_NOT_INITIALIZED;const auto fault=fault_.load();if(FAILED(fault))return fault;
        for(;;){const auto sequence=clockSequence_.load();if(sequence&1)continue;const auto frames=clockFrames_.load(),stamp=clockQpc_.load();if(sequence!=clockSequence_.load())continue;
            *position=frames;if(qpc)*qpc=stamp;return S_OK;}
    }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* output) override{if(!output)return E_POINTER;*output=AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;return S_OK;}
    bool DeviceOpen() const{return deviceOpen_.load();}
    std::wstring StatusJson() {
        if(!initialized_.load())return L"{\"initialized\":false}";
        std::uint64_t queued{};{std::lock_guard guard(queueMutex_);queued=write_-read_;}
        return L"{\"initialized\":true,\"deviceOpen\":"+std::wstring(deviceOpen_.load()?L"true":L"false")+
            L",\"started\":"+(started_.load()?L"true":L"false")+L",\"endpoint\":"+JsonString(endpoint_)+
            L",\"sampleRate\":"+std::to_wstring(canonical_.Format.nSamplesPerSec)+L",\"queueFrames\":"+std::to_wstring(queued)+
            L",\"hardwareFrames\":"+std::to_wstring(reportedHardwareFrames_.load())+L",\"containerBits\":"+std::to_wstring(reportedHardwareBits_.load())+
            L",\"validBits\":"+std::to_wstring(reportedHardwareValid_.load())+L",\"deviceEvents\":"+std::to_wstring(hardwareEvents_.load())+
            L",\"underruns\":"+std::to_wstring(underruns_.load())+L",\"consumedFrames\":"+std::to_wstring(consumed_.load())+
            L",\"hardwareSubmittedFrames\":"+std::to_wstring(hardwareSubmitted_.load())+L",\"clockFrames\":"+std::to_wstring(clockFrames_.load())+
            L",\"opens\":"+std::to_wstring(opens_.load())+L",\"closes\":"+std::to_wstring(closes_.load())+
            L",\"hresult\":"+std::to_wstring(static_cast<unsigned long>(fault_.load()))+L"}";
    }
};
}
HRESULT CreateWasapiClient(IAudioClient* query,const std::wstring& endpoint,IAudioClient** output) {
    if(!output)return E_POINTER;*output=nullptr;
    try{*output=new WasapiClient(query,endpoint);return S_OK;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
std::wstring WasapiStatusJson() {
    std::lock_guard guard(wasapiRegistryMutex);bool open=false;std::wstring clients=L"[";
    for(auto* client:wasapiClients){open|=client->DeviceOpen();if(clients.size()>1)clients+=L",";clients+=client->StatusJson();}
    return L"{\"deviceOpen\":"+std::wstring(open?L"true":L"false")+L",\"sinks\":"+clients+L"]}";
}
}
