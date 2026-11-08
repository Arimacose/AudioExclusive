#include <windows.h>
#include "DevicePolicy.h"
#include "WasapiPolicy.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cmath>
static void check(bool v,const char* reason){if(!v)throw std::runtime_error(reason);}
static unsigned live{},created{},initialized{},released{};
static UINT32 alignedFrames{};
class Probe final:public IAudioClient {
 ULONG refs=1;
public:
 bool called{};Probe(){++live;++created;}~Probe(){--live;++released;}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**)override{return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,LPCGUID)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* n)override{*n=alignedFrames;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE,const WAVEFORMATEX*,WAVEFORMATEX**)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*,REFERENCE_TIME*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Start()override{throw std::runtime_error("preflight started playback");}
 HRESULT STDMETHODCALLTYPE Stop()override{return E_NOTIMPL;}HRESULT STDMETHODCALLTYPE Reset()override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetService(REFIID,void**)override{return E_NOTIMPL;}
};
int main(){try{
 check(nce::SelectAsioPeriod(64,64,128,0)==64,"fixed ASIO size");
 check(nce::SelectAsioPeriod(64,1024,300,-1)==512,"power-of-two ASIO size");
 check(nce::SelectAsioPeriod(96,1024,300,96)==384,"stepped ASIO size");
 check(nce::SelectAsioPeriod(96,1024,1000,96)==960,"step maximum");
 check(nce::SelectAsioPeriod(96,1024,300,0)==300,"continuous ASIO size");
 check(nce::SelectAsioPeriod(96,100,98,-1)==0,"impossible ASIO range");
 check(nce::SelectAsioPeriod(0,1024,300,0)==0&&nce::SelectAsioPeriod(64,1024,300,-2)==0,"invalid ASIO limits");
 check(nce::SelectAsioPeriod(64,1024,300,LONG_MAX)==64,"ASIO step overflow");
 unsigned formatChecks=0,alignmentChecks=0;
 for(unsigned rate:{44100u,48000u,88200u,96000u,176400u,192000u}){
  auto candidates=nce::WasapiPcmCandidates(rate,2);
  for(auto& f:candidates){
   check(f.Format.nSamplesPerSec==rate&&f.Format.nBlockAlign==f.Format.wBitsPerSample/4,"WASAPI format changed rate/alignment");
   check(f.Format.nAvgBytesPerSec==rate*f.Format.nBlockAlign,"WASAPI byte rate");
   for(std::int32_t sample:{INT32_MIN,INT32_MIN+1,-16777217,-65537,-257,-1,0,1,255,256,65537,16777217,INT32_MAX-1,INT32_MAX}){
    unsigned char bytes[8];memset(bytes,0xcc,sizeof(bytes));
    const auto container=f.Format.wBitsPerSample,valid=f.Samples.wValidBitsPerSample;
    nce::StoreWasapiInteger(bytes+1,container,valid,sample);
    check(bytes[0]==0xcc&&bytes[1+container/8]==0xcc,"WASAPI conversion overflow");
    std::uint64_t raw=0;for(unsigned i=0;i<container/8u;++i)raw+=std::uint64_t{bytes[1+i]}<<(8*i);
    auto actual=static_cast<std::int64_t>(raw);if(raw&(std::uint64_t{1}<<(container-1)))actual-=std::int64_t{1}<<container;
    auto expected=static_cast<std::int64_t>(std::floor(std::ldexp(static_cast<double>(sample),static_cast<int>(valid)-32)))*(std::int64_t{1}<<(container-valid));
    check(actual==expected,"WASAPI integer format/valid bits mismatch");++formatChecks;
   }
  }
  // A non-128-aligned driver requirement, learned from failed Initialize.
  alignedFrames=rate/100+7;const auto required=nce::FramesPeriod(alignedFrames,rate);
  auto activate=[](IAudioClient** c){*c=new Probe;return S_OK;};
  auto init=[&](IAudioClient* c,DWORD,REFERENCE_TIME period,const WAVEFORMATEX*){
   auto* p=static_cast<Probe*>(c);check(!p->called,"retry reused initialized client");p->called=true;++initialized;
   return period==required?S_OK:AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;};
  REFERENCE_TIME selected{};auto before=created;
  check(nce::ProbeExclusivePeriod(activate,init,&candidates[0].Format,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,200000,selected)==S_OK,"WASAPI alignment retry");
  check(selected==required&&created==before+2&&live==0,"driver alignment result/client cleanup");++alignmentChecks;
  auto fault=[](IAudioClient*,DWORD,REFERENCE_TIME,const WAVEFORMATEX*){return AUDCLNT_E_DEVICE_INVALIDATED;};
  before=created;check(nce::ProbeExclusivePeriod(activate,fault,&candidates[0].Format,0,200000,selected)==AUDCLNT_E_DEVICE_INVALIDATED&&created==before+1&&live==0,"WASAPI device failure swallowed");
  alignedFrames=0;auto invalid=[](IAudioClient*,DWORD,REFERENCE_TIME,const WAVEFORMATEX*){return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;};
  check(FAILED(nce::ProbeExclusivePeriod(activate,invalid,&candidates[0].Format,0,200000,selected))&&live==0,"zero alignment accepted");
 }
 check(created==released,"preflight client leak");
 std::cout<<"PASS device policy: ASIO fixed/stepped/power-of-two/continuous/invalid limits; "<<formatChecks<<" WASAPI boundary sample checks across 6 rates and 7 formats; "<<alignmentChecks<<" non-128 alignment retries with fresh clients, device errors and cleanup; no playback started\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
