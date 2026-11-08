#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include "PayloadHashes.h"

namespace fs=std::filesystem;
namespace {
struct Handle {HANDLE value{};~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
std::string Hash(const BYTE* bytes,DWORD length) {
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};DWORD objectSize{},written{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider");
    struct Algorithm {BCRYPT_ALG_HANDLE value;~Algorithm(){BCryptCloseAlgorithmProvider(value,0);}} cleanup{algorithm};
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<BYTE*>(&objectSize),sizeof(objectSize),&written,0)<0)throw std::runtime_error("SHA256 property");
    std::vector<BYTE> object(objectSize);
    if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)<0)throw std::runtime_error("SHA256 create");
    struct HashCleanup {BCRYPT_HASH_HANDLE value;~HashCleanup(){BCryptDestroyHash(value);}} hashCleanup{hash};
    BYTE digest[32]{};
    if(BCryptHashData(hash,const_cast<BYTE*>(bytes),length,0)<0||BCryptFinishHash(hash,digest,sizeof(digest),0)<0)throw std::runtime_error("SHA256 digest");
    constexpr char digits[]="0123456789abcdef";std::string result;
    for(BYTE value:digest){result+=digits[value>>4];result+=digits[value&15];}return result;
}
void Extract(HINSTANCE instance,int id,const fs::path& path,const char* expected) {
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource)throw std::runtime_error("embedded resource missing");
    auto* data=static_cast<const BYTE*>(LockResource(LoadResource(instance,resource)));
    DWORD length=SizeofResource(instance,resource);
    if(!data||!length||Hash(data,length)!=expected)throw std::runtime_error("embedded payload checksum mismatch");
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data),length);output.close();
    if(!output)throw std::runtime_error("cannot write temporary resource");
}
std::wstring Quote(std::wstring_view value) {
    std::wstring result=L"\"";size_t slashes{};
    for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}
        if(c==L'\"'){result.append(slashes*2+1,L'\\');result+=c;}
        else{result.append(slashes,L'\\');result+=c;}slashes=0;}
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
fs::path PowerShell() {
    wchar_t programs[32768]{};
    if(GetEnvironmentVariableW(L"ProgramFiles",programs,32768)){
        auto installed=fs::path(programs)/L"PowerShell"/L"7"/L"pwsh.exe";
        if(fs::is_regular_file(installed))return installed;
    }
    // Keep the installer independent from temporary development runtimes
    // on PATH. Prefer installed PowerShell 7, then the Windows fallback.
    wchar_t windows[32768]{};if(!GetWindowsDirectoryW(windows,32768))throw std::runtime_error("Windows directory");
    auto legacy=fs::path(windows)/L"System32"/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    if(!fs::is_regular_file(legacy))throw std::runtime_error("PowerShell not found");return legacy;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    fs::path staging;
    try {
        int count{};LPWSTR* arguments=CommandLineToArgvW(GetCommandLineW(),&count);
        if(!arguments)throw std::runtime_error("command line");
        struct Arguments {LPWSTR* value;~Arguments(){LocalFree(value);}} argumentCleanup{arguments};
        std::wstring action=L"Auto",app=L"All";bool batch=false;
        for(int i=1;i<count;++i){std::wstring_view arg=arguments[i];
            if(arg==L"--batch")batch=true;
            else if(arg==L"--install")action=L"Install";
            else if(arg==L"--status")action=L"Status";
            else if(arg==L"--menu")action=L"Menu";
            else if(arg==L"--remove-netease"){action=L"Remove";app=L"NetEase";}
            else if(arg==L"--remove-apple"){action=L"Remove";app=L"AppleMusic";}
            else if(arg==L"--remove-all")action=L"Remove";
            else if(arg==L"--launch-netease"){action=L"Launch";app=L"NetEase";}
            else if(arg==L"--launch-apple"){action=L"Launch";app=L"AppleMusic";}
            else throw std::runtime_error("unsupported argument");
        }
        wchar_t temp[32768]{};if(!GetTempPathW(32768,temp))throw std::runtime_error("temporary directory");
        GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("temporary identifier");
        wchar_t guid[40]{};StringFromGUID2(id,guid,40);
        staging=fs::path(temp)/(std::wstring(L"AudioExclusive-")+guid);
        if(!fs::create_directory(staging))throw std::runtime_error("temporary directory collision");
        const auto script=staging/L"Manage-AudioExclusive.ps1",bundle=staging/L"payload.zip";
        Extract(instance,101,script,kManagerSha256);Extract(instance,102,bundle,kBundleSha256);
        const auto shell=PowerShell();
        std::wstring command=Quote(shell.wstring())+L" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "+Quote(script.wstring())+
            L" -BundlePath "+Quote(bundle.wstring())+L" -BundleHash "+std::wstring(kBundleSha256,kBundleSha256+64)+
            L" -Action "+action+L" -App "+app+(batch?L" -NoMenu":L"");
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION information{};
        if(batch){startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);}
        if(!CreateProcessW(shell.c_str(),command.data(),nullptr,nullptr,batch?TRUE:FALSE,
            batch?CREATE_NO_WINDOW:CREATE_NEW_CONSOLE,nullptr,staging.c_str(),&startup,&information))throw std::runtime_error("cannot start PowerShell");
        Handle process{information.hProcess},thread{information.hThread};
        WaitForSingleObject(process.value,INFINITE);DWORD result=2;GetExitCodeProcess(process.value,&result);
        // Only delete the two files this bootstrap created. The app/plugin
        // operations, backup and recovery all run in the displayed PowerShell.
        DeleteFileW(script.c_str());DeleteFileW(bundle.c_str());RemoveDirectoryW(staging.c_str());
        return static_cast<int>(result);
    }catch(const std::exception& error){
        std::wstring message=L"无法启动 PowerShell 独占插件管理器：";
        const std::string detail=error.what();message.append(detail.begin(),detail.end());
        MessageBoxW(nullptr,message.c_str(),L"AudioExclusive",MB_OK|MB_ICONERROR);return 2;
    }
}
