# Third-party materials

The project's own code is covered by the repository's [MIT license](../../LICENSE).
Third-party components and the host SDK retain their own terms. Usage and build
instructions are in the root [README](../../README.md) and [BUILD](../../BUILD.md).

## Included materials

- `cjson/LICENSE` and `cjson/NOTICE.txt`: cJSON notices. The plugin uses the host's shared cJSON library; it does not package that library.
- `ffmpeg/COPYING.LGPLv2.1` and `ffmpeg/SOURCE-AND-BUILD.txt`: FFmpeg license, exact source version and rebuild recipe.
- `go2rtc/LICENSE`: the upstream go2rtc MIT license. The build collects the actual selected Go module graph and required dependency license files into the runtime legal directory.

Keep the applicable copyright notices, licenses and generated dependency records
when distributing the plugin. The project MIT license does not replace them.

## Release companions

The packager separates the installable plugin from its distribution materials:

- `releases/`: the plugin installation package and its SHA-256 checksum file.
- `release-support/mhcamera-<version>/sources/`: the unmodified `ffmpeg-4.4.4.tar.xz` source archive and its SHA-256 checksum file.

Optional release metadata is written into the versioned support directory, not
the plugin installation directory. Support files are distribution materials,
not disposable build caches.

Keep the matching source and checksum files available when publishing a plugin,
for example as attachments to the same GitHub Release. The local directory split
does not remove the requirement to provide corresponding source. Retain the
FFmpeg build instructions and complete legal directory inside the installation
package. If the FFmpeg source, feature selection or local build changes, update
the source and build record accordingly.

## Host-provided SDK

The AInice Vision runtime provides `libainice.so`, its headers and the Web SDK.
An ordinary device account can inspect the provided development documentation
and export the compilation interfaces described in the build guide.

The exported host SDK stays in the local workspace's ignored `sdk/` directory;
it is not vendored as project source or included in the plugin package. Its
redistribution terms have not been established by this project and are not
replaced by the project's MIT license. Consult the SDK's applicable terms before
redistributing it.
