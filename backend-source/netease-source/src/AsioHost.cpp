// SPDX-License-Identifier: GPL-3.0-only
#include "Backend.h"
#include "AsioHost.h"
#include "AsioPcm.h"
#include "DevicePolicy.h"
#include "iasiodrv.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <vector>
#include <mutex>
#include <cmath>
#include <new>

namespace nce { namespace {
struct Driver {std::wstring id,name;bool compatible;};
std::vector<Driver> Drivers() {
    std::vector<Driver> result;
    for(HKEY root:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) for(REGSAM view:{KEY_WOW64_64KEY,KEY_WOW64_32KEY}) {
        HKEY key{};if(RegOpenKeyExW(root,L"SOFTWARE\\ASIO",0,KEY_READ|view,&key)!=ERROR_SUCCESS)continue;
        for(DWORD index=0;;++index){wchar_t name[256]{};DWORD length=256;
            if(RegEnumKeyExW(key,index,name,&length,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS)break;
            HKEY item{};if(RegOpenKeyExW(key,name,0,KEY_READ|view,&item)!=ERROR_SUCCESS)continue;
            wchar_t id[100]{},description[512]{};DWORD idSize=sizeof(id),descSize=sizeof(description);
            LONG found=RegGetValueW(item,nullptr,L"CLSID",RRF_RT_REG_SZ,nullptr,id,&idSize);
            RegGetValueW(item,nullptr,L"Description",RRF_RT_REG_SZ,nullptr,description,&descSize);RegCloseKey(item);
            CLSID guid{};if(found!=ERROR_SUCCESS||FAILED(CLSIDFromString(id,&guid)))continue;
            wchar_t canonical[40]{};StringFromGUID2(guid,canonical,40);
            bool compatible=view==KEY_WOW64_64KEY;
            auto old=std::find_if(result.begin(),result.end(),[&](const Driver& d){return d.id==canonical;});
            if(old==result.end())result.push_back({canonical,description[0]?description:name,compatible});
            else old->compatible|=compatible;
        }RegCloseKey(key);
    }return result;
}
class Client;
std::atomic<Client*> current{};
std::mutex lifecycle;
bool Good(ASIOError e){return e==ASE_OK||e==ASE_SUCCESS;}
class Client final: public IAudioClient, public IAudioRenderClient, public IAudioClock {
    std::atomic<ULONG> refs{1};IAudioClient* native{};IASIO* driver{};
    std::wstring id,name;bool initialized{},buffers{},started{},reserved{};
    WAVEFORMATEXTENSIBLE wave{};UINT32 capacity{},acquired{};long period{};
    std::vector<std::int32_t> ring,packet;
    alignas(64) std::atomic<std::uint64_t> read{},write{};
    std::atomic<HANDLE> event{};std::atomic<bool> fault{},faultReported{};
    std::atomic<std::uint64_t> clockFrames{},driverNanoseconds{},clockQpc{},clockSequence{};
    std::atomic<int> clockSource{};std::atomic<long> driverError{ASE_OK};
    DeviceClockProgress clockProgress;std::uint64_t clockOffset{};
    std::mutex capabilityMutex;
    long minimumFrames{},maximumFrames{},preferredFrames{},bufferStep{},outputChannels{};
    bool outputReadyAvailable{};
    std::vector<unsigned> supportedRates;std::vector<ASIOClockSource> clockSources;
    static constexpr unsigned commonRates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000,352800,384000,705600,768000};
    std::array<ASIOBufferInfo,2> buffer{};std::array<ASIOChannelInfo,2> channel{};
    ASIOCallbacks callbacks{};
    static void Switch(long index,ASIOBool){if(auto* self=current.load(std::memory_order_acquire))self->Render(index,nullptr);}
    static ASIOTime* Time(ASIOTime* t,long i,ASIOBool){if(auto* self=current.load(std::memory_order_acquire))self->Render(i,t);return t;}
    static void Rate(ASIOSampleRate rate){if(auto* self=current.load())if(!std::isfinite(rate)||std::abs(rate-self->wave.Format.nSamplesPerSec)>0.5)self->Fail(ASE_NoClock);}
    static long Message(long selector,long value,void*,double*) {
        if(selector==kAsioSelectorSupported)return value==kAsioEngineVersion||value==kAsioResetRequest||value==kAsioResyncRequest||value==kAsioSupportsTimeInfo||value==kAsioLatenciesChanged;
        if(selector==kAsioEngineVersion)return 2;
        if(selector==kAsioSupportsTimeInfo)return 1;
        if(selector==kAsioResetRequest||selector==kAsioResyncRequest||selector==kAsioLatenciesChanged){if(auto* s=current.load())s->Fail(ASE_InvalidMode);return 1;}
        return 0;
    }
    void Fail(long reason) noexcept {driverError.store(reason);fault.store(true);if(auto h=event.load())SetEvent(h);}
    HRESULT ReportFault(){if(!faultReported.exchange(true)){Backend::Instance().OutputFault(AUDCLNT_E_DEVICE_INVALIDATED);Log(L"asio_device_clock_or_reset_fault",static_cast<HRESULT>(driverError.load()));}return AUDCLNT_E_DEVICE_INVALIDATED;}
    template<class T> static std::uint64_t Wide(T value) noexcept {return (std::uint64_t{value.hi}<<32)|value.lo;}
    bool ReadClock(const ASIOTime* time) noexcept {
        ASIOSamples position{};ASIOTimeStamp timestamp{};int source=1;
        if(time&&(time->timeInfo.flags&kSampleRateValid))Rate(time->timeInfo.sampleRate);
        if(fault.load())return false;
        if(time&&(time->timeInfo.flags&kSamplePositionValid)&&(time->timeInfo.flags&kSystemTimeValid)){
            position=time->timeInfo.samplePosition;timestamp=time->timeInfo.systemTime;source=2;
        }else{
            const auto result=driver->getSamplePosition(&position,&timestamp);
            if(!Good(result)){Fail(result);return false;}
        }
        std::uint64_t relative{};
        const auto stallLimit=std::max<unsigned>(3u,(wave.Format.nSamplesPerSec+static_cast<unsigned>(period)*10-1)/(static_cast<unsigned>(period)*10));
        if(!clockProgress.Update(Wide(position),relative,stallLimit)){Fail(ASE_SPNotAdvancing);return false;}
        LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
        const auto qpc=(now.QuadPart/frequency.QuadPart)*10000000ULL+(now.QuadPart%frequency.QuadPart)*10000000ULL/frequency.QuadPart;
        clockSequence.fetch_add(1);clockFrames.store(clockOffset+relative);clockQpc.store(qpc);clockSequence.fetch_add(1);
        driverNanoseconds.store(Wide(timestamp));clockSource.store(source);
        return true;
    }
    void Render(long index,const ASIOTime* time) noexcept {
        if(index<0||index>1)return;
        if(!fault.load())ReadClock(time);
        auto r=read.load(std::memory_order_relaxed);const auto w=write.load(std::memory_order_acquire);
        const auto available=fault.load()?0:std::min<std::uint64_t>(w-r,static_cast<unsigned>(period));
        for(unsigned c=0;c<2;++c){auto* destination=static_cast<unsigned char*>(buffer[c].buffers[index]);
            const auto bytes=AsioSampleBytes(channel[c].type);
            if(!destination)return;
            for(long f=0;f<period;++f){const auto sample=static_cast<std::uint64_t>(f)<available?ring[((r+f)%capacity)*2+c]:0;
                StoreAsioSample(destination+f*bytes,channel[c].type,sample);}
        }
        read.store(r+available,std::memory_order_release);
        if(available<static_cast<unsigned>(period))underruns.fetch_add(1,std::memory_order_relaxed);
        consumed.fetch_add(available,std::memory_order_relaxed);
        if(outputReadyAvailable)driver->outputReady();if(auto h=event.load(std::memory_order_acquire))SetEvent(h);
    }
    static bool SupportedInput(const WAVEFORMATEX* f) noexcept {
        if(!f||f->nChannels!=2||f->nSamplesPerSec<8000||f->nSamplesPerSec>768000)return false;
        if(f->wBitsPerSample!=16&&f->wBitsPerSample!=24&&f->wBitsPerSample!=32)return false;
        if(f->nBlockAlign!=f->nChannels*f->wBitsPerSample/8||f->nAvgBytesPerSec!=f->nSamplesPerSec*f->nBlockAlign)return false;
        if(f->wFormatTag==WAVE_FORMAT_PCM)return true;
        if(f->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)return f->wBitsPerSample==32;
        if(f->wFormatTag!=WAVE_FORMAT_EXTENSIBLE||f->cbSize<22)return false;
        auto* x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        const GUID pcm={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}},floating={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        if(x->dwChannelMask!=0&&x->dwChannelMask!=3)return false;
        if(x->Samples.wValidBitsPerSample==0||x->Samples.wValidBitsPerSample>f->wBitsPerSample)return false;
        return x->SubFormat==pcm||(x->SubFormat==floating&&f->wBitsPerSample==32&&x->Samples.wValidBitsPerSample==32);
    }
    HRESULT EnsureDriver() {
        std::lock_guard lock(lifecycle);if(driver)return S_OK;if(current.load()&&current.load()!=this)return AUDCLNT_E_DEVICE_IN_USE;
        current.store(this);reserved=true;
        const auto unreserve=[this](){if(current.load()==this)current.store(nullptr);reserved=false;};
        CLSID clsid{};if(FAILED(CLSIDFromString(id.c_str(),&clsid))){unreserve();return E_INVALIDARG;}
        HRESULT hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,clsid,reinterpret_cast<void**>(&driver));
        if(FAILED(hr)){unreserve();return hr;}
        if(!driver){unreserve();return E_POINTER;}
        if(!driver->init(GetDesktopWindow())){driver->Release();driver=nullptr;unreserve();return E_FAIL;}
        return S_OK;
    }
    HRESULT CheckDriverFormat(const WAVEFORMATEX* format) {
        if(!SupportedInput(format))return AUDCLNT_E_UNSUPPORTED_FORMAT;
        auto hr=EnsureDriver();if(FAILED(hr))return hr;
        long inputs{},outputs{};
        if(!Good(driver->getChannels(&inputs,&outputs))||outputs<2)return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if(!Good(driver->canSampleRate(format->nSamplesPerSec)))return AUDCLNT_E_UNSUPPORTED_FORMAT;
        for(long c=0;c<2;++c){ASIOChannelInfo info{};info.channel=c;
            if(!Good(driver->getChannelInfo(&info))||!AsioSampleBytes(info.type))return AUDCLNT_E_UNSUPPORTED_FORMAT;}
        return S_OK;
    }
    HRESULT InitDriver(const WAVEFORMATEX* format) {
        if(initialized)return AUDCLNT_E_ALREADY_INITIALIZED;
        HRESULT hr=CheckDriverFormat(format);if(FAILED(hr))return hr;
        std::lock_guard capabilities(capabilityMutex);
        long inputs{};if(!Good(driver->getChannels(&inputs,&outputChannels)))return E_FAIL;
        const double rate=format->nSamplesPerSec; wave.Format.nSamplesPerSec=format->nSamplesPerSec;
        if(!Good(driver->canSampleRate(rate))||!Good(driver->setSampleRate(rate)))return AUDCLNT_E_UNSUPPORTED_FORMAT;
        double actual{};if(!Good(driver->getSampleRate(&actual))||!std::isfinite(actual)||std::abs(actual-rate)>0.5)return AUDCLNT_E_UNSUPPORTED_FORMAT;
        supportedRates.clear();clockSources.clear();
        for(unsigned candidate:commonRates)if(Good(driver->canSampleRate(candidate)))supportedRates.push_back(candidate);
        std::array<ASIOClockSource,32> sources{};long count=static_cast<long>(sources.size());
        if(Good(driver->getClockSources(sources.data(),&count))&&count>=0&&count<=static_cast<long>(sources.size()))clockSources.assign(sources.begin(),sources.begin()+count);
        // Preserve the driver's selected internal/external clock source.
        if(!Good(driver->getBufferSize(&minimumFrames,&maximumFrames,&preferredFrames,&bufferStep)))return E_FAIL;
        period=SelectAsioPeriod(minimumFrames,maximumFrames,preferredFrames,bufferStep);if(!period)return E_FAIL;
        wave={};wave.Format={WAVE_FORMAT_EXTENSIBLE,2,format->nSamplesPerSec,format->nSamplesPerSec*8,8,32,22};
        wave.Samples.wValidBitsPerSample=32;wave.dwChannelMask=3;wave.SubFormat={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        capacity=static_cast<UINT32>(period)*4;
        ring.resize(static_cast<std::size_t>(capacity)*2);packet.resize(ring.size());
        for(long c=0;c<2;++c){channel[c].channel=c;channel[c].isInput=ASIOFalse;
            if(!Good(driver->getChannelInfo(&channel[c]))||!AsioSampleBytes(channel[c].type))return AUDCLNT_E_UNSUPPORTED_FORMAT;
            buffer[c].isInput=ASIOFalse;buffer[c].channelNum=c;}
        callbacks={Switch,Rate,Message,Time};
        if(!Good(driver->createBuffers(buffer.data(),2,period,&callbacks)))return E_FAIL;
        buffers=true;
        for(const auto& output:buffer)for(void* pointer:output.buffers)if(!pointer)return E_POINTER;
        outputReadyAvailable=Good(driver->outputReady());initialized=true;
        PrepareNativeAsioFormat(wave.Format);
        return S_OK;
    }
public:
    std::atomic<std::uint64_t> underruns{},consumed{};
    Client(IAudioClient* original,std::wstring selected,std::wstring label):native(original),id(std::move(selected)),name(std::move(label)){}
    ~Client(){Stop();if(buffers)driver->disposeBuffers();{std::lock_guard lock(lifecycle);if(reserved)current.store(nullptr);}if(driver)driver->Release();native->Release();Backend::Instance().Closed(static_cast<IAudioClient*>(this));}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IAudioClient))*out=static_cast<IAudioClient*>(this);
        else if(iid==__uuidof(IAudioRenderClient))*out=static_cast<IAudioRenderClient*>(this);
        else if(iid==__uuidof(IAudioClock))*out=static_cast<IAudioClock*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX* format,LPCGUID) override {
        Backend::Instance().OpenBegin(static_cast<IAudioClient*>(this),true);
        HRESULT hr;try{hr=InitDriver(format);}catch(const std::bad_alloc&){hr=E_OUTOFMEMORY;}
        Backend::Instance().Initialized(static_cast<IAudioClient*>(this),hr,capacity,L"ASIO: "+name);
        Backend::Instance().OpenEnd(static_cast<IAudioClient*>(this),SUCCEEDED(hr),static_cast<IAudioClient*>(this),static_cast<IAudioRenderClient*>(this),&wave);
        Log(L"asio_initialize_"+name,hr);return hr;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* out) override{if(!out)return E_POINTER;if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;*out=capacity;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* out) override{if(!out)return E_POINTER;if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;long in{},latency{};if(!Good(driver->getLatencies(&in,&latency))||latency<0)return E_FAIL;*out=10000000LL*(static_cast<std::int64_t>(latency)+capacity)/wave.Format.nSamplesPerSec;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* out) override{if(!out)return E_POINTER;if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(fault.load())return ReportFault();const auto r=read.load(std::memory_order_acquire);const auto w=write.load(std::memory_order_acquire);*out=static_cast<UINT32>(std::min<std::uint64_t>(w-r,capacity));return S_OK;}
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE,const WAVEFORMATEX* f,WAVEFORMATEX** closest) override{if(closest)*closest=nullptr;return CheckDriverFormat(f);}
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** out) override{return native->GetMixFormat(out);}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal,REFERENCE_TIME* minimum) override{auto p=initialized?10000000LL*period/wave.Format.nSamplesPerSec:200000;if(normal)*normal=p;if(minimum)*minimum=p;return S_OK;}
    HRESULT STDMETHODCALLTYPE Start() override{if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(!event.load())return AUDCLNT_E_EVENTHANDLE_NOT_SET;if(fault.load())return AUDCLNT_E_DEVICE_INVALIDATED;if(started)return AUDCLNT_E_NOT_STOPPED;clockOffset=clockFrames.load();clockProgress={};auto hr=Good(driver->start())?S_OK:E_FAIL;started=SUCCEEDED(hr);Backend::Instance().Started(static_cast<IAudioClient*>(this),hr);return hr;}
    HRESULT STDMETHODCALLTYPE Stop() override{if(started){auto e=driver->stop();if(!Good(e))return E_FAIL;started=false;}Backend::Instance().Stopped(static_cast<IAudioClient*>(this));return S_OK;}
    HRESULT STDMETHODCALLTYPE Reset() override{if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(started)return AUDCLNT_E_NOT_STOPPED;read.store(0);write.store(0);clockSequence.fetch_add(1);clockFrames.store(0);clockQpc.store(0);clockSequence.fetch_add(1);driverNanoseconds.store(0);clockSource.store(0);clockOffset=0;clockProgress={};acquired=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE h) override{if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(!h)return E_INVALIDARG;event.store(h);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid,void** out) override{if(!initialized){if(out)*out=nullptr;return AUDCLNT_E_NOT_INITIALIZED;}if(iid!=__uuidof(IAudioRenderClient)&&iid!=__uuidof(IAudioClock)){if(out)*out=nullptr;return E_NOINTERFACE;}return QueryInterface(iid,out);}
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* value) override{if(!value)return E_POINTER;if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;*value=wave.Format.nSamplesPerSec;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* value,UINT64* qpc) override{if(!value)return E_POINTER;if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(fault.load())return ReportFault();for(;;){const auto sequence=clockSequence.load();if(sequence&1)continue;const auto frames=clockFrames.load(),stamp=clockQpc.load();if(sequence!=clockSequence.load())continue;*value=frames;if(qpc)*qpc=stamp;break;}return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* value) override{if(!value)return E_POINTER;*value=AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames,BYTE** out) override{if(!out)return E_POINTER;*out=nullptr;if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(fault.load())return AUDCLNT_E_DEVICE_INVALIDATED;if(acquired)return AUDCLNT_E_OUT_OF_ORDER;UINT32 padding{};GetCurrentPadding(&padding);if(frames>capacity-padding)return AUDCLNT_E_BUFFER_TOO_LARGE;if(!frames)return S_OK;acquired=frames;*out=reinterpret_cast<BYTE*>(packet.data());return S_OK;}
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames,DWORD flags) override{if(!initialized)return AUDCLNT_E_NOT_INITIALIZED;if(flags&~AUDCLNT_BUFFERFLAGS_SILENT)return E_INVALIDARG;if(frames>acquired)return AUDCLNT_E_INVALID_SIZE;if(!frames){acquired=0;return S_OK;}auto w=write.load(std::memory_order_relaxed);bool signal=false;
        for(UINT32 f=0;f<frames;++f)for(unsigned c=0;c<2;++c){auto sample=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:packet[f*2+c];ring[((w+f)%capacity)*2+c]=sample;signal|=sample!=0;}
        write.store(w+frames,std::memory_order_release);acquired=0;Backend::Instance().Submitted(static_cast<IAudioRenderClient*>(this),frames,S_OK);if(signal)Backend::Instance().Content(static_cast<IAudioRenderClient*>(this));return S_OK;}
    std::wstring CapabilitiesJson(){
        std::lock_guard guard(capabilityMutex);
        std::wstring s=L",\"clockSource\":"+JsonString(clockSource.load()==2?L"ASIOTimeInfo":clockSource.load()==1?L"ASIOGetSamplePosition":L"awaiting-driver-clock")+
            L",\"clockFrames\":"+std::to_wstring(clockFrames.load())+L",\"driverNanoseconds\":"+std::to_wstring(driverNanoseconds.load())+
            L",\"driverError\":"+std::to_wstring(driverError.load())+L",\"faulted\":"+(fault.load()?L"true":L"false")+
            L",\"bufferFrames\":"+std::to_wstring(period)+L",\"bufferMin\":"+std::to_wstring(minimumFrames)+L",\"bufferMax\":"+std::to_wstring(maximumFrames)+
            L",\"bufferPreferred\":"+std::to_wstring(preferredFrames)+L",\"bufferGranularity\":"+std::to_wstring(bufferStep)+L",\"supportedCommonRates\":[";
        bool comma=false;for(auto rate:supportedRates){if(comma)s+=L",";comma=true;s+=std::to_wstring(rate);}s+=L"],\"outputSampleTypes\":[";
        s+=std::to_wstring(channel[0].type)+L","+std::to_wstring(channel[1].type)+L"],\"clockSources\":[";comma=false;
        for(const auto& source:clockSources){if(comma)s+=L",";comma=true;s+=L"{\"index\":"+std::to_wstring(source.index)+L",\"current\":"+(source.isCurrentSource?L"true":L"false")+L"}";}
        return s+L"]";
    }
};
}
std::wstring AsioSelection(){wchar_t value[80]{};GetPrivateProfileStringW(L"exclusive",L"asio_driver",L"",value,80,ConfigPath().c_str());return value;}
bool SetAsioSelection(const std::wstring& id){if(id==L"wasapi")return WritePrivateProfileStringW(L"exclusive",L"asio_driver",nullptr,ConfigPath().c_str())!=FALSE;for(const auto& d:Drivers())if(d.id==id&&d.compatible)return WritePrivateProfileStringW(L"exclusive",L"asio_driver",id.c_str(),ConfigPath().c_str())!=FALSE;return false;}
std::wstring AsioStatusJson(){std::wstring s=L"{\"selected\":"+JsonString(AsioSelection())+L",\"drivers\":[";bool comma=false;for(const auto& d:Drivers()){if(comma)s+=L",";comma=true;s+=L"{\"id\":"+JsonString(d.id)+L",\"name\":"+JsonString(d.name)+L",\"compatible\":"+(d.compatible?L"true":L"false")+L",\"capabilitiesProbed\":false,\"reason\":"+JsonString(d.compatible?L"仅确认 64 位注册；实际能力在打开选定驱动后查询":L"仅安装了 32 位 ASIO 驱动，当前网易云为 64 位")+L"}";}s+=L"]";{std::lock_guard lock(lifecycle);if(auto* c=current.load())s+=L",\"underruns\":"+std::to_wstring(c->underruns.load())+L",\"consumedFrames\":"+std::to_wstring(c->consumed.load())+c->CapabilitiesJson();}return s+L"}";}
HRESULT CreateAsioClient(IAudioClient* native,IAudioClient** out){if(!out)return E_POINTER;*out=nullptr;auto id=AsioSelection();for(const auto& d:Drivers())if(d.id==id&&d.compatible){*out=new(std::nothrow)Client(native,id,d.name);return *out?S_OK:E_OUTOFMEMORY;}return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);}
}
