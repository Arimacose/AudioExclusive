# AudioExclusive for macOS

Core Audio 伴随应用，支持 Apple Music 与网易云音乐。要求 macOS 14.2+，支持 Hog Mode 的双声道纯输出设备。

选择应用、设备与采样率后开启独占。使用进程音频捕获、设备独占权和时钟补偿；连续两次检测到空闲后释放设备，恢复活动时重新开启，检测周期为一秒。

首次启动需要系统音频录制权限。包使用本地签名；首次打开时可能需要在“系统设置 → 隐私与安全性”中允许打开。受保护内容的捕获取决于系统与应用策略。

在安装 Xcode Command Line Tools 的 Mac 上执行 `bash build.sh`。脚本构建 arm64 + x86_64 Universal 应用、执行后端自检、验证签名并输出 `build/AudioExclusive-macOS-universal.zip`。

[下载发布包](https://github.com/Arimacose/AudioExclusive/releases/latest)
