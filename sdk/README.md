# 本地设备 SDK

此目录保存从自己的设备普通账号导出的开发依赖，不是插件自身源码，也不作为默认公开内容。

```text
sdk/
  ainice/usr/include/ainice/     设备提供的完整头文件目录
  ainice/usr/lib/libainice.so    设备提供的动态库
  cjson/include/cjson/cJSON.h    匹配版本的上游头文件
  cjson/lib/                    设备 cJSON 动态库及链接名
```

当前开发基线为 AArch64、glibc 2.38、cJSON 1.7.19。获取方式见根目录 [BUILD.md](../BUILD.md)，更新 SDK 时应从同一设备固件获取配套头文件与库。

宿主 SDK 与本项目 MIT 许可分开处理。公开再分发前需另行确认授权；不要删除原有版权或许可声明。SDK 不打入插件安装包，运行时使用设备自身提供的库。

本目录默认只提交这份说明，不提交导出的文件、账号信息或设备凭据。
