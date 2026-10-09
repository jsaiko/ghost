# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only
#
# arm64-osx-dynamic without the debug half: vcpkg builds each port once.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_BUILD_TYPE release)
