#include "Support.h"
#include <bcrypt.h>
#include <shlwapi.h>
#include <array>
#include <vector>
#include <mutex>
#include <stdexcept>

namespace nce {
std::wstring ModulePath(HMODULE module) {
    std::wstring value(32768, L'\0');
    DWORD length=GetModuleFileNameW(module,value.data(),static_cast<DWORD>(value.size()));
    if(!length || length>=value.size()) throw std::runtime_error("module path");
    value.resize(length); return value;
}
std::wstring FileHash(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file==INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    ULONG size{},copied{}; bool success=false; std::array<UCHAR,32> digest{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
       BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&copied,0)>=0) {
        std::vector<UCHAR> storage(size), block(65536);
        if(BCryptCreateHash(algorithm,&hash,storage.data(),size,nullptr,0,0)>=0) {
            success=true; DWORD count{};
            for(;;) {
                if(!ReadFile(file,block.data(),static_cast<DWORD>(block.size()),&count,nullptr)){success=false;break;}
                if(!count) break;
                if(BCryptHashData(hash,block.data(),count,0)<0){success=false;break;}
            }
            if(success) success=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
            BCryptDestroyHash(hash);
        }
    }
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file); if(!success)return {};
    constexpr wchar_t digits[]=L"0123456789abcdef"; std::wstring result;
    for(auto byte:digest){result+=digits[byte>>4];result+=digits[byte&15];}
    return result;
}
std::wstring Utf16(std::string_view input) {
    if(input.empty()) return {};
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if(length<=0) throw std::runtime_error("invalid UTF-8");
    std::wstring result(length,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),result.data(),length);
    return result;
}
std::string Utf8(std::wstring_view input) {
    if(input.empty()) return {};
    int length=WideCharToMultiByte(CP_UTF8,0,input.data(),static_cast<int>(input.size()),nullptr,0,nullptr,nullptr);
    std::string result(length,'\0');
    WideCharToMultiByte(CP_UTF8,0,input.data(),static_cast<int>(input.size()),result.data(),length,nullptr,nullptr);
    return result;
}
std::wstring JsonString(std::wstring_view input) {
    std::wstring result=L"\""; constexpr wchar_t hex[]=L"0123456789abcdef";
    for(wchar_t ch:input) {
        if(ch==L'"'||ch==L'\\'){result+=L'\\';result+=ch;}
        else if(ch<32 || ch==0x2028 || ch==0x2029) {
            result+=L"\\u"; for(int shift=12;shift>=0;shift-=4)result+=hex[(ch>>shift)&15];
        } else result+=ch;
    }
    return result+L'"';
}
std::wstring ConfigPath() {
    wchar_t local[32768]{}; DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if(!length||length>=32768)throw std::runtime_error("LOCALAPPDATA unavailable");
    std::wstring directory=std::wstring(local)+L"\\NCExclusive";
    if(!CreateDirectoryW(directory.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)
        throw std::runtime_error("cannot create plugin settings directory");
    return directory+L"\\settings.ini";
}
void Log(std::wstring_view event,HRESULT result) noexcept {
    try {
        static std::mutex lock; std::lock_guard guard(lock);
        std::wstring path=ConfigPath(); path.resize(path.size()-12);
        path+=L"plugin-"+std::to_wstring(GetCurrentProcessId())+L".jsonl";
        HANDLE file=CreateFileW(path.c_str(),FILE_APPEND_DATA|GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
        LARGE_INTEGER size{};
        if(GetFileSizeEx(file,&size)&&size.QuadPart>1024*1024) {
            CloseHandle(file); std::wstring archive=path+L".previous";
            MoveFileExW(path.c_str(),archive.c_str(),MOVEFILE_REPLACE_EXISTING);
            file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        }
        if(file==INVALID_HANDLE_VALUE)return;
        std::string line=Utf8(L"{\"tick\":"+std::to_wstring(GetTickCount64())+L",\"event\":"+JsonString(event)+
                              L",\"hresult\":"+std::to_wstring(static_cast<unsigned long>(result))+L"}\n");
        DWORD written{}; WriteFile(file,line.data(),static_cast<DWORD>(line.size()),&written,nullptr); CloseHandle(file);
    } catch(...) {}
}
}
