#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
PLUGIN_ROOT="$ROOT/mhcamera"
WORKSPACE_ROOT=$(realpath -m "${MHCAMERA_WORKSPACE_ROOT:-"$ROOT"}")
CACHE_ROOT=${CACHE_ROOT:-"$WORKSPACE_ROOT/build"}
RUNTIME_ROOT=${RUNTIME_ROOT:-"$CACHE_ROOT/runtime"}
FFMPEG_ROOT=${FFMPEG_ROOT:-"$CACHE_ROOT/ffmpeg/install/usr/local"}
AINICE_SDK_ROOT=${AINICE_SDK_ROOT:-"$WORKSPACE_ROOT/sdk/ainice"}
CJSON_ROOT=${CJSON_ROOT:-"$WORKSPACE_ROOT/sdk/cjson"}
TOOLCHAIN_ROOT=${TOOLCHAIN_ROOT:-"$WORKSPACE_ROOT/toolchains/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu"}
CROSS_COMPILE=${CROSS_COMPILE:-"$TOOLCHAIN_ROOT/bin/aarch64-none-linux-gnu-"}
SYSROOT=${SYSROOT:-"$TOOLCHAIN_ROOT/aarch64-none-linux-gnu/libc"}
CC=${CC:-"${CROSS_COMPILE}gcc"}
STRIP=${STRIP:-"${CROSS_COMPILE}strip"}
READELF=${READELF:-"${CROSS_COMPILE}readelf"}
CPU=${CPU:-cortex-a53}
OUT="$RUNTIME_ROOT/mhcamera.app"

die() { printf 'build_backend: %s\n' "$*" >&2; exit 1; }

verify_backend() {
  local needed
  [ -f "$OUT" ] && [ -x "$OUT" ] || die "backend missing: $OUT"
  "$READELF" -h "$OUT" | grep -q AArch64 || die "backend is not AArch64"
  needed=$("$READELF" -d "$OUT" | awk '/\(NEEDED\)/ {gsub(/\[|\]/,"",$5); print $5}' | LC_ALL=C sort)
  for library in libainice.so libavcodec.so.58 libavutil.so.56 libcjson.so.1; do
    grep -Fxq "$library" <<<"$needed" || die "backend missing NEEDED $library"
  done
  for library in libswscale.so libswresample.so libavformat.so; do
    ! grep -Fq "$library" <<<"$needed" || die "backend must not depend on $library"
  done
  "$READELF" -d "$OUT" | grep -Eq '\((RPATH|RUNPATH)\).*[[][$]ORIGIN/libs[]]' ||
    die 'backend must use $ORIGIN/libs for private libraries'
  "$READELF" -Ws "$OUT" | awk '$7=="UND" && $8=="ainice_video_send_frame" {found=1} END {exit found?0:1}' ||
    die "backend must import ainice_video_send_frame"
  ! "$READELF" -Ws "$OUT" | awk '$7=="UND" && $8~/^(swr_|sws_|avcodec_find_encoder)/ {found=1} END {exit found?0:1}' ||
    die "backend imports an unsupported FFmpeg API"
}

case "${1:-}" in
  --help)
    printf '%s\n' 'Usage: scripts/build_backend.sh [--verify-only]' \
      'Defaults: SDKs under workspace/sdk; Arm GNU 13.2 toolchain/sysroot under workspace/toolchains.' \
      'Optional: MHCAMERA_WORKSPACE_ROOT, TOOLCHAIN_ROOT, SYSROOT, AINICE_SDK_ROOT, CJSON_ROOT' \
      'Optional: CC, CROSS_COMPILE, STRIP, READELF, CPU, CACHE_ROOT, FFMPEG_ROOT, RUNTIME_ROOT' \
      'The supplied SDK must expose usr/include/ainice and usr/lib/libainice.so.'
    exit 0;;
  --verify-only) verify_backend; exit 0;;
  '') ;;
  *) die "unknown argument: $1";;
esac
[ "$#" -eq 0 ] || die "unexpected arguments"
for tool in "$CC" "$STRIP" "$READELF"; do
  command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
done
CC=$(realpath -ms -- "$(command -v "$CC")")
STRIP=$(realpath -ms -- "$(command -v "$STRIP")")
READELF=$(realpath -ms -- "$(command -v "$READELF")")
SYSROOT=$(cd -- "$SYSROOT" && pwd -P)
AINICE_SDK_ROOT=$(cd -- "$AINICE_SDK_ROOT" && pwd -P)
CJSON_ROOT=$(cd -- "$CJSON_ROOT" && pwd -P)
FFMPEG_ROOT=$(cd -- "$FFMPEG_ROOT" && pwd -P)
mkdir -p "$RUNTIME_ROOT"
RUNTIME_ROOT=$(cd -- "$RUNTIME_ROOT" && pwd -P)
OUT="$RUNTIME_ROOT/mhcamera.app"
[ -f "$AINICE_SDK_ROOT/usr/include/ainice/video.h" ] &&
  [ -f "$AINICE_SDK_ROOT/usr/lib/libainice.so" ] || die "ainice SDK headers or library missing"
[ -f "$CJSON_ROOT/include/cjson/cJSON.h" ] && [ -f "$CJSON_ROOT/lib/libcjson.so" ] ||
  die "cJSON headers or link library missing"
[ -f "$FFMPEG_ROOT/include/libavcodec/avcodec.h" ] || die "FFmpeg headers missing; run build_ffmpeg.sh first"
CACHE_ROOT="$CACHE_ROOT" RUNTIME_ROOT="$RUNTIME_ROOT" READELF="$READELF" \
  "$SCRIPT_DIR/build_ffmpeg.sh" --verify-only
sources=(
  src/main.c src/bridge.c src/service.c src/rtsp.c
  src/xiaomi_api_client.c src/source.c src/host_input.c
  src/media_logic.c src/media_wire.c src/media.c src/media_owner.c
  src/process.c src/store.c
)
cd "$PLUGIN_ROOT"
"$CC" --sysroot="$SYSROOT" -std=gnu11 -Wall -Wextra -Werror -O3 \
  -mcpu="$CPU" -mtune="$CPU" -ffunction-sections -fdata-sections \
  -ffile-prefix-map="$PLUGIN_ROOT"=. -fmacro-prefix-map="$PLUGIN_ROOT"=. \
  -I"$AINICE_SDK_ROOT/usr/include" -I"$CJSON_ROOT/include" \
  -I"$FFMPEG_ROOT/include" -I"$SYSROOT/usr/include" \
  "${sources[@]}" \
  -L"$AINICE_SDK_ROOT/usr/lib" -L"$CJSON_ROOT/lib" -L"$FFMPEG_ROOT/lib" -L"$SYSROOT/usr/lib" \
  -Wl,-rpath-link,"$AINICE_SDK_ROOT/usr/lib" -Wl,-rpath-link,"$CJSON_ROOT/lib" \
  -Wl,-rpath-link,"$FFMPEG_ROOT/lib" -Wl,-rpath-link,"$SYSROOT/usr/lib" \
  -Wl,--gc-sections -Wl,--as-needed -Wl,--no-allow-shlib-undefined -Wl,-z,defs \
  -Wl,-rpath,'$ORIGIN/libs' \
  -lainice -lavcodec -lavutil -lcjson -pthread -lm -ldl -o "$OUT"
"$STRIP" --strip-all "$OUT"
chmod 0755 "$OUT"
verify_backend
printf '%s\n' "$OUT"
