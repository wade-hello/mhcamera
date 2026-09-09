#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
PLUGIN_ROOT="$ROOT/mhcamera"
SOURCE_META="$PLUGIN_ROOT/go2rtc/SOURCE.lock"
PATCH_DIR="$PLUGIN_ROOT/go2rtc/patches"
LICENSE_COLLECTOR="$PLUGIN_ROOT/go2rtc/tools/collect_licenses.go"
WORKSPACE_ROOT=$(realpath -m "${MHCAMERA_WORKSPACE_ROOT:-"$ROOT"}")
CACHE_ROOT=${CACHE_ROOT:-"$WORKSPACE_ROOT/build"}
DOWNLOAD_ROOT=${DOWNLOAD_ROOT:-"$WORKSPACE_ROOT/downloads"}

# shellcheck disable=SC1090
source "$SOURCE_META"
CS2_IPV4_EQUALITY_PATCH_FILE="$PATCH_DIR/$GO2RTC_CS2_IPV4_EQUALITY_PATCH"
AIV1_OUTPUT_PATCH_FILE="$PATCH_DIR/$GO2RTC_AIV1_OUTPUT_PATCH"
XIAOMI_PHONE_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_PHONE_PATCH"
XIAOMI_DEVICE_PAGINATION_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_DEVICE_PAGINATION_PATCH"
XIAOMI_HOME_ROOM_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_HOME_ROOM_PATCH"
XIAOMI_SESSION_RENEWAL_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_SESSION_RENEWAL_PATCH"
XIAOMI_CS2_COMMAND_DRAIN_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_PATCH"
XIAOMI_CS2_FRAME_SAFETY_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_CS2_FRAME_SAFETY_PATCH"
XIAOMI_CS2_CLOSE_MESSAGES_PATCH_FILE="$PATCH_DIR/$GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_PATCH"
CAMERA_SOURCE_RTSP_OUTPUT_PATCH_FILE="$PATCH_DIR/$GO2RTC_CAMERA_SOURCE_RTSP_OUTPUT_PATCH"

OUT=${GO2RTC_BUILD_ROOT:-"$CACHE_ROOT/go2rtc"}
RUNTIME_ROOT=${RUNTIME_ROOT:-"$CACHE_ROOT/runtime"}
DOWNLOADS=${DOWNLOADS:-"$DOWNLOAD_ROOT/go2rtc"}
GOMODCACHE=${GOMODCACHE:-"$OUT/gomodcache"}
GOCACHE=${GOCACHE:-"$OUT/gocache"}
GO_ROOT=${GO_ROOT:-"$WORKSPACE_ROOT/toolchains/go$GO_VERSION"}

case "${1:-}" in
  --help)
    printf '%s\n' 'Usage: scripts/build_go2rtc.sh' \
      'Builds the locked Go source and toolchain for Linux ARM64.' \
      'Optional: MHCAMERA_WORKSPACE_ROOT, CACHE_ROOT, RUNTIME_ROOT, DOWNLOAD_ROOT, DOWNLOADS' \
      'Optional: GO2RTC_BUILD_ROOT, GO_ROOT, GOMODCACHE, GOCACHE'
    exit 0;;
  '') ;;
  *) printf 'build_go2rtc: unknown argument: %s\n' "$1" >&2; exit 1;;
esac
[ "$#" -eq 0 ] || { printf '%s\n' 'build_go2rtc: unexpected arguments' >&2; exit 1; }
for path_variable in OUT RUNTIME_ROOT DOWNLOADS GOMODCACHE GOCACHE GO_ROOT; do
  printf -v "$path_variable" '%s' "$(realpath -m "${!path_variable}")"
done

GO_ARCHIVE="$DOWNLOADS/go$GO_VERSION.linux-amd64.tar.gz"
SOURCE_ARCHIVE="$DOWNLOADS/go2rtc-$GO2RTC_COMMIT.tar.gz"

die() {
  echo "build_go2rtc: $*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

verify_sha256() {
  local file=$1 expected=$2 actual
  actual=$(sha256sum "$file" | awk '{print $1}')
  [ "$actual" = "$expected" ] || die "SHA-256 mismatch for $file: expected $expected, got $actual"
}

download_checked() {
  local url=$1 expected=$2 destination=$3 temporary
  if [ -f "$destination" ]; then
    verify_sha256 "$destination" "$expected"
    return
  fi
  temporary=$(mktemp "$DOWNLOADS/.download.XXXXXX")
  if ! curl --fail --location --proto '=https' --tlsv1.2 --output "$temporary" "$url"; then
    rm -f -- "$temporary"
    die "download failed: $url"
  fi
  verify_sha256 "$temporary" "$expected"
  mv -- "$temporary" "$destination"
}

for command in awk curl file grep install patch readelf sed sha256sum tar realpath; do
  require_command "$command"
done
[ "$(uname -s)" = Linux ] || die "fixed Go bootstrap supports Linux hosts only"
[ "$(uname -m)" = x86_64 ] || die "fixed Go bootstrap supports x86_64 hosts only"
[ -r "$SOURCE_META" ] || die "missing source lock: $SOURCE_META"
[ -r "$CS2_IPV4_EQUALITY_PATCH_FILE" ] || die "missing CS2 IPv4 equality patch: $CS2_IPV4_EQUALITY_PATCH_FILE"
[ -r "$AIV1_OUTPUT_PATCH_FILE" ] || die "missing AIV1 output patch: $AIV1_OUTPUT_PATCH_FILE"
[ -r "$XIAOMI_PHONE_PATCH_FILE" ] || die "missing Xiaomi phone bootstrap patch: $XIAOMI_PHONE_PATCH_FILE"
[ -r "$XIAOMI_DEVICE_PAGINATION_PATCH_FILE" ] || die "missing Xiaomi device pagination patch: $XIAOMI_DEVICE_PAGINATION_PATCH_FILE"
[ -r "$XIAOMI_HOME_ROOM_PATCH_FILE" ] || die "missing Xiaomi home/room patch: $XIAOMI_HOME_ROOM_PATCH_FILE"
[ -r "$XIAOMI_SESSION_RENEWAL_PATCH_FILE" ] || die "missing Xiaomi session renewal patch: $XIAOMI_SESSION_RENEWAL_PATCH_FILE"
[ -r "$XIAOMI_CS2_COMMAND_DRAIN_PATCH_FILE" ] || die "missing Xiaomi CS2 command drain patch: $XIAOMI_CS2_COMMAND_DRAIN_PATCH_FILE"
[ -r "$XIAOMI_CS2_FRAME_SAFETY_PATCH_FILE" ] || die "missing Xiaomi CS2 frame safety patch: $XIAOMI_CS2_FRAME_SAFETY_PATCH_FILE"
[ -r "$XIAOMI_CS2_CLOSE_MESSAGES_PATCH_FILE" ] || die "missing Xiaomi CS2 close messages patch: $XIAOMI_CS2_CLOSE_MESSAGES_PATCH_FILE"
[ -r "$CAMERA_SOURCE_RTSP_OUTPUT_PATCH_FILE" ] || die "missing Camera source RTSP output patch: $CAMERA_SOURCE_RTSP_OUTPUT_PATCH_FILE"
[ -r "$LICENSE_COLLECTOR" ] || die "missing license collector: $LICENSE_COLLECTOR"

mkdir -p "$DOWNLOADS" "$GOMODCACHE" "$GOCACHE" "$OUT/work" "$RUNTIME_ROOT"
verify_sha256 "$CS2_IPV4_EQUALITY_PATCH_FILE" "$GO2RTC_CS2_IPV4_EQUALITY_PATCH_SHA256"
verify_sha256 "$AIV1_OUTPUT_PATCH_FILE" "$GO2RTC_AIV1_OUTPUT_PATCH_SHA256"
verify_sha256 "$XIAOMI_PHONE_PATCH_FILE" "$GO2RTC_XIAOMI_PHONE_PATCH_SHA256"
verify_sha256 "$XIAOMI_DEVICE_PAGINATION_PATCH_FILE" "$GO2RTC_XIAOMI_DEVICE_PAGINATION_PATCH_SHA256"
verify_sha256 "$XIAOMI_HOME_ROOM_PATCH_FILE" "$GO2RTC_XIAOMI_HOME_ROOM_PATCH_SHA256"
verify_sha256 "$XIAOMI_SESSION_RENEWAL_PATCH_FILE" "$GO2RTC_XIAOMI_SESSION_RENEWAL_PATCH_SHA256"
verify_sha256 "$XIAOMI_CS2_COMMAND_DRAIN_PATCH_FILE" "$GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_PATCH_SHA256"
verify_sha256 "$XIAOMI_CS2_FRAME_SAFETY_PATCH_FILE" "$GO2RTC_XIAOMI_CS2_FRAME_SAFETY_PATCH_SHA256"
verify_sha256 "$XIAOMI_CS2_CLOSE_MESSAGES_PATCH_FILE" "$GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_PATCH_SHA256"
verify_sha256 "$CAMERA_SOURCE_RTSP_OUTPUT_PATCH_FILE" "$GO2RTC_CAMERA_SOURCE_RTSP_OUTPUT_PATCH_SHA256"
download_checked "$GO_HOST_ARCHIVE_URL" "$GO_HOST_ARCHIVE_SHA256" "$GO_ARCHIVE"
download_checked "$GO2RTC_TARBALL_URL" "$GO2RTC_TARBALL_SHA256" "$SOURCE_ARCHIVE"

if [ ! -x "$GO_ROOT/bin/go" ]; then
  [ ! -e "$GO_ROOT" ] || die "partial Go toolchain exists at $GO_ROOT"
  toolchain_tmp=$(mktemp -d "$OUT/.toolchain.XXXXXX")
  tar --no-same-owner -xzf "$GO_ARCHIVE" -C "$toolchain_tmp"
  mkdir -p "$(dirname -- "$GO_ROOT")"
  mv -- "$toolchain_tmp/go" "$GO_ROOT"
  rmdir -- "$toolchain_tmp"
fi
GO="$GO_ROOT/bin/go"
[ "$("$GO" version)" = "go version go$GO_VERSION linux/amd64" ] || die "unexpected Go toolchain: $("$GO" version)"

work_tmp=$(mktemp -d "$OUT/work/build.XXXXXX")
stage_tmp=$(mktemp -d "$OUT/.stage.XXXXXX")
cleanup() {
  rm -rf -- "$work_tmp" "$stage_tmp"
}
trap cleanup EXIT

tar --no-same-owner -xzf "$SOURCE_ARCHIVE" -C "$work_tmp"
SOURCE_DIR="$work_tmp/go2rtc-$GO2RTC_COMMIT"
[ -f "$SOURCE_DIR/go.mod" ] || die "source archive has unexpected layout"
apply_required_patch() {
  local patch_file=$1
  patch --batch --force --fuzz=0 --directory "$SOURCE_DIR" --strip=1 --input "$patch_file" ||
    die "required patch did not apply cleanly: $patch_file"
}
apply_required_patch "$CS2_IPV4_EQUALITY_PATCH_FILE"
apply_required_patch "$AIV1_OUTPUT_PATCH_FILE"
apply_required_patch "$XIAOMI_PHONE_PATCH_FILE"
apply_required_patch "$XIAOMI_DEVICE_PAGINATION_PATCH_FILE"
apply_required_patch "$XIAOMI_HOME_ROOM_PATCH_FILE"
apply_required_patch "$XIAOMI_SESSION_RENEWAL_PATCH_FILE"
apply_required_patch "$XIAOMI_CS2_COMMAND_DRAIN_PATCH_FILE"
apply_required_patch "$XIAOMI_CS2_FRAME_SAFETY_PATCH_FILE"
apply_required_patch "$XIAOMI_CS2_CLOSE_MESSAGES_PATCH_FILE"
apply_required_patch "$CAMERA_SOURCE_RTSP_OUTPUT_PATCH_FILE"

export GOENV=off
export GOTOOLCHAIN=local
export GOWORK=off
export GOMODCACHE
export GOCACHE
export GOPROXY=${GOPROXY:-https://proxy.golang.org,direct}
export GOSUMDB=${GOSUMDB:-sum.golang.org}
export SOURCE_DATE_EPOCH

cd "$SOURCE_DIR"
"$GO" mod download all
mkdir -p "$stage_tmp/bin" "$stage_tmp/legal/go2rtc"
build_binary() {
  local destination=$1
  CGO_ENABLED=0 GOOS=linux GOARCH=arm64 GOARM64=v8.0 \
    "$GO" build -mod=readonly -trimpath -buildvcs=false \
    -ldflags='-s -w -buildid=' -o "$destination" ./cmd/mhcamera-go2rtc
}
build_binary "$stage_tmp/bin/go2rtc"

readelf -h "$stage_tmp/bin/go2rtc" | grep -Eq 'Machine:[[:space:]]+AArch64' || die "go2rtc is not AArch64"
file "$stage_tmp/bin/go2rtc" | grep -q 'statically linked' || die "go2rtc must remain CGO-free/static"

CGO_ENABLED=0 GOOS=linux GOARCH=arm64 GOARM64=v8.0 \
  "$GO" list -mod=readonly -deps -json ./cmd/mhcamera-go2rtc >"$stage_tmp/GO-PACKAGES.raw.json"
install -m 0644 "$SOURCE_DIR/LICENSE" "$stage_tmp/legal/go2rtc/LICENSE"
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 GOAMD64=v1 \
  "$GO" run -mod=readonly "$LICENSE_COLLECTOR" \
  -packages "$stage_tmp/GO-PACKAGES.raw.json" \
  -normalized "$stage_tmp/legal/go2rtc/GO-MODULES.json" \
  -output "$stage_tmp/legal/go2rtc"
if grep -Eq '"(Dir|GoMod)"[[:space:]]*:' "$stage_tmp/legal/go2rtc/GO-MODULES.json" || \
  grep -Eq ':[[:space:]]*"/' "$stage_tmp/legal/go2rtc/GO-MODULES.json" || \
  grep -Fq "$PLUGIN_ROOT" "$stage_tmp/legal/go2rtc/GO-MODULES.json" || \
  grep -Fq "$OUT" "$stage_tmp/legal/go2rtc/GO-MODULES.json"; then
  die "normalized GO-MODULES.json contains a host path"
fi

cs2_ipv4_equality_patch_sha=$(sha256sum "$CS2_IPV4_EQUALITY_PATCH_FILE" | awk '{print $1}')
aiv1_output_patch_sha=$(sha256sum "$AIV1_OUTPUT_PATCH_FILE" | awk '{print $1}')
xiaomi_phone_patch_sha=$(sha256sum "$XIAOMI_PHONE_PATCH_FILE" | awk '{print $1}')
xiaomi_device_pagination_patch_sha=$(sha256sum "$XIAOMI_DEVICE_PAGINATION_PATCH_FILE" | awk '{print $1}')
xiaomi_home_room_patch_sha=$(sha256sum "$XIAOMI_HOME_ROOM_PATCH_FILE" | awk '{print $1}')
xiaomi_session_renewal_patch_sha=$(sha256sum "$XIAOMI_SESSION_RENEWAL_PATCH_FILE" | awk '{print $1}')
xiaomi_cs2_command_drain_patch_sha=$(sha256sum "$XIAOMI_CS2_COMMAND_DRAIN_PATCH_FILE" | awk '{print $1}')
xiaomi_cs2_frame_safety_patch_sha=$(sha256sum "$XIAOMI_CS2_FRAME_SAFETY_PATCH_FILE" | awk '{print $1}')
xiaomi_cs2_close_messages_patch_sha=$(sha256sum "$XIAOMI_CS2_CLOSE_MESSAGES_PATCH_FILE" | awk '{print $1}')
camera_source_rtsp_output_patch_sha=$(sha256sum "$CAMERA_SOURCE_RTSP_OUTPUT_PATCH_FILE" | awk '{print $1}')
modules_sha=$(sha256sum "$stage_tmp/legal/go2rtc/GO-MODULES.json" | awk '{print $1}')
licenses_sha=$(sha256sum "$stage_tmp/legal/go2rtc/THIRD-PARTY-LICENSES.json" | awk '{print $1}')
binary_sha=$(sha256sum "$stage_tmp/bin/go2rtc" | awk '{print $1}')
go_version=$("$GO" env GOVERSION)
binary_modules=$("$GO" version -m "$stage_tmp/bin/go2rtc" | sed '1s#^[^:]*:#go2rtc:#')

cat >"$stage_tmp/legal/go2rtc/SOURCE-AND-BUILD.md" <<EOF
# go2rtc source and build offer

- Runtime: Linux ARM64
- Upstream: go2rtc $GO2RTC_VERSION, commit $GO2RTC_COMMIT
- Source archive: $GO2RTC_TARBALL_URL
- Source archive SHA-256: $GO2RTC_TARBALL_SHA256
- CS2 IPv4 equality patch: $GO2RTC_CS2_IPV4_EQUALITY_PATCH
- CS2 IPv4 equality patch SHA-256: $cs2_ipv4_equality_patch_sha
- CS2 patch purpose: canonical net.IP.Equal comparison during CS2 peer validation
- AIV1 output patch: $GO2RTC_AIV1_OUTPUT_PATCH
- AIV1 output patch SHA-256: $aiv1_output_patch_sha
- AIV1 patch purpose: add-only product entry point and private ordered AIV1 AVCC Consumer
- Xiaomi phone bootstrap patch: $GO2RTC_XIAOMI_PHONE_PATCH
- Xiaomi phone bootstrap patch SHA-256: $xiaomi_phone_patch_sha
- Xiaomi phone patch purpose: private Passport SMS bootstrap with native rotated-token login handoff
- Xiaomi device pagination patch: $GO2RTC_XIAOMI_DEVICE_PAGINATION_PATCH
- Xiaomi device pagination patch SHA-256: $xiaomi_device_pagination_patch_sha
- Xiaomi device pagination patch purpose: complete pagination with validated cursors, atomic snapshots, and duplicate conflict detection
- Xiaomi home/room patch: $GO2RTC_XIAOMI_HOME_ROOM_PATCH
- Xiaomi home/room patch SHA-256: $xiaomi_home_room_patch_sha
- Xiaomi home/room patch purpose: strict six-region device catalogs with optional atomic home and room enrichment
- Xiaomi session renewal patch: $GO2RTC_XIAOMI_SESSION_RENEWAL_PATCH
- Xiaomi session renewal patch SHA-256: $xiaomi_session_renewal_patch_sha
- Xiaomi session renewal patch purpose: typed cloud errors, single-flight renewal, and atomic rotated-token persistence
- Xiaomi CS2 command drain patch: $GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_PATCH
- Xiaomi CS2 command drain patch SHA-256: $xiaomi_cs2_command_drain_patch_sha
- Xiaomi CS2 command drain upstream PR: $GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_UPSTREAM_PR
- Xiaomi CS2 command drain upstream commit: $GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_UPSTREAM_COMMIT
- Xiaomi CS2 command drain patch purpose: continuously consume post-login MISS commands and idempotently close the connection if draining fails
- Xiaomi CS2 frame safety patch: $GO2RTC_XIAOMI_CS2_FRAME_SAFETY_PATCH
- Xiaomi CS2 frame safety patch SHA-256: $xiaomi_cs2_frame_safety_patch_sha
- Xiaomi CS2 frame safety upstream PR: $GO2RTC_XIAOMI_CS2_FRAME_SAFETY_UPSTREAM_PR
- Xiaomi CS2 frame safety upstream commit: $GO2RTC_XIAOMI_CS2_FRAME_SAFETY_UPSTREAM_COMMIT
- Xiaomi CS2 frame safety patch purpose: reviewed/adapted frame guards and independent record ownership with the upstream 2 MiB reconstructed-record limit, rejected-size reporting, and guarded command/media readers
- Xiaomi CS2 close messages patch: $GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_PATCH
- Xiaomi CS2 close messages patch SHA-256: $xiaomi_cs2_close_messages_patch_sha
- Xiaomi CS2 close messages upstream source: $GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_UPSTREAM_URL
- Xiaomi CS2 close messages upstream commit: $GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_UPSTREAM_COMMIT
- Xiaomi CS2 close messages patch purpose: distinguish the CS2 close request (0xF0) from its acknowledgment (0xF1)
- Camera source RTSP output patch: $GO2RTC_CAMERA_SOURCE_RTSP_OUTPUT_PATCH
- Camera source RTSP output patch SHA-256: $camera_source_rtsp_output_patch_sha
- Camera source RTSP patch purpose: independent cancellable Xiaomi source owner shared by video-only AIV1 and default-off authenticated single-viewer RTSP/TCP with original audio, immutable prepared tracks, and generation-safe teardown
- Go toolchain: $go_version
- Go archive SHA-256: $GO_HOST_ARCHIVE_SHA256
- Build mode: CGO_ENABLED=0, GOOS=linux, GOARCH=arm64, GOARM64=v8.0
- Reproducibility flags: -trimpath -buildvcs=false -ldflags='-s -w -buildid='
- SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH

The build script verifies every downloaded archive before extraction, applies
the mandatory patch series without skip or reverse fallback, and builds the
ARM64 executable with normalized paths and dependency license records. The empty
Go build ID is intentional. When upgrading go2rtc, first check whether upstream
contains the CS2 IPv4 equality fix; if so, remove that equivalent bug patch
and its lock entry. Revalidate the phone bootstrap protocol before updating its
product patch. Revalidate Xiaomi catalog pagination, home/room metadata, and
session-renewal behavior before updating their respective patches. Preserve an
equivalent post-login MISS command drain when updating the CS2 integration.
Recheck the reviewed frame-safety adaptations against the cited upstream commit
before deleting or rebasing that patch. Remove the CS2 close-messages patch when
the selected upstream contains its cited commit or an equivalent fix.

## Embedded module record

\`\`\`
$binary_modules
\`\`\`
EOF

cat >"$stage_tmp/legal/go2rtc/BUILD-PROVENANCE.json" <<EOF
{
  "schema": 2,
  "runtime": "linux-arm64",
  "upstream_version": "$GO2RTC_VERSION",
  "upstream_commit": "$GO2RTC_COMMIT",
  "source_archive_sha256": "$GO2RTC_TARBALL_SHA256",
  "patches": [
    {
      "file": "$GO2RTC_CS2_IPV4_EQUALITY_PATCH",
      "sha256": "$cs2_ipv4_equality_patch_sha",
      "purpose": "canonical net.IP.Equal comparison during CS2 peer validation"
    },
    {
      "file": "$GO2RTC_AIV1_OUTPUT_PATCH",
      "sha256": "$aiv1_output_patch_sha",
      "purpose": "add-only product entry point and private ordered AIV1 AVCC Consumer"
    },
    {
      "file": "$GO2RTC_XIAOMI_PHONE_PATCH",
      "sha256": "$xiaomi_phone_patch_sha",
      "purpose": "private Passport SMS bootstrap with native rotated-token login handoff"
    },
    {
      "file": "$GO2RTC_XIAOMI_DEVICE_PAGINATION_PATCH",
      "sha256": "$xiaomi_device_pagination_patch_sha",
      "purpose": "complete pagination with validated cursors, atomic snapshots, and duplicate conflict detection"
    },
    {
      "file": "$GO2RTC_XIAOMI_HOME_ROOM_PATCH",
      "sha256": "$xiaomi_home_room_patch_sha",
      "purpose": "strict six-region device catalogs with optional atomic home and room enrichment"
    },
    {
      "file": "$GO2RTC_XIAOMI_SESSION_RENEWAL_PATCH",
      "sha256": "$xiaomi_session_renewal_patch_sha",
      "purpose": "typed cloud errors, single-flight renewal, and atomic rotated-token persistence"
    },
    {
      "file": "$GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_PATCH",
      "sha256": "$xiaomi_cs2_command_drain_patch_sha",
      "upstream_pr": "$GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_UPSTREAM_PR",
      "upstream_commit": "$GO2RTC_XIAOMI_CS2_COMMAND_DRAIN_UPSTREAM_COMMIT",
      "purpose": "continuously consume post-login MISS commands and idempotently close the connection if draining fails"
    },
    {
      "file": "$GO2RTC_XIAOMI_CS2_FRAME_SAFETY_PATCH",
      "sha256": "$xiaomi_cs2_frame_safety_patch_sha",
      "upstream_pr": "$GO2RTC_XIAOMI_CS2_FRAME_SAFETY_UPSTREAM_PR",
      "upstream_commit": "$GO2RTC_XIAOMI_CS2_FRAME_SAFETY_UPSTREAM_COMMIT",
      "purpose": "reviewed/adapted frame guards and independent record ownership with the upstream 2 MiB reconstructed-record limit, rejected-size reporting, and guarded command/media readers"
    },
    {
      "file": "$GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_PATCH",
      "sha256": "$xiaomi_cs2_close_messages_patch_sha",
      "upstream_url": "$GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_UPSTREAM_URL",
      "upstream_commit": "$GO2RTC_XIAOMI_CS2_CLOSE_MESSAGES_UPSTREAM_COMMIT",
      "purpose": "distinguish the CS2 close request (0xF0) from its acknowledgment (0xF1)"
    },
    {
      "file": "$GO2RTC_CAMERA_SOURCE_RTSP_OUTPUT_PATCH",
      "sha256": "$camera_source_rtsp_output_patch_sha",
      "purpose": "independent cancellable Xiaomi source owner shared by video-only AIV1 and default-off authenticated single-viewer RTSP/TCP with original audio, immutable prepared tracks, and generation-safe teardown"
    }
  ],
  "go_version": "$GO_VERSION",
  "go_archive_sha256": "$GO_HOST_ARCHIVE_SHA256",
  "source_date_epoch": $SOURCE_DATE_EPOCH,
  "cgo_enabled": false,
  "goos": "linux",
  "goarch": "arm64",
  "goarm64": "v8.0",
  "build_id": "",
  "go_modules_format": "concatenated-json-objects",
  "go_modules_scope": "runtime-package-graph",
  "go_modules_sha256": "$modules_sha",
  "third_party_licenses_sha256": "$licenses_sha",
  "binary_sha256": "$binary_sha"
}
EOF

mkdir -p "$RUNTIME_ROOT/bin" "$RUNTIME_ROOT/legal"
legal_new=$(mktemp -d "$RUNTIME_ROOT/legal/.go2rtc.XXXXXX")
cp -a "$stage_tmp/legal/go2rtc/." "$legal_new/"
if [ -e "$RUNTIME_ROOT/legal/go2rtc" ]; then
  legal_old=$(mktemp -d "$RUNTIME_ROOT/legal/.go2rtc-old.XXXXXX")
  rmdir "$legal_old"
  mv "$RUNTIME_ROOT/legal/go2rtc" "$legal_old"
  mv "$legal_new" "$RUNTIME_ROOT/legal/go2rtc"
  rm -rf -- "$legal_old"
else
  mv "$legal_new" "$RUNTIME_ROOT/legal/go2rtc"
fi
install -m 0755 "$stage_tmp/bin/go2rtc" "$RUNTIME_ROOT/bin/go2rtc"

echo "go2rtc: $RUNTIME_ROOT/bin/go2rtc"
echo "go2rtc sha256: $binary_sha"
