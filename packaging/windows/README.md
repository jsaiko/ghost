# Windows client

The Windows build is the client only: libgdp, `spectre` and `spectre-qt`.
The host side (wraith, ghostd) is Linux only. `spectre` decodes with
Vulkan Video or D3D11VA and presents with Vulkan (see
[the spectre client](../../docs/design/spectre-client.md)).

## Prerequisites

- Visual Studio 2022 or its Build Tools, with the "Desktop development
  with C++" workload. It brings MSVC, and CMake and Ninja on the
  developer prompt's `PATH`.
- Git.
- [vcpkg](https://github.com/microsoft/vcpkg), for SDL3, FFmpeg (with
  Vulkan), Opus, zstd, protobuf, ngtcp2 (with OpenSSL), Qt, PyroWave, the
  Vulkan loader, glslang and pkgconf. `vcpkg.json` here is the manifest;
  the build runs vcpkg itself, so nothing is installed by hand. PyroWave
  has no vcpkg port of its own: `ports/pyrowave` builds the commit
  `packaging/build-pyrowave.sh` pins for Linux hosts, since both ends of
  a session must run the same one.

  ```bat
  git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
  C:\src\vcpkg\bootstrap-vcpkg.bat -disableMetrics
  ```

No Vulkan SDK is needed: the loader and headers come from vcpkg, and the
Vulkan driver comes with the GPU driver.

## Build

From an **x64 Native Tools Command Prompt for VS 2022** (the default
Developer Command Prompt targets 32-bit x86):

```bat
set VCPKG_ROOT=C:\src\vcpkg
cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_MANIFEST_DIR=%CD%\packaging\windows ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build
ctest --test-dir build --output-on-failure
```

The first configure builds every vcpkg dependency into
`build\vcpkg_installed`, which takes a while (Qt and FFmpeg are the
longest). A second build tree can share them with
`-DVCPKG_INSTALLED_DIR=<first build>\vcpkg_installed`.

Use `Debug` only for debugging spectre itself: a Debug build links
vcpkg's debug libraries, which are built without optimisation -- FFmpeg,
and ngtcp2 and OpenSSL, which handle every datagram of the stream -- and
a real session lags visibly on it.

This produces `build\bin\spectre.exe` and `build\bin\spectre-qt.exe`,
with every DLL they load copied next to them and Qt's platform plugin in
`build\bin\platforms\`. The launcher looks for `spectre.exe` in its own
directory.

## Run

Start `build\bin\spectre-qt.exe`, or run `spectre.exe` directly (see
[command line](../../docs/reference/command-line.md#spectre)).
`spectre --probe-decoders` lists the decoders that work on the machine.

`build\bin` runs on another machine as it is, given the
[Microsoft Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)
(x64), which vcpkg does not copy.

## Not done yet

- An installer, code signing, and a Start menu entry.
