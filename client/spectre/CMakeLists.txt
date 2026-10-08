# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)
# Session audio: Opus decode + SDL3 audio playback --
# see src/audio/. SDL3 (already linked below) covers SDL_INIT_AUDIO itself.
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # VA-API (libva/libva-drm) is Linux-only; D3D11VA on Windows and
  # VideoToolbox on macOS play the same role (see decoder.cpp/
  # video_image_source.cpp) and their libraries ship with the OS, so
  # there's nothing to find with pkg-config there.
  pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva)
  pkg_check_modules(LIBVA_DRM REQUIRED IMPORTED_TARGET libva-drm)
endif()
find_package(Vulkan REQUIRED)
# Vulkan Video decode (decoder.cpp/video_image_source.cpp) needs FFmpeg's
# Vulkan hwcontext. Its header is no evidence of that: FFmpeg installs
# hwcontext_vulkan.h even when configured --disable-vulkan (vcpkg without
# its "vulkan" feature, for one), and the backend then compiles in only to
# find no Vulkan device type at runtime. So link against av_vk_frame_alloc,
# which libavutil exports only when built with Vulkan. Re-run on every
# configure (unset below) so rebuilding FFmpeg is picked up without clearing
# the cache. Without it the backend compiles out and spectre falls back as if
# the GPU had no decode queue.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${LIBAVUTIL_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${LIBAVUTIL_LINK_LIBRARIES})
unset(SPECTRE_FFMPEG_HAS_VULKAN CACHE)
check_c_source_compiles("
#include <libavutil/hwcontext_vulkan.h>
int main(void) { return av_vk_frame_alloc() != 0; }
" SPECTRE_FFMPEG_HAS_VULKAN)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

function(spectre_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

# --- Shaders: GLSL -> SPIR-V at build time, embedded as C arrays so the
# binary has no runtime dependency on locating a shaders/ directory. ---
find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "spectre: glslangValidator not found (apt install glslang-tools; vcpkg's glslang[tools] on Windows and macOS)")
endif()

set(SPECTRE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SPECTRE_SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SPECTRE_SHADER_GEN_DIR}")

set(SPECTRE_SHADER_HEADERS "")
foreach(shader video.vert video.frag rect.vert rect.frag cursor.vert cursor.frag text.vert text.frag)
  set(src "${SPECTRE_SHADER_DIR}/${shader}")
  string(REPLACE "." "_" shader_ident "${shader}")
  set(header "${SPECTRE_SHADER_GEN_DIR}/${shader_ident}_spv.h")
  add_custom_command(
    OUTPUT "${header}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --vn "${shader_ident}_spv" -o "${header}" "${src}"
    DEPENDS "${src}"
    COMMENT "Compiling ${shader} to SPIR-V"
  )
  list(APPEND SPECTRE_SHADER_HEADERS "${header}")
endforeach()
add_custom_target(spectre_shaders DEPENDS ${SPECTRE_SHADER_HEADERS})

# --- Window icon: assets/app-icon-128.png (a downscaled copy of
# spectre-qt's app icon -- see its own CMakeLists.txt/resources/app.qrc)
# embedded as a C array, the same reason the shaders above are: so
# stream_session.cpp's SDL_SetWindowIcon() call has no loose file to find
# at runtime. ---
set(SPECTRE_ICON_SRC "${CMAKE_SOURCE_DIR}/assets/app-icon-128.png")
set(SPECTRE_ICON_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SPECTRE_ICON_HEADER "${SPECTRE_ICON_GEN_DIR}/app_icon_png.h")
file(MAKE_DIRECTORY "${SPECTRE_ICON_GEN_DIR}")
add_custom_command(
  OUTPUT "${SPECTRE_ICON_HEADER}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT=${SPECTRE_ICON_SRC}" "-DOUTPUT=${SPECTRE_ICON_HEADER}" "-DSYMBOL=kAppIconPng"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
  DEPENDS "${SPECTRE_ICON_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
  COMMENT "Embedding app-icon-128.png as a C array"
)
add_custom_target(spectre_icon DEPENDS "${SPECTRE_ICON_HEADER}")

# --- Splash bitmap: assets/app-icon-splash-256.bgra, a pre-baked raw
# 256x256 VK_FORMAT_B8G8R8A8_UNORM (premultiplied-alpha) bitmap -- embedded
# as-is (no runtime PNG decode needed) so overlay_renderer.cpp can upload it
# straight into the same textured-quad pipeline the cursor overlay uses,
# drawn centered until the first video frame arrives (see
# VulkanPresenter::present_splash). Downsampled with Lanczos from
# assets/app-icon.png (998x998), not upscaled from the 128px window
# icon, for quality; regenerate it (Lanczos resize to 256x256, premultiply
# alpha, write raw BGRA) if app-icon.png changes. ---
set(SPECTRE_SPLASH_SRC "${CMAKE_SOURCE_DIR}/assets/app-icon-splash-256.bgra")
set(SPECTRE_SPLASH_HEADER "${SPECTRE_ICON_GEN_DIR}/splash_bitmap.h")
add_custom_command(
  OUTPUT "${SPECTRE_SPLASH_HEADER}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT=${SPECTRE_SPLASH_SRC}" "-DOUTPUT=${SPECTRE_SPLASH_HEADER}" "-DSYMBOL=kSplashBitmapBgra"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
  DEPENDS "${SPECTRE_SPLASH_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
  COMMENT "Embedding app-icon-splash-256.bgra as a C array"
)
add_custom_target(spectre_splash DEPENDS "${SPECTRE_SPLASH_HEADER}")

# --- UI font: resources/fonts/DejaVuSans-subset.ttf (see the README there)
# embedded the same way, rasterized at runtime by src/ui/font.cpp. ---
set(SPECTRE_FONT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/DejaVuSans-subset.ttf")
set(SPECTRE_FONT_HEADER "${SPECTRE_ICON_GEN_DIR}/ui_font_ttf.h")
add_custom_command(
  OUTPUT "${SPECTRE_FONT_HEADER}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT=${SPECTRE_FONT_SRC}" "-DOUTPUT=${SPECTRE_FONT_HEADER}" "-DSYMBOL=kUiFontTtf"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
  DEPENDS "${SPECTRE_FONT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
  COMMENT "Embedding DejaVuSans-subset.ttf as a C array"
)
add_custom_target(spectre_ui_font DEPENDS "${SPECTRE_FONT_HEADER}")

add_executable(spectre
  src/main.cpp
  src/log.cpp
  src/prefs.cpp
  src/net/session_client.cpp
  src/net/lan_link.cpp
  src/decode/decoder.cpp
  src/decode/codec_support.cpp
  src/present/vulkan_device.cpp
  src/present/video_image_source.cpp
  src/present/lossless_plane.cpp
  src/present/overlay_renderer.cpp
  src/present/vulkan_presenter.cpp
  src/stream/stream_session.cpp
  src/input/raw_controller.cpp
  src/ui/font.cpp
  src/ui/glyphs.cpp
  src/ui/hotkey.cpp
  src/ui/session_menu.cpp
  src/ui/stats_overlay.cpp
  src/ui/tile_debug_overlay.cpp
  src/ui/toast.cpp
  src/ui/toolbar.cpp
  src/audio/opus_decode.cpp
  src/audio/jitter_buffer.cpp
  src/audio/audio_player.cpp
  src/audio/microphone_capture.cpp
  src/audio/system_volume.cpp
)
add_dependencies(spectre spectre_shaders spectre_icon spectre_splash spectre_ui_font)
if(WIN32)
  # The embedded PNG (below) only covers the *running* app's window icon;
  # Explorer/taskbar read the .exe file's own icon from a linked-in Win32
  # resource, same as spectre-qt.exe (see its CMakeLists.txt).
  target_sources(spectre PRIVATE resources/app.rc)
endif()

target_include_directories(spectre PRIVATE src third_party "${SPECTRE_SHADER_GEN_DIR}" "${SPECTRE_ICON_GEN_DIR}")
spectre_warnings(spectre)

target_link_libraries(spectre PRIVATE
  gdp
  PkgConfig::SDL3
  PkgConfig::LIBAVCODEC
  PkgConfig::LIBAVUTIL
  PkgConfig::OPUS
  Vulkan::Vulkan
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(spectre PRIVATE PkgConfig::LIBVA PkgConfig::LIBVA_DRM)
elseif(APPLE)
  # VideoToolbox decode: the codec probe (codec_support.cpp) asks
  # VideoToolbox directly, and video_image_source.cpp reads the decoded
  # CVPixelBuffers.
  target_link_libraries(spectre PRIVATE
    "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo" "-framework CoreFoundation")
else()
  # D3D11VA hardware decode and the shared-texture import into Vulkan.
  # VK_USE_PLATFORM_WIN32_KHR is what makes vulkan.h declare
  # VK_KHR_external_memory_win32; it also pulls in windows.h, hence NOMINMAX
  # (its min/max macros break <algorithm>).
  target_compile_definitions(spectre PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
  # dxguid: the D3D11_DECODER_PROFILE_* GUIDs codec_support.cpp probes.
  # iphlpapi: the route and interface lookups of lan_link.cpp.
  target_link_libraries(spectre PRIVATE d3d11 dxgi dxguid iphlpapi)
endif()

if(SPECTRE_FFMPEG_HAS_VULKAN)
  target_compile_definitions(spectre PRIVATE SPECTRE_VULKAN_DECODE)
else()
  message(STATUS "spectre: FFmpeg has no Vulkan hwcontext -- Vulkan Video decode disabled")
endif()

# The "pyrowave" codec (src/decode/pyrowave_decode.cpp): only with
# libpyrowave-shared installed (packaging/build-pyrowave.sh), and only on
# FFmpeg's Vulkan hwcontext, whose frame pool its decode writes into.
if(GHOST_ENABLE_PYROWAVE)
  pkg_check_modules(PYROWAVE IMPORTED_TARGET pyrowave-shared)
endif()
if(PYROWAVE_FOUND AND SPECTRE_FFMPEG_HAS_VULKAN)
  message(STATUS "spectre: pyrowave codec enabled (libpyrowave-shared ${PYROWAVE_VERSION})")
  target_sources(spectre PRIVATE src/decode/pyrowave_decode.cpp)
  target_compile_definitions(spectre PRIVATE SPECTRE_HAVE_PYROWAVE)
  target_link_libraries(spectre PRIVATE PkgConfig::PYROWAVE)
else()
  message(STATUS "spectre: pyrowave codec disabled (needs libpyrowave-shared and FFmpeg's Vulkan hwcontext)")
endif()

install(TARGETS spectre RUNTIME DESTINATION bin)

# --- Tools ---

# A headless session client for measuring the transport and wraith's rate
# control (tools/netprobe.cpp; tools/netem-harness drives it).
# The same SessionClient as spectre, and nothing that needs a GPU or window.
# POSIX only (poll()), like the netem harness around it.
if(NOT WIN32)
  add_executable(spectre_netprobe
    tools/netprobe.cpp
    src/log.cpp
    src/net/session_client.cpp
  )
  target_include_directories(spectre_netprobe PRIVATE src)
  spectre_warnings(spectre_netprobe)
  target_link_libraries(spectre_netprobe PRIVATE gdp)
endif()

# --- Tests ---

find_package(Threads REQUIRED)

add_executable(spectre_jitter_buffer_test
  tests/jitter_buffer_test.cpp
  src/audio/jitter_buffer.cpp
  src/audio/opus_decode.cpp
)
target_include_directories(spectre_jitter_buffer_test PRIVATE src)
spectre_warnings(spectre_jitter_buffer_test)
target_link_libraries(spectre_jitter_buffer_test PRIVATE gdp PkgConfig::OPUS Threads::Threads)
add_test(NAME spectre_jitter_buffer_test COMMAND spectre_jitter_buffer_test)

add_executable(spectre_ui_font_test
  tests/ui_font_test.cpp
  src/ui/font.cpp
)
add_dependencies(spectre_ui_font_test spectre_ui_font)
target_include_directories(spectre_ui_font_test PRIVATE src third_party "${SPECTRE_ICON_GEN_DIR}")
spectre_warnings(spectre_ui_font_test)
add_test(NAME spectre_ui_font_test COMMAND spectre_ui_font_test)
