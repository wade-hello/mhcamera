# mhcamera 1.0.9 发布附件

`releases/mhcamera-1.0.9.plugin` 是设备插件管理器安装包。此目录保存对应源码附件；每个压缩包旁的 `.sha256` 文件可在 `sources/` 目录执行 `sha256sum -c 文件名.sha256` 校验。

- `mhcamera-1.0.9-source.tar.gz`：本项目当前版本的源码、许可与构建脚本；不含宿主 SDK 和开发机工具链。
- `go2rtc-b5948cfb25404cc5cb37b166ecaa2dca20b11d4b.tar.gz`：锁定的 go2rtc 上游源码；本项目的补丁和来源锁定文件在项目源码归档的 `mhcamera/go2rtc/` 下。
- `go-modules-1.0.9.tar.gz`：此次构建使用的 Go 模块源码缓存，包括模块下载校验数据；在项目源码目录执行 `mkdir -p build/go2rtc && tar -xzf 此文件 -C build/go2rtc` 可恢复为 `build/go2rtc/gomodcache/`。
- `ffmpeg-4.4.4.tar.xz`：插件私有 FFmpeg 库的上游源码。FFmpeg 的 LGPL 许可与构建说明也保存在插件包中。

完整交叉编译方法见项目源码归档中的 `BUILD.md`。构建仍需开发机的 Arm GNU 工具链和从自己的设备导出的宿主 SDK；这些内容不在插件包或源码附件中。go2rtc 构建脚本会校验所用上游压缩包、固定版本 Go 工具链和每个补丁的 SHA-256。
