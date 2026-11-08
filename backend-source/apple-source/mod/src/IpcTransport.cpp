#include "IpcTransport.h"

#include <sddl.h>

#include <vector>

namespace ammod::ipc {

std::wstring CurrentUserSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<std::byte> buffer(bytes);
    if (!bytes || !GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR text{};
    if (!ConvertSidToStringSidW(user->User.Sid, &text) || !text) return {};
    std::wstring result(text);
    LocalFree(text);
    return result;
}

std::wstring PipeName() {
    const auto sid = CurrentUserSid();
    return sid.empty() ? std::wstring{} : L"\\\\.\\pipe\\AppleMusicExclusive.v1." + sid;
}

Message NewMessage(MessageType type, Role role) {
    Message message{};
    message.type = type;
    message.role = role;
    message.senderPid = GetCurrentProcessId();
    GetSystemTimeAsFileTime(&message.timestampUtc);
    CoCreateGuid(&message.requestId);
    return message;
}

namespace {
bool TransferMessage(HANDLE pipe, Message& message, bool write, DWORD timeoutMs) {
    OVERLAPPED operation{};
    operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!operation.hEvent)return false;
    DWORD transferred{};
    const BOOL immediate=write
        ? WriteFile(pipe,&message,sizeof(message),&transferred,&operation)
        : ReadFile(pipe,&message,sizeof(message),&transferred,&operation);
    DWORD error=immediate?ERROR_SUCCESS:GetLastError();
    bool ok=immediate!=FALSE;
    if(!immediate&&error==ERROR_IO_PENDING) {
        const DWORD wait=WaitForSingleObject(operation.hEvent,timeoutMs);
        if(wait==WAIT_OBJECT_0) {
            ok=GetOverlappedResult(pipe,&operation,&transferred,FALSE)!=FALSE;
            error=ok?ERROR_SUCCESS:GetLastError();
        } else {
            error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
            CancelIoEx(pipe,&operation);
            // OVERLAPPED and its buffer must remain alive until cancellation
            // finishes. Local named-pipe operations support CancelIoEx.
            GetOverlappedResult(pipe,&operation,&transferred,TRUE);
            ok=false;
        }
    }
    CloseHandle(operation.hEvent);
    SetLastError(error);
    return ok&&transferred==sizeof(message);
}
}

bool ReadMessage(HANDLE pipe, Message& message) {
    return TransferMessage(pipe,message,false,1000)&&IsValid(message);
}

bool ReadMessageTimeout(HANDLE pipe, Message& message, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available >= sizeof(Message)) return ReadMessage(pipe, message);
        if (GetTickCount64() >= deadline) {
            SetLastError(ERROR_TIMEOUT);
            return false;
        }
        Sleep(10);
    }
}

bool WriteMessage(HANDLE pipe, const Message& message) {
    if(!IsValid(message))return false;
    auto buffer=message;
    return TransferMessage(pipe,buffer,true,200);
}

PipeSecurity::PipeSecurity() {
    const auto sid = CurrentUserSid();
    if (sid.empty()) return;
    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) return;
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
    valid_ = true;
}

PipeSecurity::~PipeSecurity() {
    if (descriptor_) LocalFree(descriptor_);
}

} // namespace ammod::ipc
