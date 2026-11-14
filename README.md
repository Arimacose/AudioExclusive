# AudioExclusive

一款针对网易云音乐和Apple Music的开启独占模式绕过Windows SRC插件

[下载最新版本](https://github.com/Arimacose/AudioExclusive/releases/latest) · [发布记录](https://github.com/Arimacose/AudioExclusive/releases)

## 最新版本

**2026.11.14.2**

适配Apple Music到1.7.0.156 (1.1792.39942.0)，适配网易云音乐到3.1.41版本

- 修复 Apple Music 相邻歌曲采样率不同时下一首无声；源数据等待期间保留输出运行状态，按实际设备时钟完成尾包后切换歌曲。
- Apple Music 暂停时释放独占设备，恢复时续播已提交但尚未播放的 PCM。
- 修复转换器重复使用时的格式竞态、音频统计并发读取、网易云暂停历史不足和设置写入阻塞音频提交的问题。
- 修复 ASIO 初始化失败后的驱动占用、设置页动画回调保留，以及分别升级两个应用后的启动包选择。
- macOS 加强通道帧数与格式布局检查，数字静音不再触发捕获故障判定。

本次完整检查记录及测试入口见 [业务逻辑审计](AUDIT-2026-11-14.md)。

## 使用

1. 下载并运行 `AudioExclusive-Setup.exe`，菜单与操作过程显示在 PowerShell 中。
2. 选择自动检测并安装。安装器分别查找网易云音乐和 Apple Music；某个应用未安装时跳过该应用并继续。
3. 从菜单或开始菜单中的“网易云音乐（独占插件）”/“Apple Music（独占插件）”启动。
4. 网易云的开关位于“设置 → 播放 → 系统音频增强”下方。Apple Music 在设置中提供独占选项。
5. 需要取消时，重新打开管理器，选择取消对应应用的独占插件。

Windows 版本面向 x64。网易云支持 WASAPI 独占输出与已安装的 ASIO 驱动；ASIO 需要设备厂商提供的驱动。暂停后释放设备，供其他应用使用。

## 支持的 Windows 客户端

| 应用 | 版本 |
| --- | --- |
| Apple Music | 1.7.0.156（软件包 1.1792.39942.0） |
| Apple Music | 1.7.0.155（软件包 1.1792.39682.0） |
| Apple Music | 1.6.4.90（软件包 1.1540.23042.0） |
| 网易云音乐 | 3.1.41.205564、3.1.36.205322 |

遇到不支持的版本时，安装器列出当前探测到的版本、已支持版本及跳过原因，继续处理其他应用。

## 源码与构建

- `backend-source/apple-source`：Apple Music 音频后端、设置界面与伴随进程。
- `backend-source/netease-source`：网易云音频后端、ASIO/WASAPI 输出、设置页面注入与启动器。
- `backend-source/macos-source`：macOS Core Audio 伴随应用。
- `backend-source/tests`：音频格式、设备策略、输出时钟、ASIO/WASAPI 和页面生命周期测试。
- `installer`：PowerShell 管理器与单文件安装器源码。

Windows 构建需要 Visual Studio 2022 C++ Build Tools、Windows SDK、CMake 3.24+、Python 3 和已安装的支持版 Apple Music。在 PowerShell 中执行：

```powershell
.\Build-Windows.ps1 -AppleMusicPackageDirectory 'C:\Program Files\WindowsApps\AppleInc.AppleMusicWin_1.1792.39942.0_x64__nzyj5cx40ttqa'
```

安装器输出到 `build/setup/Release/AudioExclusive-Setup.exe`。构建过程通过 C++/WinRT 从本机 Apple Music 包生成 WinUI 投影；仓库不包含音乐客户端或其框架二进制文件。

macOS 源码与构建说明见 [macOS README](backend-source/macos-source/README.md)。macOS 使用 Core Audio Hog Mode 和进程音频捕获，要求 macOS 14.2+，与 Windows 注入插件采用不同的实现。

## 开源许可

仓库新增代码按 GPL-3.0 发布；Apple Music 原有代码保留 MIT 许可，第三方组件保留各自许可。详见 [LICENSE](LICENSE) 和 [第三方说明](THIRD_PARTY_NOTICES.md)。
