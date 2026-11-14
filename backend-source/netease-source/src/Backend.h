#pragma once
#include "Support.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdint>

namespace nce {
enum class Phase {Disabled, Armed, Initializing, Open, Active, Paused, Faulted, Unsupported};
struct StreamState {
    bool exclusive{},opened{},running{};
    void* client{}; void* render{};
    std::uint32_t rate{},frames{};
    std::uint16_t bits{},channels{};
    std::uint64_t submitted{};
    std::uint64_t nonSilentPackets{};
    std::uint64_t generation{};
    HRESULT lastError{S_OK};
    std::wstring endpoint;
};
class Backend final {
public:
    static Backend& Instance();
    void LoadConfig();
    bool SetEnabled(bool enabled);
    bool Enabled() const noexcept {return enabled_.load(std::memory_order_acquire);}
    void Compatibility(bool supported);
    void UiReady(bool ready);
    void OutputFault(HRESULT error);
    void OpenBegin(void* stream,bool exclusive);
    void OpenEnd(void* stream,bool result,void* client,void* render,const void* format);
    void Initialized(void* stream,HRESULT result,std::uint32_t frames,std::wstring endpoint);
    void Started(void* client,HRESULT result);
    void Submitted(void* render,std::uint32_t frames,HRESULT result);
    void Content(void* render);
    void Stopped(void* stream);
    void Closed(void* stream);
    std::wstring StatusJson();
private:
    std::mutex configMutex_;
    std::mutex mutex_;
    std::atomic<bool> enabled_{};
    bool supported_{},uiReady_{};
    std::uint64_t sequence_{};
    HRESULT lastFault_{S_OK};
    std::unordered_map<void*,StreamState> streams_;
};
bool InstallCefHooks(HMODULE module,HMODULE plugin);
std::wstring ActualAudioStatusJson();
}
