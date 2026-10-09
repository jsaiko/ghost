# macOS client

The macOS build is the client only: libgdp, `spectre` and `spectre-qt`.
The host side (wraith, ghostd) is Linux only. `spectre` runs on Vulkan
through MoltenVK and decodes with VideoToolbox (see
[the spectre client](../../docs/design/spectre-client.md)).

## Prerequisites

- Xcode or the Command Line Tools (`xcode-select --install`).
- `cmake` (3.20+), `ninja`, `pkg-config` and `nasm` on `PATH`. Homebrew has
  all four; without Homebrew, `vcpkg fetch cmake` / `vcpkg fetch ninja`
  give the first two, `pkgconf` builds from its release tarball with
  `./configure && make`, and nasm.us publishes a prebuilt macOS `nasm`.
- [vcpkg](https://github.com/microsoft/vcpkg), for SDL3, FFmpeg (with
  VideoToolbox), Opus, zstd, protobuf, ngtcp2 (with OpenSSL), the Vulkan
  loader and glslang. `vcpkg.json` here is the manifest.
- MoltenVK: the `MoltenVK-macos.tar` release from
  https://github.com/KhronosGroup/MoltenVK/releases (vcpkg has no port).
  spectre needs only `MoltenVK/dynamic/dylib/macOS/` from it --
  `libMoltenVK.dylib` and `MoltenVK_icd.json`.
- Qt 6 (6.8 LTS or later) for `spectre-qt`, from the official installer or
  `aqt install-qt mac desktop 6.8.3 clang_64`. Configure with
  `-DGHOST_BUILD_LAUNCHER=OFF` to skip it.

## Build

```sh
export VCPKG_ROOT=~/src/vcpkg
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_DIR=$PWD/packaging/macos \
  -DVCPKG_TARGET_TRIPLET=x64-osx-dynamic \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/macos
cmake --build build-macos
ctest --test-dir build-macos --output-on-failure
```

On Apple silicon the triplet is `arm64-osx-dynamic`. The first configure
builds every vcpkg dependency, which takes a while (FFmpeg is the longest).
For a release-only build, which builds each dependency once instead of
twice, add `-DVCPKG_OVERLAY_TRIPLETS=$PWD/packaging/macos/triplets
-DVCPKG_TARGET_TRIPLET=arm64-osx-dynamic-release
-DVCPKG_HOST_TRIPLET=arm64-osx-release`, as CI does.

Use `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (or `Release`) for anything but
debugging spectre itself: a Debug build links vcpkg's debug libraries,
which are built without optimisation -- FFmpeg with
`--disable-optimizations`, and ngtcp2 and OpenSSL, which handle every
datagram of the stream -- and a real session lags visibly on it. A second build tree can
share the first one's dependencies with
`-DVCPKG_INSTALLED_DIR=<first build>/vcpkg_installed`.

This produces `build-macos/bin/spectre` and
`build-macos/bin/spectre-qt.app`, which carries its own copy of `spectre`
in `Contents/MacOS/` -- that's where the launcher looks for it.

## Run

The Vulkan loader has to be told where MoltenVK is until it's bundled:

```sh
export VK_DRIVER_FILES=/path/to/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json
open build-macos/bin/spectre-qt.app   # or run spectre directly, see docs/reference/command-line.md
```

`open` doesn't pass the environment on; start
`build-macos/bin/spectre-qt.app/Contents/MacOS/spectre-qt` from the shell
instead while `VK_DRIVER_FILES` is needed.

## Keys

Mac keyboards have no Scroll Lock and need Fn for F-keys, so on macOS:

- **Ctrl+Cmd+G** captures or releases the mouse (Scroll Lock still works on keyboards
  that have one).
- **Ctrl+Cmd+F** toggles fullscreen (as does Ctrl+Shift+F11).
- The session menu chord is unchanged: Ctrl+Option+Cmd (left side).

Cmd is forwarded as Super, the same as the Windows key elsewhere, so the
remote desktop's Ctrl shortcuts (copy, paste) are Ctrl, not Cmd.

## Not done yet

- Bundling the dylibs (vcpkg's, MoltenVK and its ICD file, Qt via
  `macdeployqt`) into the .app, signing and notarization, and a .dmg.
- A zero-copy VideoToolbox path. Frames are copied (one memcpy per row)
  into the presenter's staging image; see
  [the spectre client](../../docs/design/spectre-client.md#presentation).
