# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only
#
# The host triplet (protoc, glslang) without the debug half.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_BUILD_TYPE release)
