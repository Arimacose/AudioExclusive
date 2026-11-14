#include <windows.h>
#include <audioclient.h>
#include <unknwn.h>
#include "iasiodrv.h"
#include <array>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include "AsioReference.h"
static ASIOCallbacks callback{};static double actualRate{};static unsigned stops{},disposes{};
static std::array<std::vector<unsigned char>,4> driverBuffers;
static Layout outputLayout=layouts[0];
static unsigned bufferPeriod=128,liveDrivers{},nativeUpdates{};
static bool rejectRate{},wrongRate{},rejectBuffers{};
static bool autoClock=true,nanRate{},outputReadySupported=true;
static std::uint64_t mockPosition{};
static ASIOError clockError=ASE_OK;
static unsigned positionQueries{},clockSourceWrites{},readyCalls{};
static long customMinimum{},customMaximum{},customPreferred{},customStep{};

class Driver final:public IASIO {
    ULONG references=1;
public:
    Driver(){++liveDrivers;}
    ~Driver(){--liveDrivers;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p)override{*p=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--references;if(!n)delete this;return n;}
    ASIOBool init(void*)override{return ASIOTrue;}
    void getDriverName(char* n)override{strcpy_s(n,32,"Test driver");}long getDriverVersion()override{return 1;}
    void getErrorMessage(char* n)override{strcpy_s(n,124,"test");}
    ASIOError start()override{mockPosition=0;return ASE_OK;}ASIOError stop()override{++stops;return ASE_OK;}
    ASIOError getChannels(long* i,long* o)override{*i=0;*o=2;return ASE_OK;}
    ASIOError getLatencies(long* i,long* o)override{*i=0;*o=bufferPeriod;return ASE_OK;}
    ASIOError getBufferSize(long* a,long* b,long* p,long* g)override{*a=customMinimum?customMinimum:bufferPeriod;*b=customMinimum?customMaximum:bufferPeriod;*p=customMinimum?customPreferred:bufferPeriod;*g=customMinimum?customStep:0;return ASE_OK;}
    ASIOError canSampleRate(double r)override{return !rejectRate&&(r==44100||r==48000||r==88200||r==96000||r==176400||r==192000)?ASE_OK:ASE_NoClock;}
    ASIOError getSampleRate(double* r)override{*r=nanRate?std::numeric_limits<double>::quiet_NaN():actualRate+(wrongRate?1:0);return ASE_OK;}
    ASIOError setSampleRate(double r)override{actualRate=r;return canSampleRate(r);}
    ASIOError getClockSources(ASIOClockSource* s,long* count)override{if(*count<2)return ASE_InvalidParameter;*count=2;s[0].index=0;s[0].isCurrentSource=ASIOFalse;s[1].index=7;s[1].isCurrentSource=ASIOTrue;return ASE_OK;}
    ASIOError setClockSource(long)override{++clockSourceWrites;return ASE_OK;}
    ASIOError getSamplePosition(ASIOSamples* p,ASIOTimeStamp* t)override{++positionQueries;p->hi=static_cast<unsigned long>(mockPosition>>32);p->lo=static_cast<unsigned long>(mockPosition);t->hi=0;t->lo=static_cast<unsigned long>(mockPosition*1000);if(autoClock)mockPosition+=bufferPeriod;return clockError;}
    ASIOError getChannelInfo(ASIOChannelInfo* c)override{c->type=static_cast<ASIOSampleType>(outputLayout.type);c->isActive=ASIOTrue;return ASE_OK;}
    ASIOError createBuffers(ASIOBufferInfo* info,long channels,long frames,ASIOCallbacks* callbacks)override{if(rejectBuffers)return ASE_NoMemory;callback=*callbacks;for(long c=0;c<channels;++c)for(unsigned b=0;b<2;++b){auto& v=driverBuffers[c*2+b];v.assign(frames*outputLayout.bytes+32,0xcc);info[c].buffers[b]=v.data()+16;}return ASE_OK;}
    ASIOError disposeBuffers()override{++disposes;return ASE_OK;}ASIOError controlPanel()override{return ASE_OK;}
    ASIOError future(long,void*)override{return ASE_NotPresent;}ASIOError outputReady()override{++readyCalls;return outputReadySupported?ASE_OK:ASE_NotPresent;}
};
static HRESULT WINAPI FakeCreate(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID* out){*out=new Driver;return S_OK;}
// Header is intentionally included once, before the host under test.
#define __ASIODRIVER_TEST_HEADER_ALREADY_INCLUDED 1
#define CoCreateInstance FakeCreate
// iasiodrv.h has no include guard. Suppress its repeated inclusion via the
// generated test copy's pragma once (upstream header itself is unchanged).
#include "AsioHostUnderTest.inc"
#undef CoCreateInstance
namespace nce {
void PrepareNativeAsioFormat(const WAVEFORMATEX& f){if(f.wBitsPerSample!=32||f.nChannels!=2)throw std::runtime_error("native format not updated");++nativeUpdates;}
std::wstring ConfigPath(){return L"unused-test-config.ini";}
void Log(std::wstring_view,HRESULT) noexcept {}
std::wstring JsonString(std::wstring_view s){return L"\""+std::wstring(s)+L"\"";}
}
// The adapter never initializes the original WASAPI client. A tiny IUnknown
// suffices here because these tests do not request its GetMixFormat service.
class Native final:public IUnknown {ULONG refs=1;public:HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**)override{return E_NOINTERFACE;}ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}};
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}

static nce::Client* NewClient(){return new nce::Client(reinterpret_cast<IAudioClient*>(new Native),L"{00000000-0000-0000-0000-000000000001}",L"mock");}
static void GuardBuffers(){for(const auto& b:driverBuffers)for(unsigned i=0;i<16;++i)check(b[i]==0xcc&&b[b.size()-1-i]==0xcc,"driver buffer overwritten");}
int main(){try{
 {
    auto* invalid=new nce::Client(reinterpret_cast<IAudioClient*>(new Native),L"invalid-driver-id",L"invalid");
    WAVEFORMATEX wave{WAVE_FORMAT_PCM,2,48000,48000*4,4,16,0};
    check(FAILED(invalid->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&wave,nullptr)),"malformed driver ID accepted");
    check(nce::current.load()==nullptr,"failed driver load retained the global callback reservation");
    auto* valid=NewClient();check(valid->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&wave,nullptr)==S_OK,"failed driver probe blocked a valid driver");
    invalid->Release();check(nce::current.load()==valid,"failed driver cleanup removed the active driver reservation");valid->Release();
    auto* fresh=NewClient();IAudioClock* clock{};BYTE* packet{};
    check(fresh->Reset()==AUDCLNT_E_NOT_INITIALIZED,"Reset accepted an uninitialized ASIO adapter");
    check(fresh->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(&clock))==AUDCLNT_E_NOT_INITIALIZED&&clock==nullptr,"GetService exposed an uninitialized ASIO clock");
    check(fresh->ReleaseBuffer(0,0)==AUDCLNT_E_NOT_INITIALIZED,"ReleaseBuffer accepted an uninitialized ASIO adapter");
    check(fresh->GetBuffer(0,&packet)==AUDCLNT_E_NOT_INITIALIZED,"GetBuffer accepted an uninitialized ASIO adapter");fresh->Release();
 }
 unsigned cases=0;std::uint64_t verifiedSamples=0;
 for(unsigned rate:{44100u,48000u,88200u,96000u,176400u,192000u})for(unsigned bits:{16u,24u,32u})for(auto layout:layouts)for(unsigned period:{64u,128u,512u,1024u}){
    outputLayout=layout;bufferPeriod=period;
    auto* client=NewClient();
    WAVEFORMATEX wave{WAVE_FORMAT_PCM,2,rate,rate*(bits/8)*2,static_cast<WORD>((bits/8)*2),static_cast<WORD>(bits),0};
    check(client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&wave,nullptr)==S_OK,"real driver format query");
    auto updatesBefore=nativeUpdates;
    check(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0,0,&wave,nullptr)==S_OK,"ASIO init");
    check(nativeUpdates==updatesBefore+1,"source format normalization request missing");
    UINT32 capacity{};check(client->GetBufferSize(&capacity)==S_OK&&capacity==period*4,"ring capacity");
    check(client->Start()==AUDCLNT_E_EVENTHANDLE_NOT_SET,"start without event accepted");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);client->SetEventHandle(event);
    IAudioClock* clock{};check(client->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(&clock))==S_OK,"ASIO clock service");
    UINT64 frequency{};check(clock->GetFrequency(&frequency)==S_OK&&frequency==rate,"ASIO clock frequency");
    std::uint64_t submitted=0,received=0;
    auto submit=[&](unsigned frames){BYTE* bytes{};check(client->GetBuffer(frames,&bytes)==S_OK,"acquire");
       auto* pcm=reinterpret_cast<std::int32_t*>(bytes);
       for(unsigned i=0;i<frames;++i)for(unsigned c=0;c<2;++c)pcm[i*2+c]=SourceSample(bits,submitted+i,c);
       check(client->ReleaseBuffer(frames,0)==S_OK,"commit");submitted+=frames;};
    submit(capacity);
    BYTE* excess{};check(client->GetBuffer(1,&excess)==AUDCLNT_E_BUFFER_TOO_LARGE,"full ring accepted excess");
    check(client->Start()==S_OK,"start");
    // Repeated wraparound with fragmented producer writes and both callback APIs.
    for(unsigned tick=0;tick<24;++tick){
       unsigned index=tick%2;
       if(tick%2){ASIOTime time{};check(callback.bufferSwitchTimeInfo(&time,index,ASIOFalse)==&time,"time callback");}
       else callback.bufferSwitch(index,ASIOFalse);
       for(unsigned c=0;c<2;++c)for(unsigned f=0;f<period;++f){VerifySample(driverBuffers[c*2+index].data()+16+f*layout.bytes,layout,SourceSample(bits,received+f,c));++verifiedSamples;}
       received+=period;GuardBuffers();
       UINT32 padding{};check(client->GetCurrentPadding(&padding)==S_OK&&padding==capacity-period,"callback consumption");
       UINT64 position{};check(clock->GetPosition(&position,nullptr)==S_OK&&position==received-period,"driver clock progression");
       check(WaitForSingleObject(event,0)==WAIT_OBJECT_0,"callback event");
       auto split=1+(tick*17)%(period-1);submit(split);submit(period-split);
    }
    for(unsigned i=0;i<4;++i)callback.bufferSwitch(i%2,ASIOFalse);
    check(client->consumed==submitted&&client->underruns==0,"queued frames lost");
    callback.bufferSwitch(0,ASIOFalse);
    for(unsigned c=0;c<2;++c)for(unsigned f=0;f<period;++f)VerifySample(driverBuffers[c*2].data()+16+f*layout.bytes,layout,0);
    check(client->underruns==1,"underrun metrics");GuardBuffers();
    auto partial=period/3;auto partialStart=submitted;submit(partial);callback.bufferSwitch(1,ASIOFalse);
    for(unsigned c=0;c<2;++c)for(unsigned f=0;f<period;++f)
       VerifySample(driverBuffers[c*2+1].data()+16+f*layout.bytes,layout,f<partial?SourceSample(bits,partialStart+f,c):0);
    check(client->underruns==2&&client->consumed==submitted,"partial underrun consumption");
    BYTE* cancelled{};check(client->GetBuffer(1,&cancelled)==S_OK,"cancel acquire");
    check(client->ReleaseBuffer(2,0)==AUDCLNT_E_INVALID_SIZE,"oversized commit accepted");
    check(client->ReleaseBuffer(0,0)==S_OK,"cancel commit");
    check(client->Reset()==AUDCLNT_E_NOT_STOPPED,"reset while running");
    check(client->Stop()==S_OK&&client->Reset()==S_OK,"stop/reset");
    UINT32 padding{};check(client->GetCurrentPadding(&padding)==S_OK&&padding==0,"reset queue");
    UINT64 position{};check(clock->GetPosition(&position,nullptr)==S_OK&&position==0,"reset clock");
    // Explicit silent submissions must ignore stale packet bytes.
    BYTE* silent{};check(client->GetBuffer(period,&silent)==S_OK,"silent acquire");memset(silent,0x7f,period*8);
    check(client->ReleaseBuffer(period,AUDCLNT_BUFFERFLAGS_SILENT)==S_OK&&client->Start()==S_OK,"restart");
    callback.bufferSwitch(1,ASIOFalse);
    for(unsigned c=0;c<2;++c)for(unsigned f=0;f<period;++f)VerifySample(driverBuffers[c*2+1].data()+16+f*layout.bytes,layout,0);
    if(cases%3==0)callback.asioMessage(kAsioResetRequest,0,nullptr,nullptr);
    else if(cases%3==1)callback.asioMessage(kAsioResyncRequest,0,nullptr,nullptr);
    else callback.sampleRateDidChange(rate==44100?48000:44100);
    check(client->GetCurrentPadding(&padding)==AUDCLNT_E_DEVICE_INVALIDATED,"fault padding");
    check(clock->GetPosition(&position,nullptr)==AUDCLNT_E_DEVICE_INVALIDATED,"fault clock");
    check(client->GetBuffer(1,&silent)==AUDCLNT_E_DEVICE_INVALIDATED,"fault acquire");
    callback.bufferSwitch(0,ASIOFalse);
    for(unsigned c=0;c<2;++c)for(unsigned f=0;f<period;++f)VerifySample(driverBuffers[c*2].data()+16+f*layout.bytes,layout,0);
    clock->Release();client->Release();CloseHandle(event);check(liveDrivers==0,"driver leaked");++cases;
 }
 check(cases==1296&&stops==cases*2&&disposes==cases,"stop/dispose lifecycle");
 check(clockSourceWrites==0&&positionQueries>0,"driver clock source overwritten or sample clock unread");
 // Fail closed on unsupported rates, falsely acknowledged rates and allocation failure.
 for(unsigned failure=0;failure<5;++failure){
    rejectRate=failure==0;wrongRate=failure==1;rejectBuffers=failure==2;
    nanRate=failure==4;
    auto* client=NewClient();unsigned rate=failure==3?12345:192000;
    WAVEFORMATEX wave{WAVE_FORMAT_PCM,2,rate,rate*8,8,32,0};
    check(FAILED(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,0,0,0,&wave,nullptr)),"driver failure ignored");
    client->Release();check(liveDrivers==0&&nce::current.load()==nullptr,"failed driver not released");
 }
 rejectRate=wrongRate=rejectBuffers=nanRate=false;
 WAVEFORMATEX supported{WAVE_FORMAT_PCM,2,96000,96000*8,8,32,0};
 // Capability checks must fail before Initialize when the selected driver rejects them.
 auto* query=NewClient();auto bad=supported;bad.nSamplesPerSec=12345;bad.nAvgBytesPerSec=12345*8;
 check(query->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&bad,nullptr)==AUDCLNT_E_UNSUPPORTED_FORMAT,"prequery lied about rate");
 bad=supported;bad.wFormatTag=WAVE_FORMAT_ALAW;
 check(query->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&bad,nullptr)==AUDCLNT_E_UNSUPPORTED_FORMAT,"compressed input accepted");
 auto savedLayout=outputLayout;outputLayout.type=32;
 check(query->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&supported,nullptr)==AUDCLNT_E_UNSUPPORTED_FORMAT,"DSD driver accepted");
 outputLayout=savedLayout;query->Release();
 // Power-of-two and stepped driver policies, plus optional outputReady.
 unsigned behaviorCases=0;
 for(unsigned scenario=0;scenario<8;++scenario){
    customMinimum=96;customMaximum=1024;customPreferred=300;customStep=scenario%2?-1:96;
    const auto expectedPeriod=scenario%2?512u:384u;bufferPeriod=expectedPeriod;
    outputReadySupported=scenario!=7;autoClock=false;clockError=ASE_OK;
    auto* client=NewClient();check(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,0,0,0,&supported,nullptr)==S_OK,"adaptive init");
    UINT32 capacity{};client->GetBufferSize(&capacity);check(capacity==expectedPeriod*4,"buffer granularity ignored");
    const auto caps=client->CapabilitiesJson();check(caps.find(L"96000")!=std::wstring::npos&&caps.find(L"\"index\":7,\"current\":true")!=std::wstring::npos,"capabilities absent");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);client->SetEventHandle(event);check(client->Start()==S_OK,"clock test start");
    mockPosition=(std::uint64_t{1}<<32)+100;callback.bufferSwitch(0,ASIOFalse);
    UINT64 pos{};check(client->GetPosition(&pos,nullptr)==S_OK&&pos==0,"64-bit clock origin");
    UINT64 stamp{},again{};client->GetPosition(&pos,&stamp);client->GetPosition(&pos,&again);check(stamp>0&&stamp==again,"clock timestamp not latched with position");
    if(scenario==0){
       mockPosition+=expectedPeriod*3;callback.bufferSwitch(1,ASIOFalse);
       check(client->GetPosition(&pos,nullptr)==S_OK&&pos==expectedPeriod*3,"clock replaced by callback count");
    }else if(scenario==1){
       auto before=positionQueries;ASIOTime info{};info.timeInfo.flags=kSamplePositionValid|kSystemTimeValid|kSampleRateValid;
       info.timeInfo.samplePosition.hi=1;info.timeInfo.samplePosition.lo=100+expectedPeriod;info.timeInfo.sampleRate=96000;
       callback.bufferSwitchTimeInfo(&info,1,ASIOFalse);
       check(positionQueries==before&&client->GetPosition(&pos,nullptr)==S_OK&&pos==expectedPeriod,"ASIOTimeInfo unused");
    }else if(scenario==2){--mockPosition;callback.bufferSwitch(1,ASIOFalse);check(client->GetPosition(&pos,nullptr)==AUDCLNT_E_DEVICE_INVALIDATED,"backward clock accepted");
    }else if(scenario==3){
       for(unsigned i=0;i<3;++i)callback.bufferSwitch(i%2,ASIOFalse);
       check(client->GetPosition(&pos,nullptr)==S_OK,"brief clock quantization treated as stalled");
       for(unsigned i=0;i<100;++i)callback.bufferSwitch(i%2,ASIOFalse);
       check(client->GetPosition(&pos,nullptr)==AUDCLNT_E_DEVICE_INVALIDATED,"stalled clock accepted");
    }else if(scenario==4||scenario==5){clockError=scenario==4?ASE_NoClock:ASE_NotPresent;callback.bufferSwitch(1,ASIOFalse);check(client->GetPosition(&pos,nullptr)==AUDCLNT_E_DEVICE_INVALIDATED,"missing clock silently estimated");
    }else if(scenario==6){ASIOTime info{};info.timeInfo.flags=kSampleRateValid;info.timeInfo.sampleRate=48000;callback.bufferSwitchTimeInfo(&info,1,ASIOFalse);check(client->GetPosition(&pos,nullptr)==AUDCLNT_E_DEVICE_INVALIDATED,"time info rate mismatch accepted");
    }else{auto before=readyCalls;mockPosition+=expectedPeriod;callback.bufferSwitch(1,ASIOFalse);check(readyCalls==before,"unsupported outputReady called");}
    client->Release();CloseHandle(event);check(liveDrivers==0,"behavior test driver leaked");++behaviorCases;
 }
 check(clockSourceWrites==0,"external clock source changed");
 std::cout<<"PASS ASIO matrix: "<<cases<<" cases = 44.1/48/88.2/96/176.4/192 kHz x 16/24/32-bit source lattices x 18 driver layouts x 64/128/512/1024-frame periods; "<<verifiedSamples<<" independently decoded samples\n";
 std::cout<<"PASS ring wrap, fragmented writes, double buffers, time callbacks, silence, empty/partial underrun, cancel/oversized commit, clocks, restart, reset/resync/rate faults, 5 failed-open cases and disposal\n";
 std::cout<<"PASS "<<behaviorCases<<" adaptive driver scenarios: actual 64-bit clock/TimeInfo, discontinuity/stall/no-clock, rate mismatch, buffer constraints, capability query and optional outputReady; external clock preserved\n";
 std::cout<<"SCOPE: mock IASIO driver against production host. Source lattices are supplied as canonical int32 after checked normalization request; actual NetEase decoder and vendor hardware are not exercised.\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
