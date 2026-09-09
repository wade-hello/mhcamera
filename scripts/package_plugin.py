#!/usr/bin/env python3
"""Build a local Mi Camera plugin package and its FFmpeg source companion."""

from __future__ import print_function

import argparse
import fcntl
import gzip
import hashlib
import io
import json
import os
import re
import stat
import struct
import sys
import tarfile
import tempfile
from contextlib import ExitStack, contextmanager
from pathlib import Path
from urllib.parse import urlparse


REPO_ROOT = Path(__file__).resolve().parents[1]
ROOT = REPO_ROOT / "mhcamera"
PLUGIN_ID = "mhcamera"
PLUGIN_SOURCE = ROOT
MAX_PACKAGE_BYTES = 64 * 1024 * 1024
MAX_UNCOMPRESSED_BYTES = 128 * 1024 * 1024
MAX_ENTRIES = 256
MAX_PATH_DEPTH = 16
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EXPECTED_LIBRARY_NAMES = {
    "libavcodec.so.58",
    "libavutil.so.56",
}
LIBRARY_RE = re.compile(r"^libav(?:codec|util)\.so\.[0-9]+$")
FFMPEG_SOURCE_SHA256 = "e80b380d595c809060f66f96a5d849511ef4a76a26b76eacf5778b94c3570309"
GO2RTC_COMMIT = "b5948cfb25404cc5cb37b166ecaa2dca20b11d4b"
GO2RTC_SOURCE_SHA256 = "78aa79bcedec8f155e4060a379613979b0b3ee48ff62ee5164bafc0ac6532386"
EXPECTED_UPGRADE_URL = ""
REQUIRED_RUNTIME_TOP_LEVEL = {
    "bin",
    "legal",
    "libs",
    "mhcamera.app",
}
REQUIRED_GO2RTC_LEGAL = {
    "BUILD-PROVENANCE.json",
    "GO-MODULES.json",
    "LICENSE",
    "SOURCE-AND-BUILD.md",
    "THIRD-PARTY-LICENSES.json",
    "modules",
}
REQUIRED_SOURCE_LEGAL = {
    "README.md",
    "cjson",
    "ffmpeg",
    "go2rtc",
}
REQUIRED_CJSON_LEGAL = {
    "LICENSE",
    "NOTICE.txt",
}
REQUIRED_FFMPEG_LEGAL = {
    "COPYING.LGPLv2.1",
    "SOURCE-AND-BUILD.txt",
}
REQUIRED_SOURCE_GO2RTC_LEGAL = {
    "LICENSE",
}
METADATA_FIELDS = (
    "id",
    "version",
    "package_url",
    "sha256",
    "size",
    "change_log",
)
DEFAULT_CHANGE_LOG = ("Build from the published mhcamera source.",)
RUNTIME_SECRET_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----",
    b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----BEGIN OPENSSH PRIVATE KEY-----",
)
PRIVATE_BUILD_OPTION_PATH_RE = re.compile(
    br"--(?:prefix|sysroot|pkg-config)=/(?:home|root|Users|workspace)/"
)
PRIVATE_KEY_BLOCK_RE = re.compile(
    br"-----BEGIN (PRIVATE KEY|RSA PRIVATE KEY|OPENSSH PRIVATE KEY)-----\r?\n"
    br"(?:[A-Za-z0-9+/=]{16,128}\r?\n){1,1024}"
    br"-----END \1-----"
)


def contains_forbidden_secret(data):
    if PRIVATE_KEY_BLOCK_RE.search(data):
        return True
    # Executables may contain PEM delimiter literals from parsers without
    # containing key material. Text/config files still reject any delimiter,
    # including a truncated key block.
    return not data.startswith(b"\x7fELF") and any(
        marker in data for marker in RUNTIME_SECRET_MARKERS
    )


def contains_private_build_environment(data):
    markers = (str(ROOT).encode("utf-8"), str(cache_root()).encode("utf-8"))
    return bool(PRIVATE_BUILD_OPTION_PATH_RE.search(data)) or any(
        marker in data for marker in markers
    )


def reject_duplicate_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("JSON contains duplicate key: %s" % key)
        result[key] = value
    return result


def load_json_object_bytes(data, description):
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=reject_duplicate_pairs)
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError("%s is invalid JSON: %s" % (description, error))
    if not isinstance(value, dict):
        raise ValueError("%s must contain a JSON object" % description)
    return value


def load_json_array_bytes(data, description):
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=reject_duplicate_pairs)
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError("%s is invalid JSON: %s" % (description, error))
    if not isinstance(value, list):
        raise ValueError("%s must contain a JSON array" % description)
    return value


def decode_json_object_stream(data, description):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("%s is not UTF-8: %s" % (description, error))
    decoder = json.JSONDecoder(object_pairs_hook=reject_duplicate_pairs)
    offset = 0
    values = []
    while True:
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        try:
            value, offset = decoder.raw_decode(text, offset)
        except ValueError as error:
            raise ValueError("%s is not a valid JSON object stream: %s" % (description, error))
        if not isinstance(value, dict):
            raise ValueError("%s contains a non-object value" % description)
        values.append(value)
    if not values:
        raise ValueError("%s is empty" % description)
    return values


def validate_module_record(module, description):
    allowed = {"Path", "Version", "Sum", "GoModSum", "Main", "Replace"}
    if set(module) - allowed:
        raise ValueError("%s contains host-only or unknown fields" % description)
    if not isinstance(module.get("Path"), str) or not module["Path"]:
        raise ValueError("%s has an invalid module path" % description)
    replacement = module.get("Replace")
    if replacement is not None:
        if not isinstance(replacement, dict):
            raise ValueError("%s contains an invalid replacement" % description)
        validate_module_record(replacement, description + " replacement")


def read_regular(path, description, max_bytes=MAX_UNCOMPRESSED_BYTES):
    try:
        descriptor = os.open(
            str(path),
            os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0),
        )
    except OSError as error:
        raise ValueError("%s is unavailable: %s" % (description, error))
    try:
        status = os.fstat(descriptor)
        if not stat.S_ISREG(status.st_mode):
            raise ValueError("%s is not a regular file: %s" % (description, path))
        if status.st_size < 1 or status.st_size > max_bytes:
            raise ValueError("%s has an invalid size: %s" % (description, path))
        chunks = []
        remaining = status.st_size
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError("%s changed while being read: %s" % (description, path))
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ValueError("%s grew while being read: %s" % (description, path))
        return b"".join(chunks), status.st_mode
    finally:
        os.close(descriptor)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_directory(path, description):
    try:
        status = path.lstat()
    except OSError as error:
        raise ValueError("%s is unavailable: %s" % (description, error))
    if path.is_symlink() or not stat.S_ISDIR(status.st_mode):
        raise ValueError("%s is not a real directory: %s" % (description, path))


def iter_regular_tree(root, description):
    require_directory(root, description)
    files = []
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root)
        if len(relative.parts) > MAX_PATH_DEPTH:
            raise ValueError("%s path exceeds %d components: %s" % (description, MAX_PATH_DEPTH, path))
        try:
            status = path.lstat()
        except OSError as error:
            raise ValueError("%s entry is unavailable: %s" % (description, error))
        if path.is_symlink():
            raise ValueError("%s contains a symlink: %s" % (description, path))
        if stat.S_ISDIR(status.st_mode):
            continue
        if not stat.S_ISREG(status.st_mode):
            raise ValueError("%s contains a special file: %s" % (description, path))
        files.append(path)
    return files


def workspace_root():
    return Path(os.environ.get("MHCAMERA_WORKSPACE_ROOT", str(REPO_ROOT))).resolve()


def cache_root():
    return Path(os.environ.get("CACHE_ROOT", str(workspace_root() / "build"))).resolve()


def validate_manifest(data, description):
    allowed = {"id", "version", "upgrade_url", "name", "description"}
    if set(data) != allowed:
        raise ValueError("%s fields are invalid" % description)
    if data.get("id") != PLUGIN_ID:
        raise ValueError("%s id must be %s" % (description, PLUGIN_ID))
    version = data.get("version")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise ValueError("%s version must be a three-part numeric version" % description)
    if data.get("upgrade_url") != EXPECTED_UPGRADE_URL:
        raise ValueError("%s upgrade_url is invalid" % description)
    for field in ("name", "description"):
        localized = data.get(field)
        if not isinstance(localized, dict) or set(localized) != {"zh", "en"}:
            raise ValueError("%s %s must contain zh and en" % (description, field))
        if any(not isinstance(value, str) or not value.strip() for value in localized.values()):
            raise ValueError("%s %s values must be non-empty strings" % (description, field))
    return data


def validate_elf64_aarch64(data, description, executable):
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("%s is not an ELF file" % description)
    if data[4] != 2 or data[5] != 1 or data[6] != 1:
        raise ValueError("%s must be little-endian ELF64" % description)
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    allowed_types = (2, 3) if executable else (3,)
    if elf_type not in allowed_types or machine != 183:
        raise ValueError("%s must be an AArch64 %s" % (
            description,
            "executable" if executable else "shared object",
        ))


def parse_checksum(data, expected_name, description):
    try:
        text = data.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise ValueError("%s is not ASCII: %s" % (description, error))
    parts = text.split()
    if len(parts) not in (1, 2) or not SHA256_RE.fullmatch(parts[0]):
        raise ValueError("%s is not a SHA-256 sidecar" % description)
    if len(parts) == 2 and parts[1].lstrip("*") != expected_name:
        raise ValueError("%s names the wrong file" % description)
    return parts[0]


def validate_source_legal(source_files):
    legal_root = PLUGIN_SOURCE / "legal"
    top = {path.name for path in legal_root.iterdir()}
    if top != REQUIRED_SOURCE_LEGAL:
        raise ValueError("source legal top-level entries are invalid")
    cjson_root = legal_root / "cjson"
    if {path.name for path in cjson_root.iterdir()} != REQUIRED_CJSON_LEGAL:
        raise ValueError("source cJSON legal entries are invalid")
    cjson_license = source_files[Path("legal/cjson/LICENSE")]
    if (b"Copyright (c) 2009-2017 Dave Gamble and cJSON contributors" not in
            cjson_license or b"Permission is hereby granted" not in cjson_license):
        raise ValueError("cJSON MIT license text is invalid")
    cjson_notice = source_files[Path("legal/cjson/NOTICE.txt")].decode("utf-8")
    for marker in (
        "cJSON 1.7.19",
        "libcjson.so.1",
        "7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562",
    ):
        if marker not in cjson_notice:
            raise ValueError("cJSON notice is missing %s" % marker)
    source_go2rtc_root = legal_root / "go2rtc"
    if ({path.name for path in source_go2rtc_root.iterdir()} !=
            REQUIRED_SOURCE_GO2RTC_LEGAL):
        raise ValueError("source go2rtc legal entries are invalid")
    source_go2rtc_license = source_files[Path("legal/go2rtc/LICENSE")]
    if (sha256_bytes(source_go2rtc_license) !=
            "b0dcf4855af5a72b4dfbd9117c207b330f4cc35658576a0b5351d6e2becac546"):
        raise ValueError("source go2rtc MIT license text digest is invalid")
    ffmpeg_root = legal_root / "ffmpeg"
    if {path.name for path in ffmpeg_root.iterdir()} != REQUIRED_FFMPEG_LEGAL:
        raise ValueError("source FFmpeg legal entries are invalid")
    license_data = source_files[Path("legal/ffmpeg/COPYING.LGPLv2.1")]
    if sha256_bytes(license_data) != "b634ab5640e258563c536e658cad87080553df6f34f62269a21d554844e58bfe":
        raise ValueError("FFmpeg LGPL-2.1 license text digest is invalid")
    source_text = source_files[Path("legal/ffmpeg/SOURCE-AND-BUILD.txt")].decode("utf-8")
    for marker in (
        "FFmpeg 4.4.4",
        "e80b380d595c809060f66f96a5d849511ef4a76a26b76eacf5778b94c3570309",
        "--prefix=/usr/local",
        "--enable-parser=hevc --enable-decoder=hevc",
    ):
        if marker not in source_text:
            raise ValueError("FFmpeg source/build notice is missing %s" % marker)


def collect_source_files():
    manifest_path = PLUGIN_SOURCE / "plugin.json"
    manifest_data, _ = read_regular(manifest_path, "source plugin manifest", 64 * 1024)
    manifest = validate_manifest(
        load_json_object_bytes(manifest_data, "source plugin manifest"),
        "source plugin manifest",
    )
    license_data, _ = read_regular(REPO_ROOT / "LICENSE", "plugin license", 64 * 1024)
    files = {Path("plugin.json"): manifest_data, Path("LICENSE"): license_data}
    for tree_name in ("www", "legal"):
        tree = PLUGIN_SOURCE / tree_name
        for path in iter_regular_tree(tree, "source %s tree" % tree_name):
            relative = Path(tree_name) / path.relative_to(tree)
            data, _ = read_regular(path, "source package file")
            files[relative] = data
    for required in (
        Path("www/index.html"),
        Path("www/assets/app.js"),
        Path("www/assets/api.js"),
        Path("www/assets/app.css"),
        Path("www/assets/dialog.js"),
    ):
        if required not in files:
            raise ValueError("source Web file is missing: %s" % required)
    validate_source_legal(files)
    return manifest, files


def validate_go_legal(runtime_files, go2rtc_digest):
    root = Path("legal/go2rtc")
    root_marker = str(ROOT).encode("utf-8")
    for relative, data in runtime_files.items():
        if relative.parts[:2] == ("legal", "go2rtc") and root_marker in data:
            raise ValueError("runtime go2rtc legal material contains a host build path: %s" % relative)
    present = {
        relative.parts[2]
        for relative in runtime_files
        if len(relative.parts) >= 3 and relative.parts[:2] == ("legal", "go2rtc")
    }
    if present != REQUIRED_GO2RTC_LEGAL:
        raise ValueError("runtime go2rtc legal entries are invalid")

    license_data = runtime_files[root / "LICENSE"]
    if b"MIT License" not in license_data or b"Copyright (c) 2022 Alexey Khit" not in license_data:
        raise ValueError("go2rtc MIT license text is invalid")
    source_notice = runtime_files[root / "SOURCE-AND-BUILD.md"]
    for marker in (
        b"v1.9.14",
        b"b5948cfb25404cc5cb37b166ecaa2dca20b11d4b",
    ):
        if marker not in source_notice:
            raise ValueError("go2rtc source/build notice is missing %s" % marker.decode("ascii"))
    provenance = load_json_object_bytes(
        runtime_files[root / "BUILD-PROVENANCE.json"],
        "go2rtc build provenance",
    )
    required_provenance = {
        "schema": 2,
        "runtime": "linux-arm64",
        "upstream_version": "v1.9.14",
        "upstream_commit": GO2RTC_COMMIT,
        "source_archive_sha256": GO2RTC_SOURCE_SHA256,
        "cgo_enabled": False,
        "goos": "linux",
        "goarch": "arm64",
        "build_id": "",
        "go_modules_format": "concatenated-json-objects",
        "go_modules_scope": "runtime-package-graph",
        "go_modules_sha256": sha256_bytes(runtime_files[root / "GO-MODULES.json"]),
        "third_party_licenses_sha256": sha256_bytes(
            runtime_files[root / "THIRD-PARTY-LICENSES.json"]
        ),
        "binary_sha256": go2rtc_digest,
    }
    for key, expected_value in required_provenance.items():
        if provenance.get(key) != expected_value:
            raise ValueError("go2rtc build provenance has an invalid %s" % key)
    modules = decode_json_object_stream(
        runtime_files[root / "GO-MODULES.json"],
        "go2rtc module graph",
    )
    main = [item for item in modules if item.get("Main") is True]
    if len(main) != 1 or main[0].get("Path") != "github.com/AlexxIT/go2rtc":
        raise ValueError("go2rtc module graph has an invalid main module")
    expected = set()
    for number, module in enumerate(modules, 1):
        validate_module_record(module, "go2rtc module graph entry %d" % number)
        if module.get("Main") is True:
            continue
        effective = module.get("Replace") or module
        if not isinstance(effective, dict):
            raise ValueError("go2rtc module graph contains an invalid replacement")
        path = effective.get("Path")
        version = effective.get("Version")
        if not isinstance(path, str) or not path or not isinstance(version, str) or not version:
            raise ValueError("go2rtc module graph contains an unversioned dependency")
        expected.add((path, version))

    index = load_json_array_bytes(
        runtime_files[root / "THIRD-PARTY-LICENSES.json"],
        "go2rtc third-party license index",
    )
    indexed = set()
    referenced = set()
    for number, item in enumerate(index, 1):
        if not isinstance(item, dict):
            raise ValueError("go2rtc license index entry %d is not an object" % number)
        if set(item) != {"module", "version", "license_files"}:
            raise ValueError("go2rtc license index entry %d fields are invalid" % number)
        module = item.get("module")
        version = item.get("version")
        license_files = item.get("license_files")
        if (not isinstance(module, str) or not module or
                not isinstance(version, str) or not version):
            raise ValueError("go2rtc license index module identity is invalid")
        key = (module, version)
        if key in indexed:
            raise ValueError("go2rtc license index contains duplicate module: %r" % (key,))
        indexed.add(key)
        if not isinstance(license_files, list) or not license_files:
            raise ValueError("go2rtc license index module has no license files: %r" % (key,))
        for license_item in license_files:
            if not isinstance(license_item, dict):
                raise ValueError("go2rtc license file index is invalid: %r" % (key,))
            if set(license_item) != {"path", "sha256"}:
                raise ValueError("go2rtc license file index fields are invalid: %r" % (key,))
            path_text = license_item.get("path")
            digest = license_item.get("sha256")
            if not isinstance(path_text, str) or not isinstance(digest, str):
                raise ValueError("go2rtc license file index fields are invalid: %r" % (key,))
            path = Path(path_text)
            if (path.is_absolute() or "\\" in path_text or ".." in path.parts or
                    path.parts[:3] != ("legal", "go2rtc", "modules")):
                raise ValueError("go2rtc license index path is unsafe: %s" % path_text)
            if not SHA256_RE.fullmatch(digest):
                raise ValueError("go2rtc license index SHA-256 is invalid: %s" % path_text)
            data = runtime_files.get(path)
            if data is None or sha256_bytes(data) != digest:
                raise ValueError("go2rtc indexed license digest mismatch: %s" % path_text)
            referenced.add(path)
    if indexed != expected:
        missing = sorted(expected - indexed)
        extra = sorted(indexed - expected)
        raise ValueError("go2rtc license index/module graph mismatch; missing=%r extra=%r" % (missing, extra))
    module_files = {
        relative for relative in runtime_files
        if relative.parts[:3] == ("legal", "go2rtc", "modules")
    }
    if module_files != referenced:
        raise ValueError("go2rtc modules legal tree contains unindexed files")


def validate_runtime(runtime):
    runtime = Path(runtime).resolve(strict=True)
    require_directory(runtime, "runtime directory")
    top = {entry.name for entry in runtime.iterdir()}
    if top != REQUIRED_RUNTIME_TOP_LEVEL:
        raise ValueError("runtime top-level entries are invalid: %r" % sorted(top))
    paths = iter_regular_tree(runtime, "runtime tree")
    files = {}
    total = 0
    for path in paths:
        relative = path.relative_to(runtime)
        data, mode = read_regular(path, "runtime package file")
        files[relative] = data
        total += len(data)
        if total > MAX_UNCOMPRESSED_BYTES:
            raise ValueError("runtime exceeds the uncompressed package size limit")
        if relative in (Path("mhcamera.app"), Path("bin/go2rtc")) and not mode & 0o111:
            raise ValueError("runtime executable bit is missing: %s" % relative)
        if contains_forbidden_secret(data):
            raise ValueError("runtime package file contains a forbidden secret marker: %s" % relative)

    if set(path.name for path in (runtime / "bin").iterdir()) != {"go2rtc"}:
        raise ValueError("runtime bin directory must contain only go2rtc")
    app_data = files.get(Path("mhcamera.app"))
    go2rtc_data = files.get(Path("bin/go2rtc"))
    if app_data is None or go2rtc_data is None:
        raise ValueError("runtime backend or go2rtc executable is missing")
    validate_elf64_aarch64(app_data, "mhcamera backend", True)
    validate_elf64_aarch64(go2rtc_data, "go2rtc", True)

    library_names = sorted(
        relative.name for relative in files
        if relative.parent == Path("libs") and LIBRARY_RE.fullmatch(relative.name)
    )
    if set(library_names) != EXPECTED_LIBRARY_NAMES:
        raise ValueError("runtime FFmpeg SONAME files are invalid")
    allowed_library_files = set()
    staging_only_provenance_files = set()
    for name in library_names:
        library_path = Path("libs") / name
        base_name = name.split(".so.", 1)[0]
        provenance_path = Path("libs") / (base_name + ".provenance.env")
        checksum_path = Path("libs") / (base_name + ".so.sha256")
        allowed_library_files.update((library_path, provenance_path, checksum_path))
        staging_only_provenance_files.add(provenance_path)
        if provenance_path not in files or checksum_path not in files:
            raise ValueError("runtime FFmpeg provenance is incomplete for %s" % name)
        validate_elf64_aarch64(files[library_path], name, False)
        digest = sha256_bytes(files[library_path])
        if parse_checksum(files[checksum_path], name, str(checksum_path)) != digest:
            raise ValueError("runtime FFmpeg checksum mismatch for %s" % name)
        try:
            provenance = files[provenance_path].decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError("runtime FFmpeg provenance is not UTF-8: %s" % error)
        required_lines = {
            "MHCAMERA_FFMPEG_PROVENANCE_SCHEMA=mhcamera-ffmpeg-v1",
            "MHCAMERA_FFMPEG_VERSION=4.4.4",
            "MHCAMERA_FFMPEG_SOURCE_SHA256=%s" % FFMPEG_SOURCE_SHA256,
            "MHCAMERA_FFMPEG_DECODERS=hevc",
            "MHCAMERA_FFMPEG_ENCODERS=none",
            "MHCAMERA_FFMPEG_PARSERS=hevc",
            "MHCAMERA_FFMPEG_BSFS=null",
            "MHCAMERA_FFMPEG_DEMUXERS=none",
            "MHCAMERA_FFMPEG_MUXERS=none",
            "MHCAMERA_FFMPEG_PROTOCOLS=none",
            "MHCAMERA_FFMPEG_ARTIFACT=%s" % name,
            "MHCAMERA_FFMPEG_SHA256=%s" % digest,
        }
        if not required_lines.issubset(set(provenance.splitlines())):
            raise ValueError("runtime FFmpeg provenance is invalid for %s" % name)
    actual_library_files = {relative for relative in files if relative.parent == Path("libs")}
    if actual_library_files != allowed_library_files:
        raise ValueError("runtime libs directory contains unexpected files")
    for provenance_path in staging_only_provenance_files:
        del files[provenance_path]

    legal_top = {entry.name for entry in (runtime / "legal").iterdir()}
    if legal_top != {"go2rtc"}:
        raise ValueError("runtime legal directory must contain only go2rtc material")
    validate_go_legal(files, sha256_bytes(go2rtc_data))
    return runtime, files


def archive_files(source_files, runtime_files):
    combined = {}
    for relative, data in source_files.items():
        combined[Path(PLUGIN_ID) / relative] = (data, 0o644)
    for relative, data in runtime_files.items():
        archive_path = Path(PLUGIN_ID) / relative
        if archive_path in combined:
            if (relative == Path("legal/go2rtc/LICENSE") and
                    combined[archive_path][0] == data):
                continue
            raise ValueError("source and runtime package files collide: %s" % relative)
        mode = 0o755 if relative in (Path("mhcamera.app"), Path("bin/go2rtc")) else 0o644
        combined[archive_path] = (data, mode)
    for archive_path, (data, _) in combined.items():
        if contains_forbidden_secret(data):
            raise ValueError("plugin package file contains a forbidden secret marker: %s" % archive_path)
        if contains_private_build_environment(data):
            raise ValueError(
                "plugin package file contains a private build environment detail: %s"
                % archive_path
            )
    if sum(len(data) for data, _ in combined.values()) > MAX_UNCOMPRESSED_BYTES:
        raise ValueError("plugin exceeds the uncompressed package size limit")
    return sorted(combined.items(), key=lambda item: item[0].as_posix())


def archive_directories(files):
    directories = {Path(PLUGIN_ID)}
    for path, _ in files:
        parent = path.parent
        while parent != Path("."):
            directories.add(parent)
            parent = parent.parent
    return sorted(directories, key=lambda path: path.as_posix())


def add_directory(archive, path):
    info = tarfile.TarInfo(path.as_posix())
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mtime = 0
    archive.addfile(info)


def add_file(archive, path, data, mode):
    info = tarfile.TarInfo(path.as_posix())
    info.size = len(data)
    info.mode = mode
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mtime = 0
    archive.addfile(info, io.BytesIO(data))


def fsync_directory(path):
    descriptor = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def ensure_directory(path):
    path = Path(path)
    missing = []
    cursor = path
    while True:
        try:
            status = cursor.lstat()
        except FileNotFoundError:
            missing.append(cursor)
            if cursor.parent == cursor:
                raise ValueError("cannot create output directory: %s" % path)
            cursor = cursor.parent
            continue
        if cursor.is_symlink() or not stat.S_ISDIR(status.st_mode):
            raise ValueError("output path is not a real directory: %s" % cursor)
        break
    for directory in reversed(missing):
        directory.mkdir(mode=0o755)
        fsync_directory(directory.parent)


@contextmanager
def publication_lock(directory):
    ensure_directory(directory)
    descriptor = os.open(
        str(directory),
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0),
    )
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        os.close(descriptor)


def atomic_write(path, data, mode=0o644):
    ensure_directory(path.parent)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".%s." % path.name, dir=str(path.parent))
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(str(temporary), mode)
        os.replace(str(temporary), str(path))
        fsync_directory(path.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def remove_regular_if_present(path):
    try:
        status = path.lstat()
    except FileNotFoundError:
        return
    if path.is_symlink() or not stat.S_ISREG(status.st_mode):
        raise ValueError("stale metadata path is not a regular file: %s" % path)
    path.unlink()
    fsync_directory(path.parent)


def build_package(path, files):
    directories = archive_directories(files)
    entries = directories + [item[0] for item in files]
    if len(entries) > MAX_ENTRIES:
        raise ValueError("plugin package exceeds %d entries" % MAX_ENTRIES)
    if any(len(entry.parts) > MAX_PATH_DEPTH for entry in entries):
        raise ValueError("plugin package path exceeds %d components" % MAX_PATH_DEPTH)
    for entry in entries:
        text = entry.as_posix()
        if entry.is_absolute() or "\\" in text or any(part in ("", ".", "..") for part in entry.parts):
            raise ValueError("plugin package contains an unsafe path: %s" % text)
    ensure_directory(path.parent)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".%s." % path.name, dir=str(path.parent))
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                    for directory in directories:
                        add_directory(archive, directory)
                    for archive_path, (data, mode) in files:
                        add_file(archive, archive_path, data, mode)
            raw.flush()
            os.fsync(raw.fileno())
        size = temporary.stat().st_size
        if size < 1 or size > MAX_PACKAGE_BYTES:
            raise ValueError("plugin package size must be in 1..67108864")
        os.chmod(str(temporary), 0o644)
        os.replace(str(temporary), str(path))
        fsync_directory(path.parent)
        return size
    finally:
        if temporary.exists():
            temporary.unlink()


def validate_url(value):
    parsed = urlparse(value)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise ValueError("package URL must be an absolute HTTP or HTTPS URL")
    return value


def metadata_bytes(version, package_url, digest, size, change_log):
    if (not isinstance(change_log, (list, tuple)) or not change_log or
            any(not isinstance(item, str) or not item.strip() for item in change_log)):
        raise ValueError("change log must contain non-empty strings")
    metadata = {
        "id": PLUGIN_ID,
        "version": version,
        "package_url": validate_url(package_url),
        "sha256": digest,
        "size": size,
        "change_log": list(change_log),
    }
    if tuple(metadata) != METADATA_FIELDS:
        raise AssertionError("metadata field order changed")
    return (json.dumps(metadata, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def package_plugin(runtime_dir, output_dir, source_archive, package_url=None, change_log=None,
                   support_dir=None):
    output_dir = Path(output_dir).resolve(strict=False)
    support_dir = Path(
        support_dir if support_dir is not None else workspace_root() / "release-support"
    ).resolve(strict=False)
    source_archive = Path(source_archive)
    if package_url is not None:
        validate_url(package_url)
    manifest, source_files = collect_source_files()
    runtime_dir, runtime_files = validate_runtime(runtime_dir)
    allowed_roots = tuple(
        (workspace_root() / name).resolve()
        for name in ("releases", "release-support")
    )
    for directory in (output_dir, support_dir):
        for root, description in ((PLUGIN_SOURCE, "source"), (runtime_dir, "runtime")):
            try:
                directory.relative_to(root)
            except ValueError:
                continue
            raise ValueError("publication directory must be outside the %s tree" % description)
        try:
            directory.relative_to(REPO_ROOT)
        except ValueError:
            continue
        for allowed_root in allowed_roots:
            try:
                directory.relative_to(allowed_root)
                break
            except ValueError:
                continue
        else:
            raise ValueError("publication directory inside the repository must be within %s" % (
                " or ".join(str(root) for root in allowed_roots),
            ))
    for directory, other in ((support_dir, output_dir), (output_dir, support_dir)):
        try:
            directory.relative_to(other)
        except ValueError:
            continue
        raise ValueError("plugin output and support directories must be separate trees")
    source_data, _ = read_regular(source_archive, "FFmpeg source companion")
    if sha256_bytes(source_data) != FFMPEG_SOURCE_SHA256:
        raise ValueError("FFmpeg source companion must be the unmodified ffmpeg-4.4.4.tar.xz archive")
    files = archive_files(source_files, runtime_files)
    package_name = "%s-%s.plugin" % (PLUGIN_ID, manifest["version"])
    package_path = output_dir / package_name
    version_support_dir = support_dir / ("%s-%s" % (PLUGIN_ID, manifest["version"]))
    checksum_path = output_dir / (package_name + ".sha256")
    companion_name = "ffmpeg-4.4.4.tar.xz"
    companion_path = version_support_dir / "sources" / companion_name
    companion_checksum_path = companion_path.with_name(companion_name + ".sha256")
    metadata_path = version_support_dir / "latest.json"
    with ExitStack() as locks:
        for directory in sorted((output_dir, version_support_dir), key=str):
            locks.enter_context(publication_lock(directory))
        size = build_package(package_path, files)
        digest = sha256_file(package_path)
        checksum = ("%s  %s\n" % (digest, package_name)).encode("utf-8")
        atomic_write(checksum_path, checksum)
        atomic_write(companion_path, source_data)
        atomic_write(
            companion_checksum_path,
            ("%s  %s\n" % (FFMPEG_SOURCE_SHA256, companion_name)).encode("ascii"),
        )
        if package_url is not None:
            atomic_write(
                metadata_path,
                metadata_bytes(
                    manifest["version"], package_url, digest, size,
                    change_log or DEFAULT_CHANGE_LOG,
                ),
            )
        else:
            remove_regular_if_present(metadata_path)
    result = {
        "package": str(package_path),
        "sha256": digest,
        "size": size,
        "support_dir": str(version_support_dir),
        "package_checksum": str(checksum_path),
        "ffmpeg_source": str(companion_path),
        "ffmpeg_source_checksum": str(companion_checksum_path),
        "ffmpeg_source_sha256": FFMPEG_SOURCE_SHA256,
    }
    if package_url is not None:
        result["metadata"] = str(metadata_path)
    return result


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", type=Path, default=Path(
        os.environ.get("RUNTIME_ROOT", str(cache_root() / "runtime"))
    ))
    parser.add_argument("--output-dir", type=Path, default=workspace_root() / "releases")
    parser.add_argument("--support-dir", type=Path, default=workspace_root() / "release-support")
    downloads = Path(os.environ.get("DOWNLOAD_ROOT", str(workspace_root() / "downloads")))
    parser.add_argument("--ffmpeg-source-archive", type=Path,
                        default=downloads / "ffmpeg" / "ffmpeg-4.4.4.tar.xz")
    parser.add_argument("--package-url", help="optional public URL for local latest.json metadata")
    parser.add_argument("--change-log", action="append", dest="change_log")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        result = package_plugin(
            args.runtime_root, args.output_dir, args.ffmpeg_source_archive,
            args.package_url, args.change_log,
            support_dir=args.support_dir,
        )
    except (OSError, ValueError, tarfile.TarError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
