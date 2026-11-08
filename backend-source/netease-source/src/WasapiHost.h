#pragma once
#include <audioclient.h>
#include <string>
namespace nce {
// Takes ownership of the uninitialized query client only on success.
HRESULT CreateWasapiClient(IAudioClient* query,const std::wstring& endpoint,IAudioClient** output);
std::wstring WasapiStatusJson();
}
