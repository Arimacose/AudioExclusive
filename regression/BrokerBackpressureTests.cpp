
#include "IpcTransport.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
using namespace ammod::ipc;
struct Client{HANDLE pipe;Role role;DWORD pid;};
std::mutex g_mutex;std::vector<Client> g_clients;
void Log(const std::wstring&){}
std::vector<std::pair<HANDLE, DWORD>> SnapshotTargets(HANDLE source) {
    std::vector<std::pair<HANDLE, DWORD>> targets;
    std::lock_guard lock(g_mutex);
    auto append = [&](const Client& client) {
        HANDLE duplicate{};
        if (DuplicateHandle(GetCurrentProcess(), client.pipe, GetCurrentProcess(), &duplicate,
                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
            targets.emplace_back(duplicate, client.pid);
        }
    };
    if (source) {
        for (const auto& client : g_clients) if (client.pipe == source) append(client);
    }
    for (const auto& client : g_clients) if (client.pipe != source) append(client);
    return targets;
}

void Broadcast(const Message& message, HANDLE source = nullptr) {
    // There are at most eight pipe clients. Use the existing bounded set of
    // broker workers; never create a thread for an individual status message.
    static std::mutex broadcastMutex;
    std::lock_guard sendLock(broadcastMutex);
    auto targets=SnapshotTargets(source);
    for(const auto& [pipe,pid]:targets) {
        const bool written=WriteMessage(pipe,message);
        if(!written)DisconnectNamedPipe(pipe);
        Log(L"broadcast pid="+std::to_wstring(pid)+L" ok="+std::to_wstring(written));
        CloseHandle(pipe);
    }
}

unsigned Threads(){unsigned n=0;HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 t{sizeof(t)};
 if(Thread32First(s,&t))do{if(t.th32OwnerProcessID==GetCurrentProcessId())++n;}while(Thread32Next(s,&t));CloseHandle(s);return n;}
void Snapshot(const char* phase){DWORD handles{};GetProcessHandleCount(GetCurrentProcess(),&handles);
 PROCESS_MEMORY_COUNTERS_EX m{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m));
 std::cout<<"{\"phase\":\""<<phase<<"\",\"threads\":"<<Threads()<<",\"handles\":"<<handles<<",\"privateBytes\":"<<m.PrivateUsage<<",\"messageBytes\":"<<sizeof(Message)<<"}\n";}
int main(){
 const auto name=L"\\\\.\\pipe\\AudioExclusiveAuditIsolated-"+std::to_wstring(GetCurrentProcessId());
 HANDLE server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,4096,4096,0,nullptr);
 HANDLE reader=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
 if(server==INVALID_HANDLE_VALUE||reader==INVALID_HANDLE_VALUE)return 2;
 if(!ConnectNamedPipe(server,nullptr)&&GetLastError()!=ERROR_PIPE_CONNECTED)return 3;
 g_clients.push_back({server,Role::Ui,GetCurrentProcessId()});
 const auto initialThreads=Threads();
 Snapshot("baseline");auto m=NewMessage(MessageType::StatusChanged,Role::Broker);
 for(int i=0;i<32;++i)Broadcast(m,nullptr);Sleep(300);Snapshot("32BroadcastsNoReader");
 for(int i=0;i<32;++i)Broadcast(m,nullptr);Sleep(300);Snapshot("64BroadcastsNoReader");
 if(Threads()>initialThreads+1)return 4;
 CloseHandle(reader);g_clients.clear();CloseHandle(server);Sleep(1000);Snapshot("afterReaderClose");
}
