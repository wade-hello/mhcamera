#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/env.sh"
: "${BOARD_HOST:?Set BOARD_HOST to your device address}"
BOARD_USER=${BOARD_USER:-ainice}
BOARD_PORT=${BOARD_PORT:-22}
REMOTE="$BOARD_USER@$BOARD_HOST"
CJSON_VERSION=1.7.19
CJSON_SHA256=7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562

mkdir -p "$CACHE_ROOT" "$DOWNLOAD_ROOT/cjson" \
  "$AINICE_SDK_ROOT/usr/include/ainice" "$AINICE_SDK_ROOT/usr/lib" \
  "$CJSON_ROOT/include/cjson" "$CJSON_ROOT/lib"

TMP_EXPORT=$(mktemp -d "${TMPDIR:-/tmp}/mhcamera-sdk.XXXXXX")
cleanup() {
  if [[ -S "$TMP_EXPORT/ssh-control" ]]; then
    ssh -S "$TMP_EXPORT/ssh-control" -O exit -p "$BOARD_PORT" "$REMOTE" \
      >/dev/null 2>&1 || :
  fi
  rm -r -- "$TMP_EXPORT"
}
trap cleanup EXIT

# OpenSSH 9+ defaults to SFTP; the device export uses the SCP protocol.
# Older OpenSSH already defaults to SCP and may not recognize -O.
SSH_VERSION=$(ssh -V 2>&1)
SCP_FLAGS=()
if [[ "$SSH_VERSION" =~ OpenSSH_([0-9]+) ]]; then
  if (( BASH_REMATCH[1] >= 9 )); then
    SCP_FLAGS+=(-O)
  fi
else
  printf 'An OpenSSH client is required for SDK export.\n' >&2
  exit 1
fi
SCP_FLAGS+=(-P "$BOARD_PORT" -o ConnectTimeout=10 \
  -o NumberOfPasswordPrompts=1 -o ControlMaster=auto -o ControlPersist=30 \
  -o "ControlPath=$TMP_EXPORT/ssh-control")

scp "${SCP_FLAGS[@]}" -r \
  "$REMOTE:/usr/include/ainice" \
  "$REMOTE:/usr/lib/libainice.so" \
  "$REMOTE:/usr/lib/libcjson.so.$CJSON_VERSION" \
  "$TMP_EXPORT/"

ARCHIVE="$DOWNLOAD_ROOT/cjson/cjson-$CJSON_VERSION.tar.gz"
if [[ ! -f "$ARCHIVE" ]]; then
  curl --fail --location --proto '=https' --tlsv1.2 \
    "https://github.com/DaveGamble/cJSON/archive/refs/tags/v$CJSON_VERSION.tar.gz" \
    --output "$TMP_EXPORT/cjson.tar.gz"
  printf '%s %s\n' "$CJSON_SHA256" "$TMP_EXPORT/cjson.tar.gz" | sha256sum -c -
  mv "$TMP_EXPORT/cjson.tar.gz" "$ARCHIVE"
else
  printf '%s %s\n' "$CJSON_SHA256" "$ARCHIVE" | sha256sum -c -
fi
tar -xOf "$ARCHIVE" "cJSON-$CJSON_VERSION/cJSON.h" > "$TMP_EXPORT/cJSON.h"

cp -a "$TMP_EXPORT/ainice/." "$AINICE_SDK_ROOT/usr/include/ainice/"
cp "$TMP_EXPORT/libainice.so" "$AINICE_SDK_ROOT/usr/lib/"
cp "$TMP_EXPORT/libcjson.so.$CJSON_VERSION" "$CJSON_ROOT/lib/"
cp "$TMP_EXPORT/cJSON.h" "$CJSON_ROOT/include/cjson/"
ln -sfn "libcjson.so.$CJSON_VERSION" "$CJSON_ROOT/lib/libcjson.so.1"
ln -sfn libcjson.so.1 "$CJSON_ROOT/lib/libcjson.so"
printf 'SDK exported to %s and %s\n' "$AINICE_SDK_ROOT" "$CJSON_ROOT"
