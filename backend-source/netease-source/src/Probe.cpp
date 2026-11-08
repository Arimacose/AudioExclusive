#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include "WasapiPolicy.h"

int wmain(int argc,wchar_t** argv) {
    const bool capabilities=argc>1&&wcscmp(argv[1],L"--capabilities")==0;
    const bool explicitEndpoint=capabilities?argc==4&&wcscmp(argv[2],L"--endpoint")==0:argc==3&&wcscmp(argv[1],L"--endpoint")==0;
    if(!explicitEndpoint&&!(capabilities&&argc==2)){std::puts("Usage: nc-exclusive-probe --endpoint <endpoint-id> | --capabilities [--endpoint <endpoint-id>]");return 2;}
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator{};IMMDevice* device{};IAudioClient* client{};WAVEFORMATEX* format{};
    HRESULT result=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator));
    if(SUCCEEDED(result))result=explicitEndpoint?enumerator->GetDevice(argv[capabilities?3:2],&device):enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&device);
    if(SUCCEEDED(result))result=device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client));
    if(SUCCEEDED(result))result=client->GetMixFormat(&format);
    if(capabilities){
        LPWSTR endpoint{};if(device)device->GetId(&endpoint);
        char identifier[1024]{};if(endpoint)WideCharToMultiByte(CP_UTF8,0,endpoint,-1,identifier,sizeof(identifier),nullptr,nullptr);
        REFERENCE_TIME normal{},minimum{};HRESULT periodResult=client?client->GetDevicePeriod(&normal,&minimum):result;
        std::printf("{\"queryOnly\":true,\"initialized\":false,\"endpoint\":\"%s\",\"hresult\":%lu,\"periodHresult\":%lu,\"defaultPeriod100ns\":%lld,\"minimumPeriod100ns\":%lld,\"sharedMixRate\":%lu,\"exclusiveFormats\":[",identifier,static_cast<unsigned long>(result),static_cast<unsigned long>(periodResult),normal,minimum,format?format->nSamplesPerSec:0);
        bool comma=false;if(client)for(unsigned rate:{44100u,48000u,88200u,96000u,176400u,192000u})for(const auto& option:nce::WasapiPcmCandidates(rate,2)){
            const auto support=client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&option.Format,nullptr);
            std::printf("%s{\"rate\":%u,\"containerBits\":%u,\"validBits\":%u,\"extensible\":%s,\"supported\":%s,\"hresult\":%lu}",comma?",":"",rate,option.Format.wBitsPerSample,option.Samples.wValidBitsPerSample,option.Format.wFormatTag==WAVE_FORMAT_EXTENSIBLE?"true":"false",support==S_OK?"true":"false",static_cast<unsigned long>(support));comma=true;
        }std::puts("]}");
        if(endpoint)CoTaskMemFree(endpoint);if(format)CoTaskMemFree(format);if(client)client->Release();if(device)device->Release();if(enumerator)enumerator->Release();if(SUCCEEDED(com))CoUninitialize();return SUCCEEDED(result)?0:1;
    }
    if(SUCCEEDED(result))result=client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,0,0,format,nullptr);
    // Never Start or submit audio. This probes only whether Windows permits
    // another shared-mode connection to the specified endpoint.
    std::printf("{\"tick\":%llu,\"sharedInitializeHresult\":%lu,\"hex\":\"0x%08lX\",\"sampleRate\":%lu}\n",
        GetTickCount64(),static_cast<unsigned long>(result),static_cast<unsigned long>(result),format?format->nSamplesPerSec:0);
    if(format)CoTaskMemFree(format);if(client)client->Release();if(device)device->Release();if(enumerator)enumerator->Release();
    if(SUCCEEDED(com))CoUninitialize();return 0;
}
