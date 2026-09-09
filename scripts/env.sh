#!/usr/bin/env bash
# Source this file from Bash to configure the standalone development workspace.
_mhcamera_repo=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export MHCAMERA_WORKSPACE_ROOT="${MHCAMERA_WORKSPACE_ROOT:-$_mhcamera_repo}"
export TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-$MHCAMERA_WORKSPACE_ROOT/toolchains/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu}"
export CROSS_COMPILE="${CROSS_COMPILE:-$TOOLCHAIN_ROOT/bin/aarch64-none-linux-gnu-}"
export CC="${CC:-${CROSS_COMPILE}gcc}"
export STRIP="${STRIP:-${CROSS_COMPILE}strip}"
export READELF="${READELF:-${CROSS_COMPILE}readelf}"
export SYSROOT="${SYSROOT:-$TOOLCHAIN_ROOT/aarch64-none-linux-gnu/libc}"
export AINICE_SDK_ROOT="${AINICE_SDK_ROOT:-$MHCAMERA_WORKSPACE_ROOT/sdk/ainice}"
export CJSON_ROOT="${CJSON_ROOT:-$MHCAMERA_WORKSPACE_ROOT/sdk/cjson}"
export CACHE_ROOT="${CACHE_ROOT:-$MHCAMERA_WORKSPACE_ROOT/build}"
export DOWNLOAD_ROOT="${DOWNLOAD_ROOT:-$MHCAMERA_WORKSPACE_ROOT/downloads}"
export FFMPEG_ROOT="${FFMPEG_ROOT:-$CACHE_ROOT/ffmpeg/install/usr/local}"
export RUNTIME_ROOT="${RUNTIME_ROOT:-$CACHE_ROOT/runtime}"
export RELEASE_DIR="${RELEASE_DIR:-$MHCAMERA_WORKSPACE_ROOT/releases}"
export GO_ROOT="${GO_ROOT:-$MHCAMERA_WORKSPACE_ROOT/toolchains/go1.24.0}"
unset _mhcamera_repo
