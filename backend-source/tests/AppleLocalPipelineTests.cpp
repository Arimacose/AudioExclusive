// Exercise the production hook functions against Apple's installed converter
// in an isolated test process. DllMain is never called and no hooks are injected.
#include "../apple-source/mod/src/HookDll.cpp"
#include "audio_v2/LocalFloatIntegerizer.h"
#include <iostream>
static void Require(bool pass,const char* why){if(!pass)throw std::runtime_error(why);}
int wmain(int argc,wchar_t** argv){try{
    Require(argc==3,"pass CoreAudioToolbox.dll and decoded 24-bit fixture paths");
    auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module!=nullptr,"load installed CoreAudioToolbox");
    g_v2OriginalAudioConverterNew=reinterpret_cast<AudioConverterNewFn>(GetProcAddress(module,"AudioConverterNew"));
    g_v2OriginalAudioConverterConvertBuffer=reinterpret_cast<AudioConverterConvertBufferFn>(GetProcAddress(module,"AudioConverterConvertBuffer"));
    auto dispose=reinterpret_cast<AudioConverterDisposeFn>(GetProcAddress(module,"AudioConverterDispose"));
    Require(g_v2OriginalAudioConverterNew&&g_v2OriginalAudioConverterConvertBuffer&&dispose,"converter exports");
    g_audioCoreRuntime.SetUiEnabled(true);
    AudioStreamBasicDescription source{48000,0x6c70636d,12,6,1,6,2,24,0};
    AudioStreamBasicDescription destination{48000,0x6c70636d,9,8,1,8,2,32,0};
    void* converter{};Require(HookV2AudioConverterNew(&source,&destination,&converter)==0&&converter,"create local PCM converter");
    auto queue=FindV2LocalQueue(converter);Require(queue!=nullptr,"register local staging queue");
    void* floatConverter{};Require(HookV2AudioConverterNew(&destination,&destination,&floatConverter)==0&&floatConverter,"create local float converter");
    auto floatQueue=FindV2LocalQueue(floatConverter);Require(floatQueue!=nullptr,"register local float queue");
    std::ifstream file(std::filesystem::path(argv[2]),std::ios::binary);Require(file.good(),"open test fixture");
    std::array<BYTE,4096*6> input{},captured{},packed{};std::array<float,4096*2> output{},canonical{};std::array<std::int32_t,4096*2> integer{};
    std::uint64_t total=0;
    while(file){file.read(reinterpret_cast<char*>(input.data()),input.size());auto bytes=static_cast<std::uint32_t>(file.gcount());if(!bytes)break;Require(bytes%6==0,"fixture alignment");auto frames=bytes/6;std::uint32_t outputBytes=frames*8;
        auto result=HookV2AudioConverterConvertBuffer(converter,bytes,input.data(),&outputBytes,output.data());
        Require(result==0&&outputBytes==frames*8,"native local conversion");
        Require(queue->Pop(captured.data(),frames),"local push hook lost frames");Require(memcmp(input.data(),captured.data(),bytes)==0,"source bytes changed");
        Require(ConvertV2LocalToCanonical(*queue,captured.data(),frames,canonical.data()),"canonical mapping");
        ammod::audio_v2::LocalFloatIntegerizerStats stats{};
        Require(ammod::audio_v2::IntegerizeLocalFloatInterleaved(canonical.data(),integer.data(),frames*2,stats),"float integerizer");
        for(unsigned i=0;i<frames*2;++i)integer[i]=ammod::audio_v2::QuantizeCanonical(integer[i],24);
        Require(ammod::audio_v2::PackCanonicalPcm32(integer.data(),packed.data(),frames*2,24),"DAC pack");Require(memcmp(input.data(),packed.data(),bytes)==0,"24-bit source-to-DAC mismatch");
        std::uint32_t floatBytes=frames*8;
        Require(HookV2AudioConverterConvertBuffer(floatConverter,frames*8,output.data(),&floatBytes,canonical.data())==0,"native float local conversion");
        Require(floatQueue->Pop(canonical.data(),frames),"float queue lost frames");
        Require(ammod::audio_v2::IntegerizeLocalFloatInterleaved(canonical.data(),integer.data(),frames*2,stats),"local float mapping");
        for(unsigned i=0;i<frames*2;++i)integer[i]=ammod::audio_v2::QuantizeCanonical(integer[i],24);
        Require(ammod::audio_v2::PackCanonicalPcm32(integer.data(),packed.data(),frames*2,24),"float DAC pack");Require(memcmp(input.data(),packed.data(),bytes)==0,"local float-to-packed24 mismatch");
        total+=frames;
    }
    Require(total>48000&&queue->capturedFrames==total&&queue->sizeBytes==0&&queue->droppedFrames==0,"frame accounting");
    std::cout<<"PASS installed Apple converter + production local push hook: "<<total<<" stereo frames, zero drops, exact 24-bit source-to-DAC mapping\n";
    Require(floatQueue->capturedFrames==total&&floatQueue->droppedFrames==0&&floatQueue->sizeBytes==0,"float frame accounting");
    std::cout<<"PASS same complete fixture through native float local capture and packed 24-bit DAC fallback\n";
    dispose(floatConverter);dispose(converter);g_audioCoreRuntime.SetUiEnabled(false);return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" Win32="<<GetLastError()<<'\n';return 1;}}
