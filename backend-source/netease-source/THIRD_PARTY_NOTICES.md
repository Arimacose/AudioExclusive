# 第三方来源与许可

- MinHook 1.3.3：Tsuda Kageyu，BSD 2-Clause；源码取自本工作区已有的 AMExclusive 构建依赖，版本由其 CMakeLists 声明确认。原许可保留在 `source/third_party/minhook/LICENSE.txt`，另收录于 `licenses/MinHook.txt`。
- CEF：Chromium Embedded Framework，BSD 风格许可。头文件固定来自上游提交 `04c8d5653ca5b3e36b693d871cce1ef09617bfc9`，Chromium 4472 分支。来源及下载 SHA256 记录在 `source/third_party/dependency-lock.json`。许可保留在 `source/third_party/cef/LICENSE.txt` 及 `licenses/CEF.txt`。
- Chromium：用于 CEF 枚举定义的 `net/base/net_error_list.h` 来自 Chromium `91.0.4472.169`，来源及 SHA256 记录在 `source/third_party/chromium-net-errors-lock.json`。Chromium 许可收录于 `licenses/Chromium.txt`。
- AMExclusive：原有项目的运行时插件、明确状态反馈与版本约束设计是本移植的上下文；本项目没有复制其 Apple 私有图偏移和音频 tap。原项目 MIT 许可收录于 `licenses/AMExclusive.txt` 以保留来源信息。

该交付包不包含网易云音乐或 Apple Music 的程序文件、解码器或歌曲，只记录本机支持版本的文件指纹和静态兼容性参数。


ASIO support added 2026-10-15. The NCExclusive plugin including this backend is distributed under GPL-3.0-only, selecting the GPL v3 option of the Steinberg ASIO SDK dual license. SDK headers: https://github.com/audiosdk/asio . Corresponding source, build scripts and all included dependency source are in the adjacent backend-source directory and source archive. See licenses/GPL-3.0.txt and licenses/ASIO-SDK-LICENSE.txt. Other applications and installer components retain their own licenses.
