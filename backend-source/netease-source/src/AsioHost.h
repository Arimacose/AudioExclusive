#pragma once
#include <audioclient.h>
#include <string>
namespace nce {
std::wstring AsioSelection();
bool SetAsioSelection(const std::wstring& id);
std::wstring AsioStatusJson();
// Takes ownership of nativeClient only on success.
HRESULT CreateAsioClient(IAudioClient* nativeClient, IAudioClient** result);
void PrepareNativeAsioFormat(const WAVEFORMATEX& format);
}
