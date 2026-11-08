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
void CEF_CALLBACK Add(cef_base_ref_counted_t*){}
int CEF_CALLBACK Release(cef_base_ref_counted_t*){return 0;}
int CEF_CALLBACK Has(cef_base_ref_counted_t*){return 1;}
template<class T>void Base(T& value){value.base={sizeof(T),Add,Release,Has,Has};}
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
int main(){try{
    Setup();nce::Observe(&client);auto* handler=nce::GetLoad(&client);nce::GetLife(&client);
    nce::OnLoadEnd(handler,&browser,&frame,200);
    Check(loadEnds==1&&executions==0,"native bootstrap page must not receive settings script");
    nce::WatchBrowser(&browser);auto watch=nce::watches.at(9);
    Check(watch->timer!=0,"bootstrap timer was not created");
    nce::BootstrapTick(nullptr,WM_TIMER,watch->timer,0);
    Check(executions==0,"native bootstrap page must remain excluded");
    page=L"orpheus://orpheus/pub/app.html";
    nce::BootstrapTick(nullptr,WM_TIMER,watch->timer,0);
    Check(executions==1,"handoff without a load callback did not inject settings");
    const auto token=nce::bindings.at(17).token;
    std::wstring request=L"__NCExclusiveRequest__/"+token+L"/1/boot/1";
    cef_string_t message{request.data(),request.size(),nullptr};
    Check(nce::Protocol(&browser,&message)&&nce::bindings.at(17).acknowledged,"boot acknowledgement not bound to the frame");
    const auto afterReply=executions;nce::BootstrapTick(nullptr,WM_TIMER,watch->timer,0);
    Check(executions==afterReply,"acknowledged bridge was injected again");
    nce::OnClose(&life,&browser);nce::BootstrapTick(nullptr,WM_TIMER,watch->timer,0);
    Check(closes==1&&watch->closed&&nce::watches.empty()&&nce::bindings.empty()&&executions==afterReply,"closed browser was accessed or leaked binding");
    page=L"https://example.com/";nce::Inject(&browser,&frame);
    Check(executions==afterReply,"external page received settings script");
    page=L"orpheus://orpheus/pub/app.html";valid=false;nce::Inject(&browser,&frame);
    Check(executions==afterReply,"invalid frame received settings script");
    std::cout<<"CEF bootstrap: native handoff, acknowledgement, duplicate suppression, close cleanup and origin/validity guards passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
