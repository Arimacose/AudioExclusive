#include "Support.h"
#include <tlhelp32.h>
#include <shlwapi.h>
#include <iostream>
#include <psapi.h>
#include <filesystem>
#include <vector>
#include <stdexcept>

namespace {
struct Handle {
    HANDLE value{};
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle()=default;explicit Handle(HANDLE handle):value(handle){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
std::uintptr_t RemoteModule(DWORD process,const std::wstring& file,bool fullPath) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,process));
    MODULEENTRY32W entry{sizeof(entry)};
    if(Module32FirstW(snapshot.value,&entry))do {
        const wchar_t* candidate=fullPath?entry.szExePath:entry.szModule;
        if(_wcsicmp(candidate,file.c_str())==0)return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    }while(Module32NextW(snapshot.value,&entry));
    // Before Ldr initializes its lists, Toolhelp cannot enumerate ntdll.
    // Resolve it from actual mapped-image regions instead of assuming ASLR bases.
    if(!fullPath) {
        Handle target(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,process));
        MEMORY_BASIC_INFORMATION region{};std::uintptr_t address{};
        while(target.value && VirtualQueryEx(target.value,reinterpret_cast<void*>(address),&region,sizeof(region))) {
            if(region.Type==MEM_IMAGE && region.AllocationBase==region.BaseAddress) {
                wchar_t mapped[32768]{};
                if(GetMappedFileNameW(target.value,region.AllocationBase,mapped,32768) &&
                   _wcsicmp(std::filesystem::path(mapped).filename().c_str(),file.c_str())==0)
                    return reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            }
            auto next=reinterpret_cast<std::uintptr_t>(region.BaseAddress)+region.RegionSize;
            if(next<=address)break;address=next;
        }
    }
    return 0;
}
bool AlreadyRunning(const std::wstring& executable) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W entry{sizeof(entry)};
    if(Process32FirstW(snapshot.value,&entry))do {
        if(_wcsicmp(entry.szExeFile,L"cloudmusic.exe"))continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID));
        if(!process.value)continue;
        std::wstring path(32768,L'\0');DWORD length=static_cast<DWORD>(path.size());
        if(QueryFullProcessImageNameW(process.value,0,path.data(),&length)){
            path.resize(length);if(_wcsicmp(path.c_str(),executable.c_str())==0)return true;}
    }while(Process32NextW(snapshot.value,&entry));return false;
}
DWORD RunRemote(HANDLE process,LPTHREAD_START_ROUTINE entry,void* argument) {
    Handle thread(CreateRemoteThread(process,nullptr,0,entry,argument,0,nullptr));
    if(!thread.value)throw std::runtime_error("CreateRemoteThread failed");
    if(WaitForSingleObject(thread.value,15000)!=WAIT_OBJECT_0)throw std::runtime_error("bootstrap timed out");
    DWORD result{};if(!GetExitCodeThread(thread.value,&result))throw std::runtime_error("thread result");return result;
}
std::uintptr_t RemoteSystemExport(DWORD process,const char* name) {
    auto function=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),name);HMODULE actualModule{};
    if(!function||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(function),&actualModule))throw std::runtime_error("system export");
    std::wstring file=nce::ModulePath(actualModule);std::wstring basename=std::filesystem::path(file).filename().wstring();
    auto remote=RemoteModule(process,basename,false);if(!remote)throw std::runtime_error("remote system module");
    return remote+reinterpret_cast<std::uintptr_t>(function)-reinterpret_cast<std::uintptr_t>(actualModule);
}
void Bootstrap(HANDLE process,DWORD processId,const std::wstring& dll) {
    // CREATE_SUSPENDED precedes the initial user-mode loader pass. A remote
    // no-op thread completes that pass while the application's main thread
    // stays suspended; ntdll is already mapped at process creation.
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
    auto exitThread=GetProcAddress(ntdll,"RtlExitUserThread");
    auto remoteNtdll=RemoteModule(processId,L"ntdll.dll",false);
    if(!exitThread||!remoteNtdll)throw std::runtime_error("initial loader bootstrap");
    RunRemote(process,reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteNtdll+
        reinterpret_cast<std::uintptr_t>(exitThread)-reinterpret_cast<std::uintptr_t>(ntdll)),nullptr);
    SIZE_T bytes=(dll.size()+1)*sizeof(wchar_t),written{};
    void* remotePath=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!remotePath)throw std::runtime_error("remote allocation");
    try {
        if(!WriteProcessMemory(process,remotePath,dll.c_str(),bytes,&written)||written!=bytes)throw std::runtime_error("remote path write");
        auto load=RemoteSystemExport(processId,"LoadLibraryW");
        RunRemote(process,reinterpret_cast<LPTHREAD_START_ROUTINE>(load),remotePath);
    }catch(...){VirtualFreeEx(process,remotePath,0,MEM_RELEASE);throw;}
    VirtualFreeEx(process,remotePath,0,MEM_RELEASE);
    auto remoteDll=RemoteModule(processId,dll,true);if(!remoteDll)throw std::runtime_error("hook DLL not loaded");
    // Map for export-address resolution only. This does not execute DllMain.
    HMODULE mapped=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
    if(!mapped)throw std::runtime_error("hook export map");
    auto init=GetProcAddress(mapped,"NCExclusiveInitialize");
    std::uintptr_t entry=init?remoteDll+reinterpret_cast<std::uintptr_t>(init)-reinterpret_cast<std::uintptr_t>(mapped):0;
    FreeLibrary(mapped);if(!entry)throw std::runtime_error("hook initialization export");
    if(RunRemote(process,reinterpret_cast<LPTHREAD_START_ROUTINE>(entry),nullptr)!=1)throw std::runtime_error("hook initialization failed");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3 || std::wstring_view(argv[1])!=L"--launch") {
        std::wcerr<<L"用法：nc-exclusive-launcher.exe --launch \"C:\\Program Files\\NetEase\\CloudMusic\\cloudmusic.exe\"\n"
                    L"此命令会启动客户端并加载插件；请在获准部署及验收阶段才运行。\n";return 2;
    }
    try {
        auto executable=std::filesystem::absolute(argv[2]).lexically_normal();
        auto directory=executable.parent_path();
        const auto executableHash=nce::FileHash(executable.wstring());
        const auto nativeHash=nce::FileHash((directory/L"cloudmusic.dll").wstring());
        const auto cefHash=nce::FileHash((directory/L"libcef.dll").wstring());
        bool supported=false;
        for(const auto& profile:nce::kClientProfiles)
            if(executableHash==profile.executableHash&&nativeHash==profile.nativeHash&&cefHash==profile.cefHash){supported=true;break;}
        if(!supported)
            throw std::runtime_error("unsupported client: exact build fingerprints do not match");
        if(AlreadyRunning(executable.wstring()))throw std::runtime_error("client is already running; close it before explicitly launching with this plugin");
        auto dll=(std::filesystem::path(nce::ModulePath(nullptr)).parent_path()/L"nc-exclusive-hook.dll").wstring();
        if(!std::filesystem::is_regular_file(dll))throw std::runtime_error("hook DLL missing");
        // No startup registration, task, original-file rewrite, or termination
        // of an existing client. Failure terminates only this newly created,
        // still suspended child; a failed launch does not leave it frozen.
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION information{};
        std::wstring command=L"\""+executable.wstring()+L"\"";
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,
                            nullptr,directory.c_str(),&startup,&information))throw std::runtime_error("CreateProcess failed");
        Handle process(information.hProcess),thread(information.hThread);
        try {Bootstrap(process.value,information.dwProcessId,dll);
            if(ResumeThread(thread.value)==static_cast<DWORD>(-1))throw std::runtime_error("ResumeThread failed");}
        catch(...){TerminateProcess(process.value,1);WaitForSingleObject(process.value,5000);throw;}
        std::wcout<<L"已启动插件引导。音频钩子、界面注入和实际独占状态请分别查看插件日志与设置页。\n";return 0;
    }catch(const std::exception& error){std::cerr<<"NCExclusive: "<<error.what()<<'\n';return 1;}
}
