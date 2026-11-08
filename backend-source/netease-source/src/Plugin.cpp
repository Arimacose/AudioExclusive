#include "Backend.h"
#include <MinHook.h>
#include <mutex>

namespace nce { bool InstallActualAudioObserver(); bool InstallActualNativeHooks(HMODULE); namespace {
HMODULE pluginModule{};
using LoadFn=HMODULE(WINAPI*)(LPCWSTR);
using LoadExFn=HMODULE(WINAPI*)(LPCWSTR,HANDLE,DWORD);
LoadFn originalLoad{};LoadExFn originalLoadEx{};
std::mutex moduleMutex;
bool nativeAttempted{},cefAttempted{};
LONG CALLBACK DiagnoseException(EXCEPTION_POINTERS* exception) {
    if(exception->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION ||
       exception->ExceptionRecord->ExceptionCode==EXCEPTION_ILLEGAL_INSTRUCTION ||
       exception->ExceptionRecord->ExceptionCode==0xC0000409) {
        HMODULE module{};
        auto address=exception->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),&module);
        Log(L"exception_at_"+ModulePath(module)+L"_rva_"+
            std::to_wstring(reinterpret_cast<std::uintptr_t>(address)-reinterpret_cast<std::uintptr_t>(module)),
            static_cast<HRESULT>(exception->ExceptionRecord->ExceptionCode));
        auto* record=exception->ExceptionRecord;
        if(record->NumberParameters>=2)Log(L"exception_access_"+std::to_wstring(record->ExceptionInformation[0])+
            L"_address_"+std::to_wstring(record->ExceptionInformation[1]));
        void* stack[24]{};USHORT depth=CaptureStackBackTrace(0,24,stack,nullptr);
        for(USHORT i=0;i<depth;++i) {
            HMODULE frameModule{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(stack[i]),&frameModule);
            Log(L"exception_stack_"+std::to_wstring(i)+L"_"+ModulePath(frameModule)+L"_rva_"+
                std::to_wstring(reinterpret_cast<std::uintptr_t>(stack[i])-reinterpret_cast<std::uintptr_t>(frameModule)));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
bool isMainProcess() {
    std::wstring command=GetCommandLineW();
    return command.find(L"--type=")==std::wstring::npos &&
        KnownExecutable(FileHash(ModulePath(nullptr)));
}
void CheckModules() noexcept {
    static thread_local bool checking{};
    if(checking)return;
    struct ScopedCheck{bool& value;ScopedCheck(bool& flag):value(flag){value=true;}~ScopedCheck(){value=false;}} scoped(checking);
    try {
        std::lock_guard guard(moduleMutex);
        if(!nativeAttempted)if(HMODULE module=GetModuleHandleW(L"cloudmusic.dll")){
            nativeAttempted=true;
            if(!InstallActualNativeHooks(module)){Backend::Instance().Compatibility(false);Log(L"native_install_failed",E_NOTIMPL);}}
        if(!cefAttempted)if(HMODULE module=GetModuleHandleW(L"libcef.dll")){
            cefAttempted=true;
            if(!InstallCefHooks(module,pluginModule))Log(L"cef_install_failed",E_NOTIMPL);}
    }catch(...){Log(L"module_install_exception",E_FAIL);}
}
HMODULE WINAPI HookLoad(LPCWSTR path) {
    HMODULE module=originalLoad(path);DWORD error=GetLastError();
    if(module)CheckModules();SetLastError(error);return module;
}
HMODULE WINAPI HookLoadEx(LPCWSTR path,HANDLE file,DWORD flags) {
    HMODULE module=originalLoadEx(path,file,flags);DWORD error=GetLastError();
    if(module && !(flags&(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|LOAD_LIBRARY_AS_IMAGE_RESOURCE|DONT_RESOLVE_DLL_REFERENCES)))CheckModules();
    SetLastError(error);return module;
}
}
}
extern "C" __declspec(dllexport) DWORD WINAPI NCExclusiveInitialize(void*) {
    using namespace nce;
    static std::mutex initMutex;static bool ready{};
    std::lock_guard guard(initMutex);if(ready)return 1;
    try {
        if(!isMainProcess())return 0;
        AddVectoredExceptionHandler(1,DiagnoseException);
        Backend::Instance().LoadConfig();
        if(MH_Initialize()!=MH_OK)return 0;
        if(!InstallActualAudioObserver()){MH_Uninitialize();return 0;}
        HMODULE kernel=GetModuleHandleW(L"kernel32.dll");
        void* load=reinterpret_cast<void*>(GetProcAddress(kernel,"LoadLibraryW"));
        void* loadEx=reinterpret_cast<void*>(GetProcAddress(kernel,"LoadLibraryExW"));
        if(MH_CreateHook(load,reinterpret_cast<void*>(&HookLoad),reinterpret_cast<void**>(&originalLoad))!=MH_OK)return 0;
        if(MH_CreateHook(loadEx,reinterpret_cast<void*>(&HookLoadEx),reinterpret_cast<void**>(&originalLoadEx))!=MH_OK){MH_RemoveHook(load);return 0;}
        MH_QueueEnableHook(load);MH_QueueEnableHook(loadEx);
        if(MH_ApplyQueued()!=MH_OK){MH_DisableHook(load);MH_DisableHook(loadEx);MH_RemoveHook(load);MH_RemoveHook(loadEx);return 0;}
        CheckModules();ready=true;Log(L"bootstrap_ready");return 1;
    }catch(...){Log(L"bootstrap_failed",E_FAIL);return 0;}
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){nce::pluginModule=module;DisableThreadLibraryCalls(module);}
    // Hooks are installed by the explicit exported entry outside loader lock.
    // Unload only with process exit; live FreeLibrary would invalidate hooks.
    return TRUE;
}
