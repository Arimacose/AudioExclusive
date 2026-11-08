// SPDX-License-Identifier: MIT
// Public Core Audio process taps; no injection into signed applications.
#import <Cocoa/Cocoa.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <vector>
#include <cmath>
#include <cstring>
#include <unistd.h>

static AudioObjectPropertyAddress Address(AudioObjectPropertySelector s,AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    return {s,scope,kAudioObjectPropertyElementMain};
}
template<class T> static OSStatus Read(AudioObjectID id,AudioObjectPropertySelector s,T& value,AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    auto a=Address(s,scope);UInt32 size=sizeof(T);return AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&value);
}
template<class T> static OSStatus Write(AudioObjectID id,AudioObjectPropertySelector s,const T& value) {
    auto a=Address(s);return AudioObjectSetPropertyData(id,&a,0,nullptr,sizeof(T),&value);
}
static std::vector<AudioObjectID> Objects(AudioObjectID id,AudioObjectPropertySelector selector,AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    auto a=Address(selector,scope);UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&size)!=noErr)return {};
    std::vector<AudioObjectID> objects(size/sizeof(AudioObjectID));
    if(size&&AudioObjectGetPropertyData(id,&a,0,nullptr,&size,objects.data())!=noErr)return {};
    return objects;
}
static NSString* Text(AudioObjectID id,AudioObjectPropertySelector selector) {
    CFStringRef value=nullptr;if(Read(id,selector,value)!=noErr||!value)return @"";
    return CFBridgingRelease(value);
}
enum class PlaybackActivity {Running,Idle,Unknown};
static PlaybackActivity Activity(NSString* bundle) {
    bool failed=false;
    for(auto process:Objects(kAudioObjectSystemObject,kAudioHardwarePropertyProcessObjectList)){
        NSString* identity=Text(process,kAudioProcessPropertyBundleID);
        if(![identity isEqualToString:bundle]&&![identity hasPrefix:[bundle stringByAppendingString:@"."]])continue;
        UInt32 running=0;
        if(Read(process,kAudioProcessPropertyIsRunningOutput,running)!=noErr)failed=true;
        else if(running)return PlaybackActivity::Running;
    }
    return failed?PlaybackActivity::Unknown:PlaybackActivity::Idle;
}
struct PauseDebounce {
    unsigned idleReads{};
    bool Update(PlaybackActivity activity) {
        if(activity!=PlaybackActivity::Idle){idleReads=0;return false;}
        return ++idleReads>=2;
    }
    void Reset(){idleReads=0;}
};
static UInt32 Channels(AudioObjectID id,AudioObjectPropertyScope scope) {
    auto a=Address(kAudioDevicePropertyStreamConfiguration,scope);UInt32 size{};
    if(AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&size)!=noErr||size<sizeof(AudioBufferList))return 0;
    std::vector<unsigned char> bytes(size);auto* list=reinterpret_cast<AudioBufferList*>(bytes.data());
    if(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,list)!=noErr)return 0;
    UInt32 total=0;for(UInt32 i=0;i<list->mNumberBuffers;++i)total+=list->mBuffers[i].mNumberChannels;return total;
}
static bool FloatStereo(const AudioStreamBasicDescription& f,double rate) {
    return f.mFormatID==kAudioFormatLinearPCM&&(f.mFormatFlags&kAudioFormatFlagIsFloat)&&
        !(f.mFormatFlags&kAudioFormatFlagIsBigEndian)&&f.mBitsPerChannel==32&&
        f.mChannelsPerFrame==2&&std::abs(f.mSampleRate-rate)<0.5;
}
// Handles both interleaved and planar HAL buffers without allocating in IO.
static bool CopyStereo(const AudioBufferList* input,AudioBufferList* output,std::uint64_t& count,bool& nonzero) {
    if(!input||!output)return false;
    unsigned inChannels=0,outChannels=0;UInt32 frames=UINT32_MAX;
    for(UInt32 i=0;i<input->mNumberBuffers;++i){const auto& b=input->mBuffers[i];if(!b.mData||!b.mNumberChannels)return false;inChannels+=b.mNumberChannels;frames=std::min(frames,b.mDataByteSize/(4*b.mNumberChannels));}
    for(UInt32 i=0;i<output->mNumberBuffers;++i){const auto& b=output->mBuffers[i];if(!b.mData||!b.mNumberChannels)return false;outChannels+=b.mNumberChannels;if(b.mDataByteSize/(4*b.mNumberChannels)!=frames)return false;}
    if(inChannels!=2||outChannels!=2)return false;
    for(UInt32 f=0;f<frames;++f){float stereo[2]{};unsigned c=0;
        for(UInt32 i=0;i<input->mNumberBuffers;++i){const auto& b=input->mBuffers[i];const auto* p=static_cast<const float*>(b.mData);for(UInt32 ch=0;ch<b.mNumberChannels;++ch){float value=p[f*b.mNumberChannels+ch];if(!std::isfinite(value))return false;stereo[c++]=value;nonzero|=value!=0;}}
        c=0;for(UInt32 i=0;i<output->mNumberBuffers;++i){const auto& b=output->mBuffers[i];auto* p=static_cast<float*>(b.mData);for(UInt32 ch=0;ch<b.mNumberChannels;++ch)p[f*b.mNumberChannels+ch]=stereo[c++];}
    }count+=frames;return true;
}
struct Engine {
    AudioObjectID physical{},tap{},aggregate{};AudioDeviceIOProcID proc{};
    double oldRate{},chosenRate{};bool changedRate{},hogged{},started{};
    std::atomic<std::uint64_t> frames{},signalPackets{};std::atomic<bool> fault{};
    ~Engine(){Stop();}
    static OSStatus IO(AudioDeviceID,const AudioTimeStamp*,const AudioBufferList* input,const AudioTimeStamp*,AudioBufferList* output,const AudioTimeStamp*,void* context) {
        auto* self=static_cast<Engine*>(context);std::uint64_t count=0;bool signal=false;
        if(self->fault.load()||!CopyStereo(input,output,count,signal)) {
            if(output)for(UInt32 i=0;i<output->mNumberBuffers;++i)if(output->mBuffers[i].mData)memset(output->mBuffers[i].mData,0,output->mBuffers[i].mDataByteSize);
            self->fault.store(true);return noErr;
        }
        self->frames.fetch_add(count,std::memory_order_relaxed);if(signal)self->signalPackets.fetch_add(1,std::memory_order_relaxed);return noErr;
    }
    void Stop() {
        if(started){AudioDeviceStop(aggregate,proc);started=false;}
        if(proc){AudioDeviceDestroyIOProcID(aggregate,proc);proc=nullptr;}
        if(aggregate){AudioHardwareDestroyAggregateDevice(aggregate);aggregate=0;}
        if(tap){AudioHardwareDestroyProcessTap(tap);tap=0;} // Restores target application's ordinary output.
        if(physical){pid_t owner=-1;if(Read(physical,kAudioDevicePropertyHogMode,owner)==noErr&&owner==getpid()){
            double now=0;if(changedRate&&Read(physical,kAudioDevicePropertyNominalSampleRate,now)==noErr&&std::abs(now-chosenRate)<0.5)Write(physical,kAudioDevicePropertyNominalSampleRate,oldRate);
            pid_t release=-1;Write(physical,kAudioDevicePropertyHogMode,release);
        }}physical=0;hogged=false;changedRate=false;
    }
    NSString* Start(NSString* bundle,AudioObjectID device,double rate) {
        Stop();frames=0;signalPackets=0;fault=false;physical=device;
        auto fail=[&](NSString* reason){Stop();return reason;};
        if(!physical||Channels(physical,kAudioDevicePropertyScopeOutput)!=2)return fail(@"当前设备不是双声道输出设备。请选择双声道 DAC。");
        // A subdevice with capture channels changes aggregate input ordering.
        // Refuse it until the channel map is verified; never route microphone audio.
        if(Channels(physical,kAudioDevicePropertyScopeInput)!=0)return fail(@"此构建仅适配无输入通道的双声道 DAC；带麦克风/录音输入的设备暂不支持。");
        NSMutableArray<NSNumber*>* processes=[NSMutableArray array];
        for(auto process:Objects(kAudioObjectSystemObject,kAudioHardwarePropertyProcessObjectList)){
            NSString* identity=Text(process,kAudioProcessPropertyBundleID);
            if([identity isEqualToString:bundle]||[identity hasPrefix:[bundle stringByAppendingString:@"."]])[processes addObject:@(process)];
        }
        if(processes.count==0)return fail(@"没有找到目标应用的音频进程。请打开该应用并开始播放后重试。");
        pid_t owner=-1;OSStatus status=Read(physical,kAudioDevicePropertyHogMode,owner);
        if(status!=noErr)return fail([NSString stringWithFormat:@"设备不提供独占控制（Hog Mode），OSStatus=%d。",status]);
        if(owner!=-1&&owner!=getpid())return fail(@"设备已被其他进程独占。");
        owner=getpid();status=Write(physical,kAudioDevicePropertyHogMode,owner);pid_t confirmed=-1;
        if(status!=noErr||Read(physical,kAudioDevicePropertyHogMode,confirmed)!=noErr||confirmed!=getpid())return fail(@"设备拒绝独占控制，未回退为共享模式。");
        hogged=true;chosenRate=rate;
        if(Read(physical,kAudioDevicePropertyNominalSampleRate,oldRate)!=noErr)return fail(@"无法读取设备采样率。");
        if(std::abs(oldRate-rate)>0.5){if(Write(physical,kAudioDevicePropertyNominalSampleRate,rate)!=noErr)return fail(@"设备不支持所选采样率。");changedRate=true;}
        NSString* uid=Text(physical,kAudioDevicePropertyDeviceUID);
        CATapDescription* description=[[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
        description.name=@"AudioExclusive application tap";[description setPrivate:YES];description.muteBehavior=CATapMutedWhenTapped;
        status=AudioHardwareCreateProcessTap(description,&tap);
        if(status!=noErr)return fail([NSString stringWithFormat:@"创建音频捕获失败，OSStatus=%d。请检查“系统设置 → 隐私与安全性 → 屏幕与系统音频录制”权限。",status]);
        AudioStreamBasicDescription tapFormat{};
        if(Read(tap,kAudioTapPropertyFormat,tapFormat)!=noErr||!FloatStereo(tapFormat,rate))return fail(@"应用捕获采样率或格式与所选设备不一致。请选择匹配采样率；此构建不会静默重采样。");
        NSDictionary* composition=@{@kAudioAggregateDeviceNameKey:@"AudioExclusive private output",@kAudioAggregateDeviceUIDKey:NSUUID.UUID.UUIDString,
            @kAudioAggregateDeviceIsPrivateKey:@YES,@kAudioAggregateDeviceIsStackedKey:@NO,
            @kAudioAggregateDeviceSubDeviceListKey:@[@{@kAudioSubDeviceUIDKey:uid}],@kAudioAggregateDeviceMainSubDeviceKey:uid,
            @kAudioAggregateDeviceTapAutoStartKey:@YES,@kAudioAggregateDeviceTapListKey:@[@{@kAudioSubTapUIDKey:description.UUID.UUIDString,@kAudioSubTapDriftCompensationKey:@YES}]};
        status=AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)composition,&aggregate);
        if(status!=noErr)return fail([NSString stringWithFormat:@"建立私有音频设备失败，OSStatus=%d。",status]);
        if(Channels(aggregate,kAudioDevicePropertyScopeInput)!=2||Channels(aggregate,kAudioDevicePropertyScopeOutput)!=2)return fail(@"聚合设备通道映射与双声道合同不一致，已停止。");
        for(auto scope:{kAudioDevicePropertyScopeInput,kAudioDevicePropertyScopeOutput}){
            auto streams=Objects(aggregate,kAudioDevicePropertyStreams,scope);if(streams.size()!=1)return fail(@"此设备的 HAL 音频流布局尚不支持。");
            AudioStreamBasicDescription format{};if(Read(streams[0],kAudioStreamPropertyVirtualFormat,format)!=noErr||!FloatStereo(format,rate))return fail(@"设备的 HAL 音频格式不匹配，已停止。");
        }
        status=AudioDeviceCreateIOProcID(aggregate,IO,this,&proc);if(status==noErr)status=AudioDeviceStart(aggregate,proc);
        if(status!=noErr)return fail([NSString stringWithFormat:@"启动输出失败，OSStatus=%d。",status]);
        started=true;return nil;
    }
    bool Healthy(){pid_t owner=-1;UInt32 alive=0;double rate=0;AudioStreamBasicDescription format{};return !fault.load()&&Read(physical,kAudioDevicePropertyHogMode,owner)==noErr&&owner==getpid()&&Read(physical,kAudioDevicePropertyDeviceIsAlive,alive)==noErr&&alive&&Read(physical,kAudioDevicePropertyNominalSampleRate,rate)==noErr&&std::abs(rate-chosenRate)<0.5&&Read(tap,kAudioTapPropertyFormat,format)==noErr&&FloatStereo(format,chosenRate);}
};

@interface Controller:NSObject<NSApplicationDelegate>
@end
@implementation Controller {
    NSWindow* window;NSTextField* status;NSPopUpButton* devices;NSPopUpButton* rates;Engine engine;NSTimer* timer;NSDate* began;NSString* target;
    bool armed;AudioObjectID selectedDevice;double selectedRate;PauseDebounce pause;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,670,365) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    window.title=@"AudioExclusive 2026.11.08.1 — 外部验收版";[window center];
    NSView* content=window.contentView;
    NSTextField* intro=[NSTextField wrappingLabelWithString:@"Apple Music / 网易云音乐独占输出\n选择设备并开启。检测到应用暂停后释放设备，恢复播放时重新开启；关闭此窗口会恢复普通输出。"];
    intro.frame=NSMakeRect(24,280,620,65);[content addSubview:intro];
    devices=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(24,234,440,30) pullsDown:NO];
    for(auto id:Objects(kAudioObjectSystemObject,kAudioHardwarePropertyDevices))if(Channels(id,kAudioDevicePropertyScopeOutput)>0){[devices addItemWithTitle:Text(id,kAudioObjectPropertyName)];devices.lastItem.representedObject=@(id);}
    [content addSubview:devices];rates=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(480,234,160,30) pullsDown:NO];[rates addItemsWithTitles:@[@"44100 Hz",@"48000 Hz",@"96000 Hz",@"192000 Hz"]];[content addSubview:rates];
    NSArray* titles=@[@"开启 Apple Music 独占",@"开启网易云独占",@"关闭独占"];
    SEL actions[]={@selector(apple:),@selector(netease:),@selector(disable:)};
    for(int i=0;i<3;++i){NSButton* button=[NSButton buttonWithTitle:titles[i] target:self action:actions[i]];button.frame=NSMakeRect(24+i*213,180,200,34);[content addSubview:button];}
    status=[NSTextField wrappingLabelWithString:@"尚未开启。系统音频权限由 macOS 提示授权。此版要求 macOS 14.2+、支持 Hog Mode 的双声道纯输出 DAC。受保护内容能否被系统捕获须由外部用户验证。"];
    status.frame=NSMakeRect(24,30,620,130);[content addSubview:status];
    [window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
    timer=[NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}
- (void)start:(NSString*)bundle {
    engine.Stop();target=bundle;selectedDevice=[devices.selectedItem.representedObject unsignedIntValue];selectedRate=[rates.titleOfSelectedItem doubleValue];
    armed=true;pause.Reset();devices.enabled=NO;rates.enabled=NO;
    if([NSRunningApplication runningApplicationsWithBundleIdentifier:target].count==0){[self disable:nil];status.stringValue=@"请先打开目标音乐应用。";return;}
    const auto activity=Activity(target);
    if(activity==PlaybackActivity::Unknown){[self disable:nil];status.stringValue=@"无法读取应用音频输出状态，已释放设备。请重新开启后重试。";return;}
    if(activity==PlaybackActivity::Idle){status.stringValue=@"等待应用播放，当前设备已释放。";return;}
    NSString* problem=engine.Start(target,selectedDevice,selectedRate);began=NSDate.date;
    if(problem){[self disable:nil];status.stringValue=problem;return;}
    status.stringValue=@"设备独占权已确认，等待目标应用的音频数据。";
}
- (void)apple:(id)sender{(void)sender;[self start:@"com.apple.Music"];}
- (void)netease:(id)sender{(void)sender;[self start:@"com.netease.163music"];}
- (void)disable:(id)sender{(void)sender;armed=false;pause.Reset();engine.Stop();devices.enabled=YES;rates.enabled=YES;status.stringValue=@"独占已关闭，音频捕获和设备占用已释放。";}
- (void)tick:(NSTimer*)sender{
    (void)sender;if(!armed)return;
    if([NSRunningApplication runningApplicationsWithBundleIdentifier:target].count==0){[self disable:nil];status.stringValue=@"应用已退出，独占已停止并释放。";return;}
    const auto activity=Activity(target);
    if(activity==PlaybackActivity::Unknown){[self disable:nil];status.stringValue=@"无法读取应用音频输出状态，已停止并释放独占。";return;}
    if(!engine.started){
        if(activity!=PlaybackActivity::Running)return;
        NSString* problem=engine.Start(target,selectedDevice,selectedRate);began=NSDate.date;pause.Reset();
        if(problem){[self disable:nil];status.stringValue=problem;}else status.stringValue=@"播放已恢复，设备独占权已重新确认。";
        return;
    }
    if(pause.Update(activity)){engine.Stop();pause.Reset();status.stringValue=@"检测到应用暂停，设备已释放；恢复播放时自动重新开启独占。";return;}
    if(!engine.Healthy()){[self disable:nil];status.stringValue=@"设备断开、独占权丢失或音频格式改变，已停止并释放独占。";return;}
    if([began timeIntervalSinceNow]<-10&&engine.signalPackets.load()==0){[self disable:nil];status.stringValue=@"10 秒内没有捕获到非静音音频，已释放独占。请检查播放、系统授权或内容捕获限制。";return;}
    status.stringValue=[NSString stringWithFormat:@"%@\n设备独占权：已确认\n输出帧：%llu；非静音包：%llu\n使用系统进程音频捕获和时钟补偿，不承诺 bit-perfect；听感及受保护内容待验收。",target,(unsigned long long)engine.frames.load(),(unsigned long long)engine.signalPackets.load()];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app{(void)app;return YES;}
- (void)applicationWillTerminate:(NSNotification*)notification{(void)notification;[timer invalidate];engine.Stop();}
@end
int main(int argc,const char* argv[]){@autoreleasepool{
    if(argc>1&&!strcmp(argv[1],"--self-test")){
        float src[]={0.5f,-0.5f,1.0f,-1.0f},dst[4]{};AudioBufferList in{1,{{2,sizeof(src),src}}},out{1,{{2,sizeof(dst),dst}}};std::uint64_t frames=0;bool signal=false;
        if(!CopyStereo(&in,&out,frames,signal)||frames!=2||!signal||memcmp(src,dst,sizeof(src)))return 1;
        out.mBuffers[0].mDataByteSize=4;if(CopyStereo(&in,&out,frames,signal))return 2;
        PauseDebounce pause;
        if(pause.Update(PlaybackActivity::Idle)||!pause.Update(PlaybackActivity::Idle))return 3;
        if(pause.Update(PlaybackActivity::Running)||pause.Update(PlaybackActivity::Idle))return 4;
        if(pause.Update(PlaybackActivity::Unknown)||pause.Update(PlaybackActivity::Idle))return 5;
        pause.Reset();if(pause.Update(PlaybackActivity::Idle))return 6;
        puts("PASS HAL stereo copy, buffer mismatch rejection, pause debounce and resume/reset activity transitions");return 0;
    }
    [NSApplication sharedApplication];NSApp.activationPolicy=NSApplicationActivationPolicyRegular;Controller* delegate=[Controller new];NSApp.delegate=delegate;[NSApp run];
}return 0;}
