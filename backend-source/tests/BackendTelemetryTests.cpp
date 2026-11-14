#include <windows.h>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
static HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
static HANDLE finish=CreateEventW(nullptr,TRUE,FALSE,nullptr);
static BOOL WINAPI BlockingProfileWrite(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR){SetEvent(entered);WaitForSingleObject(finish,5000);return TRUE;}
#define WritePrivateProfileStringW BlockingProfileWrite
#include "NativeBackend.cpp"
#undef WritePrivateProfileStringW
namespace nce {
std::wstring ConfigPath(){return L"unused.ini";}
std::wstring JsonString(std::wstring_view value){return L"\""+std::wstring(value)+L"\"";}
std::wstring AsioStatusJson(){return L"{}";}
void Log(std::wstring_view,HRESULT) noexcept{}
}
int main(){
    auto& backend=nce::Backend::Instance();backend.Compatibility(true);
    void* stream=reinterpret_cast<void*>(1);void* render=reinterpret_cast<void*>(2);
    backend.OpenBegin(stream,true);backend.OpenEnd(stream,true,stream,render,nullptr);
    auto setting=std::async(std::launch::async,[&]{return backend.SetEnabled(true);});
    if(WaitForSingleObject(entered,2000)!=WAIT_OBJECT_0){SetEvent(finish);return 1;}
    auto producer=std::async(std::launch::async,[&]{backend.Submitted(render,960,S_OK);backend.Content(render);});
    const bool progressed=producer.wait_for(std::chrono::seconds(1))==std::future_status::ready;
    SetEvent(finish);producer.get();const bool persisted=setting.get();
    CloseHandle(entered);CloseHandle(finish);
    if(!progressed||!persisted){std::cerr<<"settings I/O blocked PCM telemetry\n";return 1;}
    std::cout<<"PCM submission progresses while configuration storage is blocked\n";return 0;
}
