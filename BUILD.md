# 开发工作区与构建

开发者只需要设备普通用户账号和一台 Linux x86_64 开发主机，不需要主系统源码或 root 调试权限。板端通常没有 C、Go 编译器，构建在开发主机完成。

## 目录

```text
mhcamera-dev/
  README.md                 项目介绍与非官方声明
  BUILD.md                  开发入口
  LICENSE                   本项目 MIT 许可
  scripts/                  环境、SDK 导出、构建与打包
  mhcamera/                 plugin.json、src、www、legal、go2rtc
  sdk/ainice/               板端 SDK 头文件和 libainice.so
  sdk/cjson/                板端 cJSON 库与匹配的上游头文件
  toolchains/               Arm GNU Toolchain 和 Go
  downloads/                第三方源码和工具链下载缓存
  build/                    编译缓存及 runtime
  releases/                 插件包及其 SHA-256 校验文件
  release-support/          分版本保存第三方源码及发布元数据
```

`sdk/` 中除说明文档外的内容，以及 `toolchains/`、`downloads/`、`build/`、`releases/`、`release-support/` 均由 `.gitignore` 排除。不要将包含本地开发依赖的整个工作区直接作为源码压缩包上传；发布包及其配套材料单独作为发布附件提供。

## 准备工具链

主机需要 Bash、Python 3、OpenSSH 客户端、curl、tar、xz、patch、make、binutils 和 coreutils。

从 [Arm GNU Toolchain 官方入口](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) 下载 **13.2.Rel1、Linux x86_64 主机、aarch64-none-linux-gnu 目标**的工具链，保留下载包于 `downloads/`，完整解压到：

```text
toolchains/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu/
```

使用工具链自带的 `aarch64-none-linux-gnu/libc` sysroot，对应 glibc 2.38。不要只复制编译器可执行文件，工具链的库、头文件和许可材料也需要保留。Go 1.24.0 由 go2rtc 构建脚本按固定版本获取，安装到 `toolchains/go1.24.0/`。

在仓库根目录加载环境：

```bash
source scripts/env.sh
```

所有默认路径都相对于当前仓库，不依赖某台开发主机的目录。可以在加载前显式设置 `MHCAMERA_WORKSPACE_ROOT`、`TOOLCHAIN_ROOT`、`AINICE_SDK_ROOT`、`CJSON_ROOT`、`CACHE_ROOT`、`DOWNLOAD_ROOT` 等环境变量以改变依赖位置。

## 从自己的设备导出 SDK

先设置设备地址。密码由 OpenSSH 交互询问，不写入脚本或仓库。

```bash
export BOARD_HOST="你的设备地址"
ssh -tt -p 22 "ainice@$BOARD_HOST"
```

普通账号可以查看 `/DEVELOPER.md`、`/usr/include/ainice/`、`/examples/c-bridge/` 和 `/www/docs/`。SDK 以当前设备实际提供的头文件为准；查看完毕后退出 SSH，回到开发主机执行：

```bash
bash scripts/export_device_sdk.sh
```

导出脚本获取整套 `ainice` 头文件、`libainice.so` 和 `libcjson.so.1.7.19`，并补齐链接名。板端没有 cJSON 开发头文件时，使用校验过摘要的上游 cJSON 1.7.19 源码中的 `cJSON.h`。

脚本默认普通用户名为 `ainice`、端口为 `22`，可通过 `BOARD_USER` 和 `BOARD_PORT` 显式指定。不使用 root 调试端口，不修改板端文件、服务或配置。当前导出方法对应 cJSON 1.7.19；固件改变此依赖版本时，需要同步调整导出文件名、上游版本和摘要，不能混用。

这些文件只是本地交叉编译依赖。能从设备读取不代表已经确认可以公开再分发，SDK 不纳入本项目 MIT 授权，也不进入插件安装包。

## 构建与打包

在仓库根目录执行：

```bash
source scripts/env.sh
bash scripts/build_ffmpeg.sh
bash scripts/build_go2rtc.sh
bash scripts/build_backend.sh
python3 scripts/package_plugin.py
```

依赖固定为 FFmpeg 4.4.4、Go 1.24.0，以及 go2rtc 1.9.14 的提交 `b5948cfb25404cc5cb37b166ecaa2dca20b11d4b`。go2rtc 生产补丁及来源锁定信息在 `mhcamera/go2rtc/`；公开构建不运行测试。

默认 FFmpeg 安装目录为 `build/ffmpeg/install/usr/local/`，插件运行文件放在 `build/runtime/`。`releases/` 保存插件文件及其校验文件，其余发布材料按插件版本保存在 `release-support/`：

```text
releases/
  mhcamera-1.0.5.plugin
  mhcamera-1.0.5.plugin.sha256
release-support/mhcamera-1.0.5/
  sources/ffmpeg-4.4.4.tar.xz
  sources/ffmpeg-4.4.4.tar.xz.sha256
  latest.json               仅在显式提供发布 URL 时生成
```

构建位置可通过 `FFMPEG_ROOT`、`RUNTIME_ROOT` 指定。打包参数 `--output-dir` 指定插件及其校验文件目录，`--support-dir` 指定附件根目录；打包脚本会在附件根目录下建立对应版本的子目录。

在 `releases/` 目录运行 `sha256sum -c mhcamera-1.0.5.plugin.sha256` 可校验插件；FFmpeg 源码校验从 `sources/` 目录运行。校验文件记录同目录的文件名。

发布插件时仍需提供对应的源码和校验材料，可以作为同一 GitHub Release 的附件，或通过发布说明链接到对应下载位置。`release-support/` 是分发配套材料，不是可以随意丢弃的构建缓存。保留包内完整许可材料；用户只需通过设备插件管理器安装 `.plugin`，不需要安装源码包，也不通过 SSH 直接改写设备的插件目录。
