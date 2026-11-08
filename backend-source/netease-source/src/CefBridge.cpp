#include "Backend.h"
#include "AsioHost.h"
#include "WasapiHost.h"
#include <MinHook.h>
#include <bcrypt.h>
#include <algorithm>
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_browser_capi.h"
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>
#include <stdexcept>

namespace nce { namespace {
using FreeStringFn=void(*)(cef_string_userfree_t);
FreeStringFn freeString{};
std::wstring uiSource;
std::mutex tokenMutex;
struct Binding {std::wstring token;int browser{};bool acknowledged{};};
std::unordered_map<int64,Binding> bindings;
std::wstring TakeString(cef_string_userfree_t text) {
    if(!text)return {};std::wstring result(text->str,text->length);freeString(text);return result;
}
bool Trusted(cef_frame_t* frame) {
    if(!frame || frame->base.size!=sizeof(cef_frame_t) || !frame->is_valid(frame))return false;
    std::wstring url=TakeString(frame->get_url(frame));
    return url.starts_with(L"orpheus://orpheus/");
}
void Execute(cef_frame_t* frame,std::wstring_view script) {
    cef_string_t code{const_cast<wchar_t*>(script.data()),script.size(),nullptr};
    constexpr wchar_t source[]=L"ncexclusive://settings";
    cef_string_t url{const_cast<wchar_t*>(source),std::size(source)-1,nullptr};
    frame->execute_java_script(frame,&code,&url,1);
}
std::wstring NewToken() {
    unsigned char bytes[16]{};
    if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
        throw std::runtime_error("bridge token");
    constexpr wchar_t digits[]=L"0123456789abcdef";std::wstring value;
    for(auto b:bytes){value+=digits[b>>4];value+=digits[b&15];}return value;
}
void PruneBindings(cef_browser_t* browser) {
    const int browserId=browser->get_identifier(browser);
    std::vector<int64> candidates;
    {std::lock_guard guard(tokenMutex);
        for(const auto& [id,binding]:bindings)if(binding.browser==browserId)candidates.push_back(id);}
    for(const auto id:candidates) {
        auto* frame=browser->get_frame_byident(browser,id);
        const bool valid=frame&&frame->is_valid(frame);
        if(frame)frame->base.release(&frame->base);
        if(!valid){std::lock_guard guard(tokenMutex);bindings.erase(id);}
    }
}
void TrackBrowser(cef_browser_t* browser);
void ClearBrowserBindings(int browserId) {
    bool removed{},empty{};
    {std::lock_guard guard(tokenMutex);
        removed=std::erase_if(bindings,[&](const auto& entry){return entry.second.browser==browserId;})!=0;
        empty=bindings.empty();}
    if(removed&&empty)Backend::Instance().UiReady(false);
}
void Inject(cef_browser_t* browser,cef_frame_t* frame) {
    if(!browser || !Trusted(frame))return;
    TrackBrowser(browser);
    PruneBindings(browser);
    std::wstring token=NewToken();
    {std::lock_guard guard(tokenMutex);
        const auto id=frame->get_identifier(frame);const auto owner=browser->get_identifier(browser);
        const auto count=std::count_if(bindings.begin(),bindings.end(),[&](const auto& item){return item.second.browser==owner;});
        if(!bindings.contains(id)&&count>=128){Log(L"settings_frame_binding_limit");return;}
        bindings[id]={token,owner};}
    std::wstring source=uiSource;
    constexpr wchar_t marker[]=L"\"__NCEXCLUSIVE_TOKEN__\"";
    auto position=source.find(marker);if(position==std::wstring::npos)return;
    source.replace(position,std::size(marker)-1,JsonString(token));
    Execute(frame,source);Log(L"settings_script_injection_requested_"+TakeString(frame->get_url(frame)));
}
bool Protocol(cef_browser_t* browser,const cef_string_t* message) {
    if(!message || !browser || browser->base.size!=sizeof(cef_browser_t))return false;
    std::wstring url(message->str,message->length);
    constexpr wchar_t prefix[]=L"__NCExclusiveRequest__/";
    if(!url.starts_with(prefix))return false;
    // Consume our console messages before default logging. There is no
    // hidden navigation, resource load, local server, or network request.
    std::vector<std::wstring> parts;std::size_t begin=std::size(prefix)-1;
    while(begin<=url.size()&&parts.size()<6){auto end=url.find(L'/',begin);
        parts.push_back(url.substr(begin,end==std::wstring::npos?end:end-begin));
        if(end==std::wstring::npos)break;begin=end+1;}
    if(parts.size()!=4 || parts[0].size()!=32 || parts[1].empty() || parts[1].size()>12)return true;
    if(parts[1].find_first_not_of(L"0123456789")!=std::wstring::npos)return true;
    int64 identifier=-1;
    {std::lock_guard guard(tokenMutex);
        for(const auto& [key,binding]:bindings)if(binding.browser==browser->get_identifier(browser)&&binding.token==parts[0]){identifier=key;break;}}
    if(identifier<0)return true;
    auto* frame=browser->get_frame_byident(browser,identifier);
    if(!frame)return true;
    struct FrameRef {cef_frame_t* value;~FrameRef(){value->base.release(&value->base);}} frameRef{frame};
    if(!Trusted(frame))return true;
    bool ok=true;
    if(parts[2]==L"output")ok=SetAsioSelection(parts[3]);
    else if(parts[2]==L"set" && (parts[3]==L"0" || parts[3]==L"1"))ok=Backend::Instance().SetEnabled(parts[3]==L"1");
    else if(parts[2]==L"mounted" && parts[3]==L"1"){Backend::Instance().UiReady(true);Log(L"settings_row_mounted");}
    else if(parts[2]==L"boot" && parts[3]==L"1"){
        {std::lock_guard guard(tokenMutex);auto found=bindings.find(identifier);if(found!=bindings.end())found->second.acknowledged=true;}
        Log(L"settings_script_running");
    }
    else if(parts[2]!=L"status" || parts[3]!=L"0")ok=false;
    auto state=Backend::Instance().StatusJson();
    state.pop_back();state+=L",\"wasapiTiming\":"+ActualAudioStatusJson()+L",\"wasapiOutput\":"+WasapiStatusJson()+L"}";
    if(parts[2]==L"status") {
        static ULONGLONG previous{};auto now=GetTickCount64();
        if(now-previous>=5000){previous=now;Log(L"status_snapshot_"+state);}
    }
    Execute(frame,L"window.__NCExclusiveReply&&window.__NCExclusiveReply("+parts[1]+L",{\"ok\":"+
                   (ok?L"true":L"false")+L",\"state\":"+state+L"});");
    return true;
}

// Keep the original CEF wrapper objects and their identity. The host may
// recover its C++ client from browser->get_host()->get_client(). A proxy C
// object would not carry the host's private CppToC wrapper metadata.
std::mutex callbackMutex;
struct ClientCallbacks {
    decltype(cef_client_t::get_load_handler) load{};
    decltype(cef_client_t::get_display_handler) display{};
    decltype(cef_client_t::get_life_span_handler) life{};
};
std::unordered_map<cef_client_t*,ClientCallbacks> clients;
std::unordered_map<cef_load_handler_t*,decltype(cef_load_handler_t::on_load_end)> loads;
std::unordered_map<cef_load_handler_t*,decltype(cef_load_handler_t::on_loading_state_change)> loadingStates;
std::unordered_map<cef_display_handler_t*,decltype(cef_display_handler_t::on_console_message)> displays;
std::unordered_map<cef_life_span_handler_t*,decltype(cef_life_span_handler_t::on_before_close)> lives;
std::unordered_map<cef_life_span_handler_t*,decltype(cef_life_span_handler_t::on_after_created)> creations;
using CefReleaseFn = decltype(cef_base_ref_counted_t::release);
struct TrackedLifetime { CefReleaseFn release{}; int browserId{}; };
std::unordered_map<cef_base_ref_counted_t*,std::shared_ptr<TrackedLifetime>> lifetimes;
void EraseCallbacksLocked(cef_base_ref_counted_t* base) {
    clients.erase(reinterpret_cast<cef_client_t*>(base));
    loads.erase(reinterpret_cast<cef_load_handler_t*>(base));
    loadingStates.erase(reinterpret_cast<cef_load_handler_t*>(base));
    displays.erase(reinterpret_cast<cef_display_handler_t*>(base));
    lives.erase(reinterpret_cast<cef_life_span_handler_t*>(base));
    creations.erase(reinterpret_cast<cef_life_span_handler_t*>(base));
}
int CEF_CALLBACK ReleaseTracked(cef_base_ref_counted_t* base) {
    std::shared_ptr<TrackedLifetime> lifetime;
    {
        std::lock_guard guard(callbackMutex);
        const auto it=lifetimes.find(base);
        if(it==lifetimes.end())return 0;
        lifetime=it->second;
    }
    // CEF returns 1 after destroying the actual wrapper. Do not dereference
    // base after this call or hold our mutex across a host destructor.
    const int destroyed=lifetime->release(base);
    if(destroyed) {
        {
            std::lock_guard guard(callbackMutex);
            const auto it=lifetimes.find(base);
            // An allocation on another thread may already have reused the address.
            if(it!=lifetimes.end()&&it->second==lifetime) {
                EraseCallbacksLocked(base);
                lifetimes.erase(it);
            }
        }
        // The browser callback is optional. Retire its tokens on final CEF
        // release too, without taking an extra browser reference or touching
        // the destroyed wrapper. Never nest tokenMutex under callbackMutex.
        if(lifetime->browserId)ClearBrowserBindings(lifetime->browserId);
    }
    return destroyed;
}
void TrackLifetimeLocked(cef_base_ref_counted_t* base) {
    if(base->release==&ReleaseTracked)return;
    // A fresh wrapper at a reused address owns a new release callback record.
    EraseCallbacksLocked(base);
    lifetimes[base]=std::make_shared<TrackedLifetime>(TrackedLifetime{base->release});
    base->release=&ReleaseTracked;
}
void TrackBrowser(cef_browser_t* browser) {
    if(!browser||browser->base.size!=sizeof(cef_browser_t))return;
    const int id=browser->get_identifier(browser);
    std::lock_guard guard(callbackMutex);
    TrackLifetimeLocked(&browser->base);
    lifetimes.at(&browser->base)->browserId=id;
}
struct BrowserWatch {cef_browser_t* browser{};bool closed{};unsigned attempts{};UINT_PTR timer{};
    explicit BrowserWatch(cef_browser_t* value):browser(value){browser->base.add_ref(&browser->base);}
    // On normal close, release on the UI thread. At process teardown CEF may
    // already be unloaded; OS cleanup owns any remaining process-lifetime ref.
    ~BrowserWatch(){if(closed)browser->base.release(&browser->base);}
};
std::unordered_map<int,std::shared_ptr<BrowserWatch>> watches; // UI thread only
void FinishWatch(const std::shared_ptr<BrowserWatch>& watch) {
    if (!watch) return;
    if (watch->timer) KillTimer(nullptr, watch->timer);
    watch->timer = 0;
    watch->closed = true;
    const int id = watch->browser->get_identifier(watch->browser);
    auto found = watches.find(id);
    if (found != watches.end() && found->second == watch) watches.erase(found);
}
void WatchBrowser(cef_browser_t* browser);
// The host owns CEF references. Final Release removes all callback metadata.
template<class T,class Map,class Callback>
void Patch(T* source,Map& originals,Callback T::* member,Callback replacement) {
    if(!source)return;
    std::lock_guard guard(callbackMutex);
    TrackLifetimeLocked(&source->base);
    if(originals.contains(source)){
        if(source->*member!=replacement)Log(L"cef_handler_callback_restored");
        source->*member=replacement;return;
    }
    originals.emplace(source,source->*member);
    source->*member=replacement;
}
void CEF_CALLBACK OnLoadEnd(cef_load_handler_t* self,cef_browser_t* browser,cef_frame_t* frame,int status) {
    Log(L"cef_load_end_"+std::to_wstring(status));
    decltype(cef_load_handler_t::on_load_end) original{};
    {std::lock_guard guard(callbackMutex);original=loads.at(self);}
    if(original)original(self,browser,frame,status);
    try{Inject(browser,frame);}catch(...){Log(L"settings_injection_failed",E_FAIL);}
}
void CEF_CALLBACK OnLoadingState(cef_load_handler_t* self,cef_browser_t* browser,int loading,int back,int forward) {
    Log(L"cef_loading_"+std::to_wstring(loading));
    decltype(cef_load_handler_t::on_loading_state_change) original{};
    {std::lock_guard guard(callbackMutex);original=loadingStates.at(self);}
    if(original)original(self,browser,loading,back,forward);
    if(loading||!browser)return;
    auto* frame=browser->get_main_frame(browser);if(!frame)return;
    bool bound{};{std::lock_guard guard(tokenMutex);bound=bindings.contains(frame->get_identifier(frame));}
    if(!bound)try{Inject(browser,frame);}catch(...){Log(L"settings_fallback_injection_failed",E_FAIL);}
    frame->base.release(&frame->base);
}
int CEF_CALLBACK OnConsole(cef_display_handler_t* self,cef_browser_t* browser,cef_log_severity_t level,
                          const cef_string_t* message,const cef_string_t* source,int line) {
    try{if(Protocol(browser,message))return 1;}catch(...){Log(L"bridge_request_failed",E_FAIL);return 1;}
    decltype(cef_display_handler_t::on_console_message) original{};
    {std::lock_guard guard(callbackMutex);original=displays.at(self);}
    return original?original(self,browser,level,message,source,line):0;
}
void CEF_CALLBACK OnClose(cef_life_span_handler_t* self,cef_browser_t* browser) {
    try {
        int id=browser->get_identifier(browser);
        if(auto found=watches.find(id);found!=watches.end()){found->second->closed=true;KillTimer(nullptr,found->second->timer);watches.erase(found);}
        ClearBrowserBindings(id);
    }catch(...){Log(L"bridge_cleanup_failed",E_FAIL);}
    decltype(cef_life_span_handler_t::on_before_close) original{};
    {std::lock_guard guard(callbackMutex);original=lives.at(self);}
    if(original)original(self,browser);
}
void CEF_CALLBACK OnCreated(cef_life_span_handler_t* self,cef_browser_t* browser) {
    decltype(cef_life_span_handler_t::on_after_created) original{};
    {std::lock_guard guard(callbackMutex);original=creations.at(self);}
    if(original)original(self,browser);
    WatchBrowser(browser);
}
cef_load_handler_t* CEF_CALLBACK GetLoad(cef_client_t* self) {
    decltype(cef_client_t::get_load_handler) original{};
    {std::lock_guard guard(callbackMutex);original=clients.at(self).load;}
    auto* handler=original?original(self):nullptr;
    static bool logged{};if(!logged){logged=true;Log(L"cef_load_handler_size_"+std::to_wstring(handler?handler->base.size:0)+L"_expected_"+std::to_wstring(sizeof(cef_load_handler_t)));}
    if(handler&&handler->base.size==sizeof(cef_load_handler_t)){
        Patch(handler,loads,&cef_load_handler_t::on_load_end,&OnLoadEnd);
        Patch(handler,loadingStates,&cef_load_handler_t::on_loading_state_change,&OnLoadingState);
    }
    return handler;
}
cef_display_handler_t* CEF_CALLBACK GetDisplay(cef_client_t* self) {
    decltype(cef_client_t::get_display_handler) original{};
    {std::lock_guard guard(callbackMutex);original=clients.at(self).display;}
    auto* handler=original?original(self):nullptr;
    static bool logged{};if(!logged){logged=true;Log(L"cef_display_handler_size_"+std::to_wstring(handler?handler->base.size:0)+L"_expected_"+std::to_wstring(sizeof(cef_display_handler_t)));}
    if(handler&&handler->base.size==sizeof(cef_display_handler_t))
        Patch(handler,displays,&cef_display_handler_t::on_console_message,&OnConsole);
    return handler;
}
cef_life_span_handler_t* CEF_CALLBACK GetLife(cef_client_t* self) {
    decltype(cef_client_t::get_life_span_handler) original{};
    {std::lock_guard guard(callbackMutex);original=clients.at(self).life;}
    auto* handler=original?original(self):nullptr;
    static bool logged{};if(!logged){logged=true;Log(L"cef_life_handler_size_"+std::to_wstring(handler?handler->base.size:0)+L"_expected_"+std::to_wstring(sizeof(cef_life_span_handler_t)));}
    if(handler&&handler->base.size==sizeof(cef_life_span_handler_t)){
        Patch(handler,lives,&cef_life_span_handler_t::on_before_close,&OnClose);
        Patch(handler,creations,&cef_life_span_handler_t::on_after_created,&OnCreated);
    }
    return handler;
}
void Observe(cef_client_t* source) {
    if(!source || source->base.size!=sizeof(cef_client_t)){Log(L"client_abi_mismatch",E_NOTIMPL);return;}
    std::lock_guard guard(callbackMutex);
    TrackLifetimeLocked(&source->base);
    if(clients.contains(source))return;
    clients.emplace(source,ClientCallbacks{source->get_load_handler,source->get_display_handler,source->get_life_span_handler});
    source->get_load_handler=&GetLoad;source->get_display_handler=&GetDisplay;source->get_life_span_handler=&GetLife;
    Log(L"cef_original_client_observed");
}
// NetEase can hand off native/start.html to app.html without another observed
// load callback. Keep the browser identity and query its current client on the
// creating/UI thread until JavaScript acknowledges the bridge (at most 30s).
// No polling thread calls into CEF, and a closing browser is never touched.
void CALLBACK BootstrapTick(HWND,UINT,UINT_PTR timer,DWORD){
    std::shared_ptr<BrowserWatch> watch;
    for(const auto& [id,candidate]:watches)if(candidate->timer==timer){watch=candidate;break;}
    if(!watch||watch->closed){KillTimer(nullptr,timer);return;}
    try {
        auto* browser=watch->browser;
        auto* host=browser->get_host(browser);
        auto* client=host?host->get_client(host):nullptr;
        if(client){
            Observe(client);
            auto* load=GetLoad(client);if(load)load->base.release(&load->base);
            auto* display=GetDisplay(client);if(display)display->base.release(&display->base);
            auto* life=GetLife(client);if(life)life->base.release(&life->base);
            client->base.release(&client->base);
        }
        if(host)host->base.release(&host->base);
        auto* frame=browser->get_main_frame(browser);bool acknowledged{};
        if(frame){
            {std::lock_guard guard(tokenMutex);auto found=bindings.find(frame->get_identifier(frame));
                acknowledged=found!=bindings.end()&&found->second.acknowledged;}
            if(!acknowledged&&Trusted(frame))Inject(browser,frame);
            frame->base.release(&frame->base);
        }
        if(acknowledged){FinishWatch(watch);Log(L"settings_bootstrap_complete");return;}
        if(++watch->attempts>=60){FinishWatch(watch);Log(L"settings_bootstrap_timeout",HRESULT_FROM_WIN32(ERROR_TIMEOUT));}
    }catch(...){FinishWatch(watch);Log(L"settings_bootstrap_failed",E_FAIL);}
}
void WatchBrowser(cef_browser_t* browser){
    if(!browser||browser->base.size!=sizeof(cef_browser_t))return;
    TrackBrowser(browser);
    int id=browser->get_identifier(browser);if(watches.contains(id))return;
    auto watch=std::make_shared<BrowserWatch>(browser);watches.emplace(id,watch);watch->timer=SetTimer(nullptr,0,500,&BootstrapTick);
    if(!watch->timer){const auto error=GetLastError();FinishWatch(watch);Log(L"settings_bootstrap_schedule_failed",HRESULT_FROM_WIN32(error));}
}
using CreateFn=decltype(&cef_browser_host_create_browser);
using CreateSyncFn=decltype(&cef_browser_host_create_browser_sync);
CreateFn originalCreate{};CreateSyncFn originalCreateSync{};
int HookCreate(const cef_window_info_t* window,cef_client_t* client,const cef_string_t* url,
               const cef_browser_settings_t* settings,cef_dictionary_value_t* info,cef_request_context_t* context) {
    Log(L"cef_create_async");Observe(client);return originalCreate(window,client,url,settings,info,context);
}
cef_browser_t* HookCreateSync(const cef_window_info_t* window,cef_client_t* client,const cef_string_t* url,
                            const cef_browser_settings_t* settings,cef_dictionary_value_t* info,cef_request_context_t* context) {
    Log(L"cef_create_sync");Observe(client);auto* browser=originalCreateSync(window,client,url,settings,info,context);WatchBrowser(browser);return browser;
}
} // namespace
bool InstallCefHooks(HMODULE module,HMODULE plugin) {
    if(!KnownCef(FileHash(ModulePath(module)))){Log(L"unsupported_cef",E_NOTIMPL);return false;}
    HRSRC resource=FindResourceW(plugin,MAKEINTRESOURCEW(101),RT_RCDATA);
    if(!resource)return false;auto* bytes=static_cast<const char*>(LockResource(LoadResource(plugin,resource)));
    if(!bytes)return false;uiSource=Utf16(std::string_view(bytes,SizeofResource(plugin,resource)));
    freeString=reinterpret_cast<FreeStringFn>(GetProcAddress(module,"cef_string_userfree_utf16_free"));
    void* create=reinterpret_cast<void*>(GetProcAddress(module,"cef_browser_host_create_browser"));
    void* sync=reinterpret_cast<void*>(GetProcAddress(module,"cef_browser_host_create_browser_sync"));
    if(!freeString||!create||!sync)return false;
    if(MH_CreateHook(create,reinterpret_cast<void*>(&HookCreate),reinterpret_cast<void**>(&originalCreate))!=MH_OK)return false;
    if(MH_CreateHook(sync,reinterpret_cast<void*>(&HookCreateSync),reinterpret_cast<void**>(&originalCreateSync))!=MH_OK){MH_RemoveHook(create);return false;}
    MH_QueueEnableHook(create);MH_QueueEnableHook(sync);
    if(MH_ApplyQueued()!=MH_OK){MH_DisableHook(create);MH_DisableHook(sync);MH_RemoveHook(create);MH_RemoveHook(sync);return false;}
    Log(L"cef_hooks_ready");return true;
}
}
