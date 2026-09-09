#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
PLUGIN_ROOT="$ROOT/mhcamera"
WORKSPACE_ROOT=$(realpath -m "${MHCAMERA_WORKSPACE_ROOT:-"$ROOT"}")
CACHE_ROOT=${CACHE_ROOT:-"$WORKSPACE_ROOT/build"}
RUNTIME_ROOT=${RUNTIME_ROOT:-"$CACHE_ROOT/runtime"}
FFMPEG_ROOT=${FFMPEG_ROOT:-"$CACHE_ROOT/ffmpeg/install/usr/local"}
DOWNLOAD_ROOT=${DOWNLOAD_ROOT:-"$WORKSPACE_ROOT/downloads"}
DOWNLOADS=${DOWNLOADS:-"$DOWNLOAD_ROOT/ffmpeg"}
TOOLCHAIN_ROOT=${TOOLCHAIN_ROOT:-"$WORKSPACE_ROOT/toolchains/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu"}
CROSS_COMPILE=${CROSS_COMPILE:-"$TOOLCHAIN_ROOT/bin/aarch64-none-linux-gnu-"}
SYSROOT=${SYSROOT:-"$TOOLCHAIN_ROOT/aarch64-none-linux-gnu/libc"}
CC=${CC:-"${CROSS_COMPILE}gcc"}
READELF=${READELF:-"${CROSS_COMPILE}readelf"}
CPU=${CPU:-cortex-a53}
JOBS=${JOBS:-$(nproc)}
FFMPEG_VERSION=4.4.4
FFMPEG_SOURCE_SHA256=e80b380d595c809060f66f96a5d849511ef4a76a26b76eacf5778b94c3570309
SOURCE_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
SOURCE_ARCHIVE="$DOWNLOADS/ffmpeg-$FFMPEG_VERSION.tar.xz"

die() { printf 'build_ffmpeg: %s\n' "$*" >&2; exit 1; }

configure_features=(
  --disable-everything --disable-gpl --disable-nonfree --disable-autodetect
  --disable-bzlib --disable-iconv --disable-doc --disable-debug
  --disable-swscale --disable-swresample --disable-avresample --disable-postproc
  --disable-avdevice --disable-avfilter --disable-programs --disable-zlib
  --enable-shared --disable-static --enable-pic --enable-avcodec
  --disable-avformat --enable-avutil --disable-network
  --enable-asm --enable-neon --enable-hardcoded-tables --enable-optimizations
  --enable-parser=hevc --enable-decoder=hevc
)

verify_one() {
  local name=$1 artifact actual provenance hash_file
  provenance="$RUNTIME_ROOT/libs/$name.provenance.env"
  hash_file="$RUNTIME_ROOT/libs/$name.so.sha256"
  [ -f "$provenance" ] && [ -f "$hash_file" ] || die "missing provenance for $name"
  # shellcheck source=/dev/null
  . "$provenance"
  artifact=${MHCAMERA_FFMPEG_ARTIFACT:-}
  case "$name:$artifact" in
    libavcodec:libavcodec.so.58|libavutil:libavutil.so.56) ;;
    *) die "unexpected FFmpeg artifact: $artifact";;
  esac
  [ "${MHCAMERA_FFMPEG_PROVENANCE_SCHEMA:-}" = mhcamera-ffmpeg-v1 ] || die "bad provenance schema"
  [ "${MHCAMERA_FFMPEG_VERSION:-}" = "$FFMPEG_VERSION" ] || die "bad FFmpeg version"
  [ "${MHCAMERA_FFMPEG_SOURCE_SHA256:-}" = "$FFMPEG_SOURCE_SHA256" ] || die "source digest mismatch"
  [ "${MHCAMERA_FFMPEG_DECODERS:-}" = hevc ] || die "decoder set mismatch"
  [ "${MHCAMERA_FFMPEG_ENCODERS:-}" = none ] || die "encoder set mismatch"
  [ "${MHCAMERA_FFMPEG_PARSERS:-}" = hevc ] || die "parser set mismatch"
  [ "${MHCAMERA_FFMPEG_BSFS:-}" = null ] || die "BSF set mismatch"
  [ "${MHCAMERA_FFMPEG_DEMUXERS:-}" = none ] || die "demuxer set mismatch"
  [ "${MHCAMERA_FFMPEG_MUXERS:-}" = none ] || die "muxer set mismatch"
  [ "${MHCAMERA_FFMPEG_PROTOCOLS:-}" = none ] || die "protocol set mismatch"
  [ -f "$RUNTIME_ROOT/libs/$artifact" ] && [ ! -L "$RUNTIME_ROOT/libs/$artifact" ] || die "missing $artifact"
  actual=$(sha256sum "$RUNTIME_ROOT/libs/$artifact" | awk '{print $1}')
  [ "$actual" = "${MHCAMERA_FFMPEG_SHA256:-}" ] || die "$name provenance digest mismatch"
  (cd "$RUNTIME_ROOT/libs" && sha256sum -c "$(basename "$hash_file")") >/dev/null || die "$name checksum mismatch"
  "$READELF" -h "$RUNTIME_ROOT/libs/$artifact" | grep -q AArch64 || die "$artifact is not AArch64"
}

verify_contract() {
  local name flag
  for name in libavcodec libavutil; do verify_one "$name"; done
  for flag in --enable-decoder=h264 --enable-encoder --enable-gpl --enable-nonfree \
    --enable-swscale --enable-swresample --enable-avfilter; do
    case "${MHCAMERA_FFMPEG_CONFIGURE:-}" in *"$flag"*) die "forbidden configure flag: $flag";; esac
  done
  for flag in --enable-parser=hevc --enable-decoder=hevc --disable-gpl --disable-nonfree; do
    case "${MHCAMERA_FFMPEG_CONFIGURE:-}" in *"$flag"*) ;; *) die "missing configure flag: $flag";; esac
  done
}

case "${1:-}" in
  --help)
    printf '%s\n' 'Usage: scripts/build_ffmpeg.sh [--verify-only]' \
      'Defaults: Arm GNU 13.2 toolchain/sysroot under workspace/toolchains.' \
      'Optional: MHCAMERA_WORKSPACE_ROOT, TOOLCHAIN_ROOT, SYSROOT, DOWNLOAD_ROOT, DOWNLOADS' \
      'Optional: CC, CROSS_COMPILE, READELF, CPU, JOBS, CACHE_ROOT, FFMPEG_ROOT, RUNTIME_ROOT' \
      'Downloads remain in DOWNLOADS; headers/libraries install to FFMPEG_ROOT.'
    exit 0;;
  --verify-only) verify_contract; exit 0;;
  '') ;;
  *) die "unknown argument: $1";;
esac
[ "$#" -eq 0 ] || die "unexpected arguments"
for tool in "$CC" "$READELF" "${CROSS_COMPILE}gcc" curl make tar sha256sum realpath; do
  command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
done
SYSROOT=$(cd -- "$SYSROOT" && pwd -P)
CACHE_ROOT=$(realpath -m "$CACHE_ROOT")
RUNTIME_ROOT=$(realpath -m "$RUNTIME_ROOT")
FFMPEG_ROOT=$(realpath -m "$FFMPEG_ROOT")
DOWNLOADS=$(realpath -m "$DOWNLOADS")
SOURCE_ARCHIVE="$DOWNLOADS/ffmpeg-$FFMPEG_VERSION.tar.xz"
CC=$(realpath -ms -- "$(command -v "$CC")")
cross_gcc=$(realpath -ms -- "$(command -v "${CROSS_COMPILE}gcc")")
READELF=$(realpath -ms -- "$(command -v "$READELF")")
export PATH="$(dirname "$CC"):$(dirname "$cross_gcc"):$PATH"
mkdir -p "$DOWNLOADS" "$CACHE_ROOT/ffmpeg" "$RUNTIME_ROOT/libs" "$FFMPEG_ROOT"
if [ ! -f "$SOURCE_ARCHIVE" ]; then
  download_tmp=$(mktemp "$DOWNLOADS/.ffmpeg.XXXXXX")
  if ! curl --fail --location --retry 3 --proto '=https' --tlsv1.2 --output "$download_tmp" "$SOURCE_URL"; then
    rm -f -- "$download_tmp"
    die "download failed: $SOURCE_URL"
  fi
  if ! printf '%s  %s\n' "$FFMPEG_SOURCE_SHA256" "$download_tmp" | sha256sum -c -; then
    rm -f -- "$download_tmp"
    die "downloaded source digest mismatch"
  fi
  mv -- "$download_tmp" "$SOURCE_ARCHIVE"
fi
printf '%s  %s\n' "$FFMPEG_SOURCE_SHA256" "$SOURCE_ARCHIVE" | sha256sum -c -
work_tmp=$(mktemp -d "$CACHE_ROOT/ffmpeg/build.XXXXXX")
trap 'rm -rf -- "$work_tmp"' EXIT
tar --no-same-owner -xf "$SOURCE_ARCHIVE" -C "$work_tmp"
cd "$work_tmp/ffmpeg-$FFMPEG_VERSION"
# A relative sysroot keeps host directory names out of avcodec_configuration().
ln -s -- "$SYSROOT" sysroot
configure_base=(
  --prefix=/usr/local --enable-cross-compile
  --cross-prefix="${CROSS_COMPILE##*/}" --cc="${CC##*/}" --sysroot=sysroot
  --arch=aarch64 --target-os=linux --pkg-config=false
  --cpu="$CPU" --extra-cflags="-O3 -mcpu=$CPU -mtune=$CPU"
  --extra-ldflags='-Wl,--gc-sections -Wl,--build-id'
)
./configure "${configure_base[@]}" "${configure_features[@]}"
assert_set() {
  local suffix=$1 expected=$2 actual
  actual=$(awk -v suffix="$suffix" '$1=="#define" && $2~("^CONFIG_.*_" suffix "$") && $3=="1" {n=$2; sub(/^CONFIG_/,"",n); sub("_" suffix "$","",n); print tolower(n)}' config.h | LC_ALL=C sort | paste -sd, -)
  [ "$actual" = "$expected" ] || die "unexpected $suffix set: $actual (expected $expected)"
}
assert_set DECODER hevc
assert_set ENCODER ''
assert_set PARSER hevc
assert_set BSF null
assert_set DEMUXER ''
assert_set MUXER ''
assert_set PROTOCOL ''
grep -Fxq '#define CONFIG_FFMPEG 0' config.h || die "ffmpeg CLI enabled"
grep -Fxq '#define CONFIG_FFPROBE 0' config.h || die "ffprobe CLI enabled"
printf '%s\n' 'libavcodec/hevcdsp.o: ECFLAGS += -fno-tree-vrp' >> ffbuild/config.mak
make -j"$JOBS"
DESTDIR="$work_tmp/install"
make DESTDIR="$DESTDIR" install
cp -a "$DESTDIR/usr/local/." "$FFMPEG_ROOT/"
configure_line="${configure_base[*]} ${configure_features[*]}"
for name in libavcodec libavutil; do
  artifact=$(basename "$(readlink -f "$FFMPEG_ROOT/lib/$name.so")")
  soname=$("$READELF" -d "$FFMPEG_ROOT/lib/$artifact" | awk '/\(SONAME\)/ {gsub(/\[|\]/,"",$5); print $5; exit}')
  [ -n "$soname" ] || die "$name SONAME missing"
  install -m 0644 "$FFMPEG_ROOT/lib/$artifact" "$RUNTIME_ROOT/libs/$soname"
  digest=$(sha256sum "$RUNTIME_ROOT/libs/$soname" | awk '{print $1}')
  needed=$("$READELF" -d "$RUNTIME_ROOT/libs/$soname" | awk '/\(NEEDED\)/ {gsub(/\[|\]/,"",$5); print $5}' | LC_ALL=C sort | paste -sd, -)
  {
    printf '%s\n' MHCAMERA_FFMPEG_PROVENANCE_SCHEMA=mhcamera-ffmpeg-v1 \
      "MHCAMERA_FFMPEG_VERSION=$FFMPEG_VERSION" "MHCAMERA_FFMPEG_SOURCE_SHA256=$FFMPEG_SOURCE_SHA256" \
      MHCAMERA_FFMPEG_DECODERS=hevc MHCAMERA_FFMPEG_ENCODERS=none MHCAMERA_FFMPEG_PARSERS=hevc \
      MHCAMERA_FFMPEG_BSFS=null MHCAMERA_FFMPEG_DEMUXERS=none MHCAMERA_FFMPEG_MUXERS=none \
      MHCAMERA_FFMPEG_PROTOCOLS=none "MHCAMERA_FFMPEG_ARTIFACT=$soname" \
      "MHCAMERA_FFMPEG_SHA256=$digest" "MHCAMERA_FFMPEG_NEEDED=$needed"
    printf 'MHCAMERA_FFMPEG_CONFIGURE=%q\n' "$configure_line"
  } >"$RUNTIME_ROOT/libs/$name.provenance.env"
  printf '%s  %s\n' "$digest" "$soname" >"$RUNTIME_ROOT/libs/$name.so.sha256"
done
verify_contract
printf 'FFmpeg prefix: %s\nRuntime: %s\nSource archive: %s\n' "$FFMPEG_ROOT" "$RUNTIME_ROOT" "$SOURCE_ARCHIVE"
