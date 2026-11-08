#include "Backend.h"
#include "PacketClock.h"
#include "AsioHost.h"
#include "WasapiHost.h"
#include "WasapiPolicy.h"
#include <MinHook.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <new>

namespace nce { namespace {
using ActivateFn=HRESULT(STDMETHODCALLTYPE*)(IMMDevice*,REFIID,DWORD,PROPVARIANT*,void**);
using InitFn=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*,AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,LPCGUID);
using ServiceFn=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*,REFIID,void**);
using SimpleFn=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*);
using ReleaseFn=ULONG(STDMETHODCALLTYPE*)(IAudioClient*);
using PaddingFn=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*,UINT32*);
using PeriodFn=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*,REFERENCE_TIME*,REFERENCE_TIME*);
using EventFn=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*,HANDLE);
using BufferFn=HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*,UINT32,BYTE**);
using SubmitFn=HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*,UINT32,DWORD);
using OpenFn=int(*)(void*,const void*);
ActivateFn originalActivate{};InitFn originalInit{};ServiceFn originalService{};
SimpleFn originalStart{},originalStop{},originalReset{};ReleaseFn originalRelease{};
PaddingFn originalPadding{};PeriodFn originalPeriod{};
EventFn originalEvent{};
BufferFn originalBuffer{};SubmitFn originalSubmit{};OpenFn originalOpen{};
const ClientProfile* nativeProfile{};
struct ClientInfo {std::wstring endpoint;bool tracked{},exclusive{},initialized{},running{},convertPcm{},clockSeen{};WAVEFORMATEXTENSIBLE format{};REFERENCE_TIME period{};void* render{};HANDLE event{};UINT32 bufferFrames{};PacketClock clock;std::uint64_t lastPlayed{},paddingCalls{},blockedCalls{},readyCalls{},submitCalls{},starts{},stops{},resets{},lastSubmitPlayed{},lastSubmitTick{},lastStartPlayed{},lastStopPlayed{};};
struct RenderInfo {void* client{};BYTE* buffer{};UINT32 frames{};WORD blockAlign{},containerBits{},validBits{};bool convertPcm{};std::vector<BYTE> scratch;};
std::mutex clientMutex,renderHookMutex;
std::unordered_map<void*,ClientInfo> clients;
std::unordered_map<void*,RenderInfo> renders;
void* bufferTarget{};void* submitTarget{};
thread_local bool nativeOpening{};
thread_local void* nativeStream{};
bool Client(void* pointer,ClientInfo& result) {
    std::lock_guard guard(clientMutex);auto found=clients.find(pointer);
    if(found==clients.end())return false;result=found->second;return true;
}
int HookOpen(void* stream,const void* format) {
    const bool enabled=Backend::Instance().Enabled();
    *reinterpret_cast<int*>(static_cast<unsigned char*>(stream)+0x158)=enabled?AUDCLNT_SHAREMODE_EXCLUSIVE:AUDCLNT_SHAREMODE_SHARED;
    struct Scope {bool previous=nativeOpening;void* previousStream=nativeStream;Scope(void* stream){nativeOpening=true;nativeStream=stream;}~Scope(){nativeOpening=previous;nativeStream=previousStream;}} scope(stream);
    Log(enabled?L"actual_native_open_exclusive":L"actual_native_open_shared");
    int result=originalOpen(stream,format);Log(L"actual_native_open_result",static_cast<HRESULT>(result));return result;
}
HRESULT STDMETHODCALLTYPE HookPeriod(IAudioClient* client,REFERENCE_TIME* normal,REFERENCE_TIME* minimum) {
    HRESULT result=originalPeriod(client,normal,minimum);ClientInfo info;
    if(SUCCEEDED(result)&&Client(client,info)&&info.tracked&&info.initialized&&info.exclusive&&info.period>0) {
        if(normal)*normal=info.period;if(minimum)*minimum=info.period;
    }
    return result;
}
HRESULT PlayedFrames(IAudioClient* client,std::uint32_t rate,std::uint64_t& frames) {
    IAudioClock* clock{};
    HRESULT result=originalService(client,__uuidof(IAudioClock),reinterpret_cast<void**>(&clock));
    if(FAILED(result))return result;
    UINT64 frequency{},position{};
    result=clock->GetFrequency(&frequency);
    if(SUCCEEDED(result))result=clock->GetPosition(&position,nullptr);
    clock->Release();
    if(FAILED(result))return result;
    if(!frequency)return E_UNEXPECTED;
    frames=PacketClock::Frames(position,frequency,rate);return S_OK;
}
HRESULT STDMETHODCALLTYPE HookPadding(IAudioClient* client,UINT32* padding) {
    ClientInfo info;
    if(Client(client,info)&&info.tracked&&info.initialized&&info.exclusive) {
        // Event-driven exclusive mode hands over one entire ping-pong packet.
        // The native shared renderer asks for padding; there is no partial
        // packet to subtract in this mode. Device faults still propagate.
        HRESULT result=originalPadding(client,padding);
        if(SUCCEEDED(result)||result==E_NOTIMPL||result==AUDCLNT_E_BUFFER_SIZE_ERROR) {
            if(!padding)return E_POINTER;
            if(nativeOpening){*padding=0;return S_OK;}
            if(!info.running){*padding=info.bufferFrames;return S_OK;}
            std::uint64_t played{};HRESULT clockResult=PlayedFrames(client,info.format.Format.nSamplesPerSec,played);
            if(FAILED(clockResult)){Log(L"endpoint_clock_failed",clockResult);return clockResult;}
            if(info.clockSeen&&played<info.lastPlayed){Log(L"endpoint_clock_went_backwards",AUDCLNT_E_DEVICE_INVALIDATED);return AUDCLNT_E_DEVICE_INVALIDATED;}
            {std::lock_guard guard(clientMutex);auto found=clients.find(client);if(found!=clients.end()){
                auto& state=found->second;state.lastPlayed=played;state.clockSeen=true;++state.paddingCalls;
                if(info.clock.Ready(played))++state.readyCalls;else ++state.blockedCalls;
            }}
            *padding=info.clock.Ready(played)?0:info.bufferFrames;
            return S_OK;
        }
        Log(L"exclusive_padding_failed",result);return result;
    }
    return originalPadding(client,padding);
}
HRESULT STDMETHODCALLTYPE HookInitialize(IAudioClient* client,AUDCLNT_SHAREMODE mode,DWORD flags,
    REFERENCE_TIME duration,REFERENCE_TIME periodicity,const WAVEFORMATEX* format,LPCGUID session) {
    ClientInfo info;bool renderClient=Client(client,info);
    WAVEFORMATEXTENSIBLE hardwareFormat{};
    if(renderClient&&!nativeOpening&&Backend::Instance().Enabled()&&mode==AUDCLNT_SHAREMODE_SHARED&&!(flags&AUDCLNT_STREAMFLAGS_LOOPBACK)) {
        Log(L"shared_fallback_blocked",AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED);
        return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    }
    bool tracked=renderClient&&nativeOpening;
    if(tracked) {
        Backend::Instance().OpenBegin(client,mode==AUDCLNT_SHAREMODE_EXCLUSIVE);
        info.tracked=true;info.exclusive=mode==AUDCLNT_SHAREMODE_EXCLUSIVE;
        if(info.exclusive&&(!format||!format->nChannels||format->nSamplesPerSec<8000||format->nSamplesPerSec>768000)){
            Backend::Instance().OutputFault(AUDCLNT_E_UNSUPPORTED_FORMAT);return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if(info.exclusive&&format&&nativeStream) {
            // Preserve the source rate. Ask the native pipeline to convert its
            // decoded samples to the endpoint's accepted packed PCM format.
            // Merely changing Initialize's descriptor would corrupt PCM bytes;
            // update its own output AudioFormat through the pinned converter.
            WAVEFORMATEXTENSIBLE candidate{};
            bool accepted=false;
            for(const auto& option:WasapiPcmCandidates(format->nSamplesPerSec,format->nChannels)) {
                const auto support=client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&option.Format,nullptr);
                if(support==S_OK){candidate=option;accepted=true;break;}
                if(support!=AUDCLNT_E_UNSUPPORTED_FORMAT&&support!=S_FALSE&&support!=E_INVALIDARG){Backend::Instance().OutputFault(support);return support;}
            }
            if(!accepted){Log(L"endpoint_has_no_supported_pcm_at_source_rate",AUDCLNT_E_UNSUPPORTED_FORMAT);Backend::Instance().OutputFault(AUDCLNT_E_UNSUPPORTED_FORMAT);return AUDCLNT_E_UNSUPPORTED_FORMAT;}
            if(accepted) {
                auto* wave=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(static_cast<unsigned char*>(nativeStream)+0x130);
                hardwareFormat=candidate;format=&hardwareFormat.Format;
                *wave=candidate;info.convertPcm=candidate.Format.wBitsPerSample!=32||candidate.Samples.wValidBitsPerSample!=32;
                wave->Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;wave->Format.cbSize=22;
                wave->Format.wBitsPerSample=32;wave->Samples.wValidBitsPerSample=32;
                wave->Format.nBlockAlign=wave->Format.nChannels*4;wave->Format.nAvgBytesPerSec=wave->Format.nSamplesPerSec*wave->Format.nBlockAlign;
                using ConvertFn=void(*)(const void*,void*);
                auto convert=reinterpret_cast<ConvertFn>(reinterpret_cast<unsigned char*>(GetModuleHandleW(L"cloudmusic.dll"))+nativeProfile->convertRva);
                convert(wave,static_cast<unsigned char*>(nativeStream)+0xe8);
                Log(L"native_pcm_selected_container_"+std::to_wstring(candidate.Format.wBitsPerSample)+L"_valid_"+std::to_wstring(candidate.Samples.wValidBitsPerSample));
            }
        }
        if(format)std::memcpy(&info.format,format,std::min<std::size_t>(sizeof(info.format),sizeof(WAVEFORMATEX)+format->cbSize));
        Log(L"actual_initialize_mode_"+std::to_wstring(mode)+L"_flags_"+std::to_wstring(flags)+
            L"_rate_"+std::to_wstring(info.format.Format.nSamplesPerSec)+L"_bits_"+std::to_wstring(info.format.Format.wBitsPerSample));
        if(info.exclusive&&(flags&AUDCLNT_STREAMFLAGS_EVENTCALLBACK)&&format) {
            REFERENCE_TIME normal{},minimum{};
            HRESULT result=originalPeriod(client,&normal,&minimum);
            if(FAILED(result))return result;
            auto period=std::max<REFERENCE_TIME>(std::max(normal,minimum),200000);
            auto frames=(static_cast<std::uint64_t>(format->nSamplesPerSec)*period+9999999)/10000000;
            if(!frames||frames>UINT32_MAX)return E_INVALIDARG;
            period=FramesPeriod(static_cast<UINT32>(frames),format->nSamplesPerSec);
            IMMDeviceEnumerator* enumerator{};IMMDevice* endpoint{};
            result=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator));
            if(SUCCEEDED(result))result=enumerator->GetDevice(info.endpoint.c_str(),&endpoint);
            if(SUCCEEDED(result))result=ProbeExclusivePeriod(
                [&](IAudioClient** probe){return originalActivate(endpoint,__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(probe));},
                [&](IAudioClient* probe,DWORD probeFlags,REFERENCE_TIME probePeriod,const WAVEFORMATEX* probeFormat){return originalInit(probe,AUDCLNT_SHAREMODE_EXCLUSIVE,probeFlags,probePeriod,probePeriod,probeFormat,session);},
                format,flags,period,periodicity);
            if(endpoint)endpoint->Release();if(enumerator)enumerator->Release();
            if(FAILED(result)){Log(L"exclusive_device_period_probe_failed",result);Backend::Instance().OutputFault(result);return result;}
            duration=periodicity;
            info.period=periodicity;Log(L"exclusive_event_period_"+std::to_wstring(periodicity));
        }
        {std::lock_guard guard(clientMutex);clients[client]=info;}
    }
    HRESULT result=originalInit(client,mode,flags,duration,periodicity,format,session);
    if(tracked) {
        info.initialized=SUCCEEDED(result);UINT32 frames{};
        if(info.initialized){HRESULT sizeResult=client->GetBufferSize(&frames);if(FAILED(sizeResult)||!frames){result=FAILED(sizeResult)?sizeResult:E_UNEXPECTED;info.initialized=false;}else if(info.exclusive)info.period=FramesPeriod(frames,info.format.Format.nSamplesPerSec);}info.bufferFrames=frames;
        {std::lock_guard guard(clientMutex);clients[client]=info;}
        Backend::Instance().Initialized(client,result,frames,info.endpoint);
        Backend::Instance().OpenEnd(client,SUCCEEDED(result),client,nullptr,&info.format);
        Log(L"actual_initialize_result",result);
    }
    return result;
}
HRESULT STDMETHODCALLTYPE HookBuffer(IAudioRenderClient* render,UINT32 frames,BYTE** data) {
    HRESULT result=originalBuffer(render,frames,data);
    static bool first{};if(!first){first=true;Log(L"first_render_buffer_"+std::to_wstring(frames),result);}
    if(SUCCEEDED(result)&&data){std::lock_guard guard(clientMutex);auto found=renders.find(render);if(found!=renders.end()){
        auto& packet=found->second;packet.buffer=*data;packet.frames=frames;
        if(packet.convertPcm){
            try {packet.scratch.resize(static_cast<std::size_t>(frames)*(packet.blockAlign/(packet.containerBits/8))*4);}
            catch(const std::bad_alloc&) {
                // Cancel the acquired packet before returning an allocation fault
                // across the COM boundary; the native writer must not see a
                // smaller hardware buffer as a 32-bit conversion destination.
                originalSubmit(render,0,0);packet.buffer=nullptr;packet.frames=0;
                *data=nullptr;Log(L"pcm_scratch_allocation_failed",E_OUTOFMEMORY);return E_OUTOFMEMORY;
            }
            *data=packet.scratch.data();
        }
    }}
    if(FAILED(result))Log(L"render_get_buffer_failed",result);return result;
}
HRESULT STDMETHODCALLTYPE HookSubmit(IAudioRenderClient* render,UINT32 frames,DWORD flags) {
    bool tracked=false,signal=false;
    {std::lock_guard guard(clientMutex);auto found=renders.find(render);
        if(found!=renders.end()){tracked=true;auto& packet=found->second;
            if(packet.buffer && !(flags&AUDCLNT_BUFFERFLAGS_SILENT)) {
                if(packet.convertPcm&&frames<=packet.frames){
                    const auto sampleBytes=packet.containerBits/8;
                    const auto samples=static_cast<std::size_t>(frames)*(packet.blockAlign/sampleBytes);
                    for(std::size_t i=0;i<samples;++i){std::int32_t sample{};std::memcpy(&sample,packet.scratch.data()+i*4,4);
                        StoreWasapiInteger(packet.buffer+i*sampleBytes,packet.containerBits,packet.validBits,sample);}
                }
                auto bytes=std::min<std::size_t>(static_cast<std::size_t>(std::min(frames,packet.frames))*packet.blockAlign,512);
                for(std::size_t i=0;i<bytes;++i)if(packet.buffer[i]){signal=true;break;}
            }
            packet.buffer=nullptr;
        }
    }
    HRESULT result=originalSubmit(render,frames,flags);
    if(tracked&&SUCCEEDED(result)&&frames){
        void* owner{};ClientInfo info;
        {std::lock_guard guard(clientMutex);auto packet=renders.find(render);if(packet!=renders.end())owner=packet->second.client;}
        if(owner&&Client(owner,info)){
            std::uint64_t played{};
            if(info.running){const auto clockResult=PlayedFrames(static_cast<IAudioClient*>(owner),info.format.Format.nSamplesPerSec,played);if(FAILED(clockResult)){Log(L"submit_endpoint_clock_failed",clockResult);result=clockResult;}}
            std::lock_guard guard(clientMutex);auto found=clients.find(owner);
            if(found!=clients.end()&&SUCCEEDED(result)){auto& state=found->second;state.clock.Submit(played,frames);++state.submitCalls;state.lastSubmitPlayed=played;state.lastSubmitTick=GetTickCount64();}
        }
    }
    if(tracked){Backend::Instance().Submitted(render,frames,result);if(SUCCEEDED(result)&&signal)Backend::Instance().Content(render);}
    return result;
}
bool InstallRender(void* render) {
    std::lock_guard guard(renderHookMutex);auto** table=*reinterpret_cast<void***>(render);
    if(bufferTarget)return bufferTarget==table[3]&&submitTarget==table[4];
    if(MH_CreateHook(table[3],reinterpret_cast<void*>(&HookBuffer),reinterpret_cast<void**>(&originalBuffer))!=MH_OK)return false;
    if(MH_CreateHook(table[4],reinterpret_cast<void*>(&HookSubmit),reinterpret_cast<void**>(&originalSubmit))!=MH_OK){MH_RemoveHook(table[3]);return false;}
    MH_QueueEnableHook(table[3]);MH_QueueEnableHook(table[4]);
    if(MH_ApplyQueued()!=MH_OK){MH_DisableHook(table[3]);MH_DisableHook(table[4]);return false;}
    bufferTarget=table[3];submitTarget=table[4];return true;
}
HRESULT STDMETHODCALLTYPE HookService(IAudioClient* client,REFIID iid,void** output) {
    HRESULT result=originalService(client,iid,output);ClientInfo info;
    if(Client(client,info)&&info.tracked) {
        if(FAILED(result))Log(L"native_get_service_failed",result);
        if(SUCCEEDED(result)&&output&&*output&&iid==__uuidof(IAudioRenderClient)) {
            bool observable=InstallRender(*output);info.render=observable?*output:nullptr;
            {std::lock_guard guard(clientMutex);clients[client]=info;if(observable)renders[*output]={client,nullptr,0,info.format.Format.nBlockAlign,info.format.Format.wBitsPerSample,info.format.Format.wFormatTag==WAVE_FORMAT_EXTENSIBLE?info.format.Samples.wValidBitsPerSample:info.format.Format.wBitsPerSample,info.convertPcm,{}};}
            Backend::Instance().OpenEnd(client,true,observable?client:nullptr,info.render,&info.format);
            Log(observable?L"render_observer_ready":L"render_observer_failed",observable?S_OK:E_NOINTERFACE);
        }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE HookStart(IAudioClient* client) {
    HRESULT result=originalStart(client);ClientInfo info;
    if(Client(client,info)&&info.tracked){info.running=SUCCEEDED(result);++info.starts;PlayedFrames(client,info.format.Format.nSamplesPerSec,info.lastStartPlayed);{std::lock_guard guard(clientMutex);clients[client]=info;}Backend::Instance().Started(client,result);Log(L"timing_start_"+ActualAudioStatusJson());}return result;
}
HRESULT STDMETHODCALLTYPE HookStop(IAudioClient* client) {
    ClientInfo before;if(Client(client,before)){std::lock_guard guard(clientMutex);clients[client].running=false;}
    HRESULT result=originalStop(client);ClientInfo info;
    if(Client(client,info)&&info.tracked){std::uint64_t played{};PlayedFrames(client,info.format.Format.nSamplesPerSec,played);{std::lock_guard guard(clientMutex);++clients[client].stops;clients[client].lastStopPlayed=played;}Backend::Instance().Stopped(client);Log(L"timing_stop_"+ActualAudioStatusJson());}return result;
}
HRESULT STDMETHODCALLTYPE HookReset(IAudioClient* client) {
    HRESULT result=originalReset(client);
    if(SUCCEEDED(result)){std::lock_guard guard(clientMutex);auto found=clients.find(client);if(found!=clients.end()){found->second.clock.Reset();found->second.clockSeen=false;found->second.lastPlayed=0;++found->second.resets;}}
    return result;
}
HRESULT STDMETHODCALLTYPE HookEvent(IAudioClient* client,HANDLE event) {
    HRESULT result=originalEvent(client,event);ClientInfo info;
    if(SUCCEEDED(result)&&Client(client,info)&&info.tracked){std::lock_guard guard(clientMutex);clients[client].event=event;}
    return result;
}
ULONG STDMETHODCALLTYPE HookRelease(IAudioClient* client) {
    ClientInfo info;bool known=Client(client,info);ULONG references=originalRelease(client);
    if(!references&&known) {
        {std::lock_guard guard(clientMutex);clients.erase(client);if(info.render)renders.erase(info.render);}
        if(info.tracked)Backend::Instance().Closed(client);
    }
    return references;
}
HRESULT STDMETHODCALLTYPE HookActivate(IMMDevice* device,REFIID iid,DWORD context,PROPVARIANT* activation,void** output) {
    HRESULT result=originalActivate(device,iid,context,activation,output);
    if(SUCCEEDED(result)&&output&&*output&&(iid==__uuidof(IAudioClient)||iid==__uuidof(IAudioClient2)||iid==__uuidof(IAudioClient3))) {
        IMMEndpoint* endpoint{};EDataFlow flow=eAll;
        if(SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint),reinterpret_cast<void**>(&endpoint)))){endpoint->GetDataFlow(&flow);endpoint->Release();}
        if(flow==eRender&&nativeOpening&&Backend::Instance().Enabled()&&!AsioSelection().empty()&&iid==__uuidof(IAudioClient)) {
            auto* native=static_cast<IAudioClient*>(*output);IAudioClient* asio{};
            HRESULT status=CreateAsioClient(native,&asio);
            if(FAILED(status)){native->Release();*output=nullptr;Backend::Instance().OutputFault(status);Log(L"asio_driver_unavailable",status);return status;}
            *output=asio;return S_OK;
        }
        if(flow==eRender&&nativeOpening&&Backend::Instance().Enabled()&&iid==__uuidof(IAudioClient)) {
            LPWSTR id{};const HRESULT identified=device->GetId(&id);
            if(FAILED(identified)||!id){static_cast<IAudioClient*>(*output)->Release();*output=nullptr;return FAILED(identified)?identified:E_UNEXPECTED;}
            const std::wstring identifier=id;CoTaskMemFree(id);IAudioClient* proxy{};
            const auto created=CreateWasapiClient(static_cast<IAudioClient*>(*output),identifier,&proxy);
            if(FAILED(created)){static_cast<IAudioClient*>(*output)->Release();*output=nullptr;Backend::Instance().OutputFault(created);return created;}
            *output=proxy;return S_OK;
        }
        if(flow==eRender){LPWSTR id{};ClientInfo info;if(SUCCEEDED(device->GetId(&id))&&id){info.endpoint=id;CoTaskMemFree(id);}
            std::lock_guard guard(clientMutex);clients[*output]=std::move(info);}
    }
    return result;
}
}
std::wstring ActualAudioStatusJson() {
    std::vector<ClientInfo> snapshots;
    {std::lock_guard guard(clientMutex);for(const auto& [key,state]:clients)if(state.tracked&&state.exclusive)snapshots.push_back(state);}
    std::wstring result=L"[";
    for(const auto& state:snapshots){if(result.size()>1)result+=L",";
        result+=L"{\"running\":"+std::wstring(state.running?L"true":L"false")+L",\"played\":"+std::to_wstring(state.lastPlayed)+
            L",\"committed\":"+std::to_wstring(state.clock.committed)+L",\"paddingCalls\":"+std::to_wstring(state.paddingCalls)+
            L",\"blockedCalls\":"+std::to_wstring(state.blockedCalls)+L",\"readyCalls\":"+std::to_wstring(state.readyCalls)+
            L",\"submitCalls\":"+std::to_wstring(state.submitCalls)+L",\"starts\":"+std::to_wstring(state.starts)+
            L",\"stops\":"+std::to_wstring(state.stops)+L",\"resets\":"+std::to_wstring(state.resets)+
            L",\"lastStartPlayed\":"+std::to_wstring(state.lastStartPlayed)+L",\"lastStopPlayed\":"+std::to_wstring(state.lastStopPlayed)+
            L",\"lastSubmitPlayed\":"+std::to_wstring(state.lastSubmitPlayed)+L",\"lastSubmitTick\":"+std::to_wstring(state.lastSubmitTick)+L"}";
    }
    return result+L"]";
}
void PrepareNativeAsioFormat(const WAVEFORMATEX& format) {
    if(!nativeStream||!nativeProfile)return;
    auto* wave=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(static_cast<unsigned char*>(nativeStream)+0x130);
    *wave={};wave->Format=format;wave->Samples.wValidBitsPerSample=32;wave->dwChannelMask=3;
    wave->SubFormat={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    using ConvertFn=void(*)(const void*,void*);
    auto convert=reinterpret_cast<ConvertFn>(reinterpret_cast<unsigned char*>(GetModuleHandleW(L"cloudmusic.dll"))+nativeProfile->convertRva);
    convert(wave,static_cast<unsigned char*>(nativeStream)+0xe8);
}
bool InstallActualNativeHooks(HMODULE module) {
    const auto hash=FileHash(ModulePath(module));
    for(const auto& profile:kClientProfiles)if(hash==profile.nativeHash){nativeProfile=&profile;break;}
    if(!nativeProfile)return false;
    auto* target=reinterpret_cast<unsigned char*>(module)+nativeProfile->openRva;
    const unsigned char expected[]={0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0xb8,0x90,0x1f};
    if(std::memcmp(target,expected,sizeof(expected)))return false;
    bool ok=MH_CreateHook(target,reinterpret_cast<void*>(&HookOpen),reinterpret_cast<void**>(&originalOpen))==MH_OK&&MH_EnableHook(target)==MH_OK;
    Backend::Instance().Compatibility(ok);Log(ok?L"actual_native_hook_ready":L"actual_native_hook_failed");
    if(ok)Log(L"native_profile_"+std::wstring(nativeProfile->version));return ok;
}
bool InstallActualAudioObserver() {
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator{};IMMDevice* device{};IAudioClient* client{};bool ok=false;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator)))&&
       SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&device))&&
       SUCCEEDED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client)))) {
        auto** d=*reinterpret_cast<void***>(device);auto** c=*reinterpret_cast<void***>(client);
        struct Site {void* target;void* hook;void** original;};
        Site sites[]={{d[3],reinterpret_cast<void*>(&HookActivate),reinterpret_cast<void**>(&originalActivate)},
            {c[3],reinterpret_cast<void*>(&HookInitialize),reinterpret_cast<void**>(&originalInit)},
            {c[14],reinterpret_cast<void*>(&HookService),reinterpret_cast<void**>(&originalService)},
            {c[10],reinterpret_cast<void*>(&HookStart),reinterpret_cast<void**>(&originalStart)},
            {c[11],reinterpret_cast<void*>(&HookStop),reinterpret_cast<void**>(&originalStop)},
            {c[12],reinterpret_cast<void*>(&HookReset),reinterpret_cast<void**>(&originalReset)},
            {c[2],reinterpret_cast<void*>(&HookRelease),reinterpret_cast<void**>(&originalRelease)},
            {c[6],reinterpret_cast<void*>(&HookPadding),reinterpret_cast<void**>(&originalPadding)},
            {c[9],reinterpret_cast<void*>(&HookPeriod),reinterpret_cast<void**>(&originalPeriod)},
            {c[13],reinterpret_cast<void*>(&HookEvent),reinterpret_cast<void**>(&originalEvent)}};
        std::size_t created{};
        for(auto& site:sites){if(MH_CreateHook(site.target,site.hook,site.original)!=MH_OK)break;++created;MH_QueueEnableHook(site.target);}
        ok=created==std::size(sites)&&MH_ApplyQueued()==MH_OK;
        if(!ok)for(std::size_t i=0;i<created;++i){MH_DisableHook(sites[i].target);MH_RemoveHook(sites[i].target);}
    }
    if(client)client->Release();if(device)device->Release();if(enumerator)enumerator->Release();
    if(SUCCEEDED(com))CoUninitialize();Log(ok?L"actual_audio_observer_ready":L"actual_audio_observer_failed");return ok;
}
}
