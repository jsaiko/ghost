# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

# PyroWave at the commit packaging/build-pyrowave.sh pins for Linux: the
# bitstream is still a draft upstream, so both ends of a session must run
# the same one. Change the two together. PyroWave builds against a slice of
# Granite (its checkout_granite.sh): Granite at the commit that script names
# plus its volk and Vulkan-Headers submodules, at the commits Granite
# records for them. vcpkg_from_github doesn't fetch submodules, so each is
# its own download.
vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO Themaister/pyrowave
  REF 89f7e47d4abbf650c91fae766728af866c5e32a0
  SHA512 45f8066d897b16ee3176c06466869b1e7f22ea0a9ae6f6641767cd9d423989bfd09649c68414b5078f81d0ad8be0c591840dfd85025d02d794f2b047f14d2c7f
  HEAD_REF main
)
vcpkg_from_github(
  OUT_SOURCE_PATH GRANITE_PATH
  REPO Themaister/Granite
  REF 1b2d1801d2910fb09ebcded2f0bb3a3a781103b5
  SHA512 035f0c4abc29f8c4e46d26428dd48505c98d9d5ab70dedc9b8fcfa0eaa57d65384336cb2f74538810529d61d948387ff4f09c51d9593664557809655defc4234
  HEAD_REF master
)
vcpkg_from_github(
  OUT_SOURCE_PATH VOLK_PATH
  REPO zeux/volk
  REF 47cddf7ed97b94118a08aacb548a411188e016cc
  SHA512 aa15c3fa060134b7c7887e584ea929e8164fa5e79c01acd76719201c1b4bec27f30b28796566d9863754b328861e936a51bde04d0740cb0e8d08d1371207701d
  HEAD_REF master
)
vcpkg_from_github(
  OUT_SOURCE_PATH VULKAN_HEADERS_PATH
  REPO KhronosGroup/Vulkan-Headers
  REF 6802bb4733b63ed5efd3adb308a6c885ef180ea1
  SHA512 212aa777b4802336db7e5f47c26f0653456d02b3dd90e678aa6ed5890d89f32aee78bedc45df3bf0a0f5b029709e492f578d186c33e0dc6d84f2b6a655bfeb82
  HEAD_REF main
)

file(REMOVE_RECURSE "${SOURCE_PATH}/Granite")
file(COPY "${GRANITE_PATH}/" DESTINATION "${SOURCE_PATH}/Granite")
file(REMOVE_RECURSE "${SOURCE_PATH}/Granite/third_party/volk")
file(COPY "${VOLK_PATH}/" DESTINATION "${SOURCE_PATH}/Granite/third_party/volk")
file(REMOVE_RECURSE "${SOURCE_PATH}/Granite/third_party/khronos/vulkan-headers")
file(COPY "${VULKAN_HEADERS_PATH}/" DESTINATION "${SOURCE_PATH}/Granite/third_party/khronos/vulkan-headers")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME pyrowave-shared CONFIG_PATH share/pyrowave-shared/cmake)
# PyroWave installs its .pc under share/; vcpkg's pkg-config path is lib/.
foreach(prefix "${CURRENT_PACKAGES_DIR}" "${CURRENT_PACKAGES_DIR}/debug")
  if(EXISTS "${prefix}/share/pkgconfig/pyrowave-shared.pc")
    file(INSTALL "${prefix}/share/pkgconfig/pyrowave-shared.pc" DESTINATION "${prefix}/lib/pkgconfig")
    file(REMOVE_RECURSE "${prefix}/share/pkgconfig")
  endif()
endforeach()
vcpkg_fixup_pkgconfig()
vcpkg_copy_tools(TOOL_NAMES pyrowave-device-validation AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${GRANITE_PATH}/LICENSE")
