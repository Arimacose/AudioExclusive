#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>
#include <mutex>

// Apple-private ABI inventory for the verified Apple Music 1.6.4.90 /
// package-framework 1.1540.23042.0 x64 build. Each entry records the semantic
// role and the fingerprint that must accompany it. Public API constants live
// elsewhere; private RVAs, object fields, call-stack fingerprints and observed
// binary layouts are kept here so a package update has one review surface.
namespace ammod::apple_private {

namespace verified_build {
inline constexpr wchar_t kAppleMusicFileVersion[] = L"1.6.4.90";
inline constexpr wchar_t kPackageVersion[] = L"1.1540.23042.0";
inline constexpr wchar_t kArchitecture[] = L"x64";
} // namespace verified_build

namespace core_media_filter {
inline constexpr std::size_t kFilterClassTable = 0x18;
inline constexpr std::size_t kClassTableMethods = 0x10;
inline constexpr std::size_t kSimpleApplySlot = 0x18;
inline constexpr std::size_t kTreeApplySlot = 0x20;
inline constexpr std::size_t kSimplePredicate = 0x40;
inline constexpr std::size_t kTreeChildren = 0x30;
inline constexpr std::size_t kTreeFallback = 0x38;
inline constexpr std::size_t kStrictContext = 0x68;
// Constructor fingerprint for the verified AllowableMediaSubtypes simple filter.
// A different priority/callback shape means the private ABI has drifted.
inline constexpr std::uint32_t kStrictConstructorPriority = 0x375;
// Fingerprint: the method-table slots above must resolve to these CoreMedia RVAs.
inline std::uintptr_t kSimpleApplyRva = 0x006CA8E0;
inline std::uintptr_t kTreeApplyRva = 0x006E2F10;
} // namespace core_media_filter

namespace agent_stack {
// Fingerprint for the AppleMusic.exe local-file seek path calling
// AudioConverterReset. These are image-relative return-address RVAs, not
// absolute process addresses. The third frame has two observed compiler paths.
inline constexpr std::size_t kLocalSeekResetFrames = 3;
inline std::uintptr_t kLocalSeekResetFrame0 = 0x0040B9FA;
inline std::uintptr_t kLocalSeekResetFrame1 = 0x0082F6BC;
inline std::array<std::uintptr_t, 2> kLocalSeekResetFrame2{
    0x00B1E8AD, 0x00B1E972};
} // namespace agent_stack

namespace core_media_seek {
// Retired hook inventory. Fingerprint: exact prologue match before any hook.
inline std::uintptr_t kSetCurrentTimeRva = 0x008EA2C0;
inline constexpr std::array<std::uint8_t, 15> kSetCurrentTimePrologue{
    0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
    0x57, 0x41, 0x56, 0x41, 0x57};
} // namespace core_media_seek

namespace audio_converter_property {
inline constexpr std::uint32_t kCursor = 0x63706563; // 'cpec'
inline constexpr std::uint32_t kCurrentInputDescription = 0x61636964; // 'acid'
inline constexpr std::uint32_t kCurrentOutputDescription = 0x61636F64; // 'acod'
} // namespace audio_converter_property

namespace alac_cookie {
// ALACSpecificConfig can appear bare, after an atom header, or after two atom
// headers. Fingerprint: supported bit depth/channels/rate sanity checks at use.
inline constexpr std::array<std::uint32_t, 3> kCandidateOffsets{0, 12, 24};
inline constexpr std::uint32_t kConfigBytes = 24;
inline constexpr std::uint32_t kBitDepth = 5;
inline constexpr std::uint32_t kChannels = 9;
inline constexpr std::uint32_t kSampleRate = 20;
} // namespace alac_cookie

namespace playback_bitrate_monitor {
// Observed constructor ABI: the tenth argument is stored at this object field.
inline constexpr std::size_t kStartsOnFirstEligibleVariant = 0x58;
} // namespace playback_bitrate_monitor

// The launcher verifies complete SHA256 profiles. This independent in-process
// identity check prevents choosing RVAs for another loaded module or mixed build.
// Initialize before installing either the quality or audio hooks; call_once
// publishes the selected offsets to both worker threads.
inline bool InitializeVerifiedProfile() {
    const auto media = GetModuleHandleW(L"CoreMedia.dll");
    const auto agent = GetModuleHandleW(nullptr);
    if (!media || !agent) return false;
    static std::once_flag once;
    static bool supported = false;
    std::call_once(once, [=] {
        const auto matches = [](HMODULE module, DWORD stamp, DWORD size) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
            return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                nt->FileHeader.TimeDateStamp == stamp && nt->OptionalHeader.SizeOfImage == size;
        };
        if (matches(media, 0x69B9C1B1, 0x1498000) && matches(agent, 0x69C4EC34, 0x1262000)) {
            supported = true;
        } else if (matches(media, 0x6AC9C301, 0x16B7000) && matches(agent, 0x6AD9AC61, 0x1296000)) {
            core_media_filter::kSimpleApplyRva = 0x007DC3A0;
            core_media_filter::kTreeApplyRva = 0x007F4A60;
            core_media_seek::kSetCurrentTimeRva = 0x00A14CA0;
            agent_stack::kLocalSeekResetFrame0 = 0x00435FE6;
            agent_stack::kLocalSeekResetFrame1 = 0x008669AB;
            agent_stack::kLocalSeekResetFrame2 = {0x00B5BCA1, 0x00B5BD64};
            supported = true;
        } else if (matches(media, 0x6AC9C301, 0x16B7000) && matches(agent, 0x6AE93C6C, 0x1296000)) {
            core_media_filter::kSimpleApplyRva = 0x007DC3A0;
            core_media_filter::kTreeApplyRva = 0x007F4A60;
            core_media_seek::kSetCurrentTimeRva = 0x00A14CA0;
            agent_stack::kLocalSeekResetFrame0 = 0x00435FE6;
            agent_stack::kLocalSeekResetFrame1 = 0x008669AB;
            agent_stack::kLocalSeekResetFrame2 = {0x00B5BD15, 0x00B5BDD8};
            supported = true;
        }
    });
    return supported;
}

} // namespace ammod::apple_private
