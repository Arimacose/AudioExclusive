#include "audio_v2/WasapiExclusiveSink.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace ammod::audio_v2;
static void Check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
struct LiveSource final:IIntegerPcmSource {
    unsigned cursor{200},end{210};
    bool CanProvide(UINT32 n)const noexcept override{return end-cursor>=n||Exhausted();}
    bool Exhausted()const noexcept override{return cursor==end;}
    HRESULT Fill(std::int32_t* out,UINT32 cap,UINT32& got,bool& eos)noexcept override{
        got=std::min(cap,end-cursor);for(unsigned i=0;i<got;++i){out[i*2]=cursor;out[i*2+1]=-static_cast<int>(cursor);++cursor;}eos=Exhausted();return S_OK;
    }
};
int main(){try{
    PcmPacketLedger ledger;ledger.Initialize(4,2,3);
    const auto packet=[](int first){std::array<std::int32_t,8> result{};for(int i=0;i<4;++i){result[i*2]=first+i;result[i*2+1]=-first-i;}return result;};
    auto a=packet(100),b=packet(104),c=packet(108),d=packet(112);
    Check(ledger.CanRecord(0),"empty history not writable");ledger.Record(a.data(),4,4);ledger.Record(b.data(),4,8);ledger.Record(c.data(),2,12);
    Check(!ledger.CanRecord(0),"unplayed packet was overwritten by a wrapped history slot");
    const auto pending=ledger.Unplayed(6);
    Check(pending==std::vector<std::int32_t>({106,-106,107,-107,108,-108,109,-109}),"pause failed to preserve the exact partial-packet tail or included EOF padding");
    Check(ledger.CanRecord(4),"played history could not be reused");ledger.Record(d.data(),4,24);
    const auto gap=ledger.Unplayed(21);Check(gap==std::vector<std::int32_t>({113,-113,114,-114,115,-115}),"device-clock gaps were converted into source PCM");
    LiveSource live;ReplayPcmSource source;source.Attach(live,2);source.Prepend(pending);
    std::array<std::int32_t,12> output{};UINT32 written{};bool eos{};
    Check(source.CanProvide(6)&&source.Fill(output.data(),6,written,eos)==S_OK&&written==6&&!eos,"replay did not join the live source in one device packet");
    std::vector<std::int32_t> expected=pending;expected.insert(expected.end(),{200,-200,201,-201});
    Check(std::equal(expected.begin(),expected.end(),output.begin()),"resume duplicated, omitted or reordered source frames");
    auto last=packet(90);source.Prepend(std::vector<std::int32_t>(last.begin(),last.begin()+4));
    Check(source.Fill(output.data(),2,written,eos)==S_OK&&output[0]==90&&output[2]==91,"second pause did not prepend its already submitted tail");
    source.Clear();Check(!source.CanProvide(1)&&!source.Exhausted(),"seek/disable retained stale replay source ownership");
    std::cout<<"PASS exact partial-packet pause replay, wrap protection, device-clock gaps, EOF padding, second pause and clear\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
