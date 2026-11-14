#include "Backend.h"
#include "AsioHost.h"
#include <MinHook.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

namespace nce {
namespace {
std::wstring EndpointName(IMMDevice* device) {
    IPropertyStore* properties{};std::wstring name;
    if(device&&SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties))) {
        PROPVARIANT value{};
        if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)name=value.pwszVal;
        PropVariantClear(&value);properties->Release();
    }
    return name;
}
std::wstring WasapiDeviceJson(const std::wstring& endpoint) {
    // UI status lookup only: no audio client is activated or initialized.
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator{};IMMDevice* device{};
    std::wstring name,defaultName,defaultEndpoint;
    if((SUCCEEDED(com)||com==RPC_E_CHANGED_MODE)&&SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator)))) {
        if(!endpoint.empty()&&!endpoint.starts_with(L"ASIO:")&&SUCCEEDED(enumerator->GetDevice(endpoint.c_str(),&device))) {
            name=EndpointName(device);device->Release();device=nullptr;
        }
        if(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&device))) {
            defaultName=EndpointName(device);LPWSTR id{};
            if(SUCCEEDED(device->GetId(&id))&&id){defaultEndpoint=id;CoTaskMemFree(id);}
            device->Release();
        }
        enumerator->Release();
    }
    if(SUCCEEDED(com))CoUninitialize();
    return L"{\"name\":"+JsonString(name)+L",\"defaultName\":"+JsonString(defaultName)+
        L",\"defaultEndpoint\":"+JsonString(defaultEndpoint)+L"}";
}
}
Backend& Backend::Instance(){static Backend value;return value;}
void Backend::LoadConfig(){enabled_.store(GetPrivateProfileIntW(L"exclusive",L"enabled",0,ConfigPath().c_str())==1);}
bool Backend::SetEnabled(bool enabled) {
    std::lock_guard configGuard(configMutex_);
    {std::lock_guard guard(mutex_);if(enabled && !supported_) return false;}
    if(!WritePrivateProfileStringW(L"exclusive",L"enabled",enabled?L"1":L"0",ConfigPath().c_str()))return false;
    {std::lock_guard guard(mutex_);enabled_.store(enabled,std::memory_order_release);if(!enabled)lastFault_=S_OK;++sequence_;}
    Log(enabled?L"requested_exclusive":L"requested_shared");return true;
}
void Backend::Compatibility(bool value){std::lock_guard configGuard(configMutex_);std::lock_guard guard(mutex_);supported_=value;++sequence_;}
void Backend::UiReady(bool value){std::lock_guard guard(mutex_);uiReady_=value;++sequence_;}
void Backend::OutputFault(HRESULT error){std::lock_guard guard(mutex_);lastFault_=error;++sequence_;}
void Backend::OpenBegin(void* stream,bool exclusive) {
    std::lock_guard guard(mutex_);auto& state=streams_[stream];state={};state.exclusive=exclusive;lastFault_=S_OK;state.generation=++sequence_;
}
void Backend::Initialized(void* stream,HRESULT result,std::uint32_t frames,std::wstring endpoint) {
    bool exclusive{};
    {std::lock_guard guard(mutex_);auto& state=streams_[stream];state.lastError=result;state.frames=frames;
    state.endpoint=std::move(endpoint);++sequence_;
    if(FAILED(result))lastFault_=result;
    exclusive=state.exclusive;}
    Log(exclusive?L"exclusive_initialize":L"shared_initialize",result);
}
void Backend::OpenEnd(void* stream,bool result,void* client,void* render,const void* rawFormat) {
    HRESULT error{};
    {std::lock_guard guard(mutex_);auto& state=streams_[stream];state.opened=result;
    state.client=client;state.render=render;
    if(!result&&SUCCEEDED(state.lastError))state.lastError=E_FAIL;
    if(!result)lastFault_=state.lastError;
    if(rawFormat){auto* format=static_cast<const WAVEFORMATEX*>(rawFormat);
        state.rate=format->nSamplesPerSec;state.bits=format->wBitsPerSample;state.channels=format->nChannels;}
    ++sequence_;error=state.lastError;}
    Log(result?L"stream_open":L"stream_open_failed",error);
}
void Backend::Started(void* client,HRESULT result) {
    bool found{};{std::lock_guard guard(mutex_);
    for(auto& [key,state]:streams_)if(state.client==client){state.lastError=result;state.running=SUCCEEDED(result);if(FAILED(result))lastFault_=result;++sequence_;found=true;}}
    if(found)Log(L"audio_client_start",result);
}
void Backend::Submitted(void* render,std::uint32_t frames,HRESULT result) {
    bool failed{};{std::lock_guard guard(mutex_);
    for(auto& [key,state]:streams_)if(state.render==render){
        if(SUCCEEDED(result))state.submitted+=frames;
        else{state.lastError=result;lastFault_=result;state.running=false;++sequence_;failed=true;}
    }}
    if(failed)Log(L"render_release_failed",result);
}
void Backend::Content(void* render){std::lock_guard guard(mutex_);for(auto& [key,state]:streams_)if(state.render==render)++state.nonSilentPackets;}
void Backend::Stopped(void* stream){{std::lock_guard guard(mutex_);auto i=streams_.find(stream);if(i!=streams_.end()){i->second.running=false;++sequence_;}}Log(L"stream_stop");}
void Backend::Closed(void* stream){{std::lock_guard guard(mutex_);streams_.erase(stream);++sequence_;}Log(L"stream_close");}
std::wstring Backend::StatusJson() {
    std::unique_lock guard(mutex_);
    bool enabled=Enabled(),pending=false,opened=false,active=false;std::uint64_t frames=0,nonSilent=0;
    HRESULT error=lastFault_;const StreamState* representative=nullptr;
    for(auto& [key,state]:streams_) {
        if(!representative || (state.running&&!representative->running) ||
           (state.running==representative->running && state.generation>representative->generation))representative=&state;
    }
    if(representative){const auto& state=*representative;
        pending=state.opened&&state.exclusive!=enabled;opened=state.opened;
        active=state.opened&&state.running&&state.exclusive&&state.submitted>0;
        frames=state.submitted;nonSilent=state.nonSilentPackets;if(FAILED(state.lastError))error=state.lastError;
    }
    const wchar_t* phase=!supported_?L"unsupported":FAILED(error)?L"faulted":active?L"active":
        pending?L"pending_reopen":opened?(representative&&representative->exclusive?L"open":L"shared"):
        enabled?L"armed":L"disabled";
    std::wstring result=L"{\"enabled\":"+std::wstring(enabled?L"true":L"false")+
        L",\"supported\":"+(supported_?L"true":L"false")+L",\"uiReady\":"+(uiReady_?L"true":L"false")+
        L",\"pendingReopen\":"+(pending?L"true":L"false")+L",\"phase\":"+JsonString(phase)+
        L",\"hresult\":"+std::to_wstring(static_cast<unsigned long>(error))+
        L",\"submittedFrames\":"+std::to_wstring(frames)+L",\"nonSilentPackets\":"+std::to_wstring(nonSilent)+L",\"sequence\":"+std::to_wstring(sequence_);
    if(representative)result+=L",\"sampleRate\":"+std::to_wstring(representative->rate)+L",\"containerBits\":"+
        std::to_wstring(representative->bits)+L",\"channels\":"+std::to_wstring(representative->channels)+
        L",\"bufferFrames\":"+std::to_wstring(representative->frames)+L",\"endpoint\":"+JsonString(representative->endpoint);
    const auto endpoint=representative?representative->endpoint:std::wstring{};
    // Property/driver queries must not hold the audio telemetry mutex.
    guard.unlock();
    return result+L",\"wasapi\":"+WasapiDeviceJson(endpoint)+L",\"asio\":"+AsioStatusJson()+L"}";
}

}
