// Exercise the production bridge with a CEF client whose initial native page
// hands off to app.html without a second load callback. No app is launched.
#include "CefBridge.cpp"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::wstring page=L"orpheus://native/start.html";
unsigned executions{},loadEnds{},closes{};
bool valid=true;
cef_frame_t frame{};cef_browser_t browser{};cef_browser_host_t host{};cef_client_t client{};
cef_load_handler_t load{};cef_display_handler_t display{};cef_life_span_handler_t life{};
std::unordered_map<cef_base_ref_counted_t*,int> refs;
void CEF_CALLBACK Add(cef_base_ref_counted_t* p){++refs[p];}
int CEF_CALLBACK Release(cef_base_ref_counted_t* p){auto it=refs.find(p);if(it==refs.end())throw std::runtime_error("unknown reference");if(--it->second)return 0;refs.erase(it);return 1;}
int CEF_CALLBACK Has(cef_base_ref_counted_t*){return 1;}
template<class T>void Base(T& value){value.base={sizeof(T),Add,Release,Has,Has};refs[&value.base]=1;}
void CEF_CALLBACK FreeString(cef_string_userfree_t value){delete[] value->str;delete value;}
cef_string_userfree_t CEF_CALLBACK Url(cef_frame_t*){
    auto* data=new wchar_t[page.size()+1];memcpy(data,page.c_str(),(page.size()+1)*sizeof(wchar_t));
    return new cef_string_t{data,page.size(),nullptr};
}
void CEF_CALLBACK OriginalEnd(cef_load_handler_t*,cef_browser_t*,cef_frame_t*,int){++loadEnds;}
void CEF_CALLBACK OriginalClose(cef_life_span_handler_t*,cef_browser_t*){++closes;}
void Setup(){
    Base(frame);Base(browser);Base(host);Base(client);Base(load);Base(display);Base(life);
    frame.is_valid=[](cef_frame_t*){return valid?1:0;};frame.get_url=Url;
    frame.get_identifier=[](cef_frame_t*)->int64{return 17;};
    frame.execute_java_script=[](cef_frame_t*,const cef_string_t*,const cef_string_t*,int){++executions;};
    browser.get_identifier=[](cef_browser_t*){return 9;};
    browser.get_main_frame=[](cef_browser_t*){return &frame;};
    browser.get_frame_byident=[](cef_browser_t*,int64){return &frame;};
    browser.get_host=[](cef_browser_t*){return &host;};host.get_client=[](cef_browser_host_t*){return &client;};
    client.get_load_handler=[](cef_client_t*){return &load;};
    client.get_display_handler=[](cef_client_t*){return &display;};
    client.get_life_span_handler=[](cef_client_t*){return &life;};
    load.on_load_end=OriginalEnd;life.on_before_close=OriginalClose;
    nce::freeString=FreeString;nce::uiSource=L"const token=\"__NCEXCLUSIVE_TOKEN__\";";
}
}
namespace nce {
Backend& Backend::Instance(){static Backend value;return value;}
bool Backend::SetEnabled(bool){return true;}
void Backend::UiReady(bool){}
std::wstring Backend::StatusJson(){return L"{}";}
bool SetAsioSelection(const std::wstring&){return true;}
std::wstring ActualAudioStatusJson(){return L"[]";}
std::wstring WasapiStatusJson(){return L"{}";}
void Log(std::wstring_view,HRESULT)noexcept{}
std::wstring ModulePath(HMODULE){return {};}
std::wstring FileHash(const std::wstring&){return {};}
std::wstring Utf16(std::string_view text){return {text.begin(),text.end()};}
std::wstring JsonString(std::wstring_view text){return L"\""+std::wstring(text)+L"\"";}
}
extern "C" {
MH_STATUS WINAPI MH_CreateHook(LPVOID,LPVOID,LPVOID*){return MH_ERROR_UNSUPPORTED_FUNCTION;}
MH_STATUS WINAPI MH_RemoveHook(LPVOID){return MH_OK;}
MH_STATUS WINAPI MH_DisableHook(LPVOID){return MH_OK;}
MH_STATUS WINAPI MH_QueueEnableHook(LPVOID){return MH_OK;}
MH_STATUS WINAPI MH_ApplyQueued(){return MH_OK;}
}

void Drop(cef_base_ref_counted_t& b){Check(b.release(&b)==1,"final CEF release did not destroy wrapper");}
int main(){try{
 for(unsigned cycle=1;cycle<=1000;++cycle){
   Setup();
   client.get_load_handler=[](cef_client_t*){Add(&load.base);return &load;};
   client.get_display_handler=[](cef_client_t*){Add(&display.base);return &display;};
   client.get_life_span_handler=[](cef_client_t*){Add(&life.base);return &life;};
   nce::Observe(&client);auto* l=nce::GetLoad(&client);auto* d=nce::GetDisplay(&client);auto* f=nce::GetLife(&client);
   l->base.release(&l->base);d->base.release(&d->base);f->base.release(&f->base);
   Check(refs.at(&client.base)==1&&refs.at(&load.base)==1&&refs.at(&life.base)==1,"bridge pinned a CEF object");
   nce::OnClose(&life,&browser);
   Drop(client.base);
   Check(nce::clients.empty()&&nce::loads.size()==1,"shared handler retired before host release");
   Drop(load.base);Drop(display.base);Drop(life.base);Drop(frame.base);Drop(browser.base);Drop(host.base);
   Check(nce::clients.empty()&&nce::loads.empty()&&nce::loadingStates.empty()&&nce::displays.empty()&&nce::lives.empty()&&nce::creations.empty()&&nce::lifetimes.empty()&&refs.empty(),"lifetime metadata retained");
 }
 // Repeated injection of new frame identifiers under one live browser prunes
 // old identifiers as soon as get_frame_byident reports them detached.
 Setup();page=L"orpheus://orpheus/pub/app.html";
 static int64 currentId=100;
 frame.get_identifier=[](cef_frame_t*)->int64{return currentId;};
 browser.get_frame_byident=[](cef_browser_t*,int64 id){if(id!=currentId)return static_cast<cef_frame_t*>(nullptr);Add(&frame.base);return &frame;};
 for(int i=0;i<1000;++i){++currentId;nce::Inject(&browser,&frame);Check(nce::bindings.size()==1,"detached frame binding retained");}
 std::cout<<"PASS: 1000 CEF final-release/address-reuse cycles; no pinned refs or callback records; shared handler survives client release; 1000 frame replacements stay at one binding\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
