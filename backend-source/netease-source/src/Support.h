#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <cstdint>

namespace nce {
struct ClientProfile {
    const wchar_t* version;
    const wchar_t* executableHash;
    const wchar_t* nativeHash;
    const wchar_t* cefHash;
    std::uintptr_t openRva;
    std::uintptr_t convertRva;
};
inline constexpr ClientProfile kClientProfiles[] = {
    {L"3.1.36.205322",L"1b86292da1056a729226df9205eb9f69c1e5558bc3eddbe0639c06a2443bf2d3",
     L"98874023cab8d13f4e7e3fbc74fbc30186529039593e1359a189878ece35029f",
     L"724b3e35edb5905540877fa8d7a8583a2503599639d7c79ccef6faa8e5a6bc49",0x106bd70,0x10704c0},
    {L"3.1.41.205564",L"9bc367409cd571de11ff814636ce665eac762452e7e0763a1debc39792344f0a",
     L"d1b6d5da6143d414f6cdc54c5ca156617dfbb411a738768903bcb7393b6dba99",
     L"a2a441490a9600f7e06b8c4dc609354ff7214add445a55c8fa699760d264e210",0x108f950,0x10940a0}
};
inline bool KnownExecutable(std::wstring_view hash) {
    for(const auto& profile:kClientProfiles)if(hash==profile.executableHash)return true;return false;
}
inline bool KnownCef(std::wstring_view hash) {
    for(const auto& profile:kClientProfiles)if(hash==profile.cefHash)return true;return false;
}
std::wstring ModulePath(HMODULE module);
std::wstring FileHash(const std::wstring& path);
std::wstring Utf16(std::string_view input);
std::string Utf8(std::wstring_view input);
std::wstring JsonString(std::wstring_view input);
std::wstring ConfigPath();
void Log(std::wstring_view event, HRESULT result = S_OK) noexcept;
}
