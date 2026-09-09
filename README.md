# mhcamera

主要基于 [go2rtc](https://github.com/AlexxIT/go2rtc) 开发的非官方 AInice Vision 米家摄像头插件，当前版本为 **v1.0.5**。

## 功能

将米家摄像头视频接入 AInice Vision 的 AI 视频链路，并提供独立的 RTSP 输出。

在 AInice Vision 的插件管理页面添加发布的 `.plugin` 文件即可安装使用。

支持的米家摄像头型号可参考 [go2rtc 的兼容性说明](https://github.com/AlexxIT/go2rtc/blob/master/internal/xiaomi/README.md)，具体兼容性以本插件实际使用情况为准。

## 说明

本项目出于个人兴趣与学习目的开发，主要借助 Codex 编写和修改，可能存在尚未发现的功能、安全或兼容性问题。不承诺长期维护、持续更新、问题修复或技术支持。

本项目为非官方项目，与小米、米家、AInice 及 go2rtc、FFmpeg 的维护者无隶属、合作或官方背书关系。相关名称仅用于说明依赖项目或适配产品。

本项目主要基于 go2rtc，并使用 FFmpeg 等第三方组件。相关组件及服务适用各自的许可、使用条款和知识产权要求，本项目的 MIT 许可不替代相关第三方授权。部分功能可能因第三方项目或服务调整而变化。

请仅在自己拥有或获准使用的设备、账号和网络中使用，并自行评估隐私、安全及合规风险。本项目按“现状”提供，不作任何明示或默示保证。具体保证排除与责任限制见 [LICENSE](LICENSE)，依法不能排除或限制的责任除外。

## 源码与构建

本项目自身代码采用 [MIT](LICENSE)；第三方组件及宿主 SDK 的许可独立，见[第三方说明](mhcamera/legal/README.md)。

本仓库根目录是完整开发工作区，插件源码位于 `mhcamera/`。环境准备、板端 SDK 导出和构建方法见 [BUILD.md](BUILD.md)。SDK、工具链、下载缓存及构建产物不随源码提交。
