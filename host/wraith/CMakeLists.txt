# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

find_package(PkgConfig REQUIRED)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva)
pkg_check_modules(LIBVA_DRM REQUIRED IMPORTED_TARGET libva-drm)
# Software H.264 fallback (src/encode/software/) for hosts where no
# hardware encoder opens -- see encode/encoder_factory.cpp's backend table.
# x264 is GPL-2.0-or-later, compatible with wraith's GPL-3.0-only.
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)
# wraith.toml (src/util/config.cpp).
pkg_check_modules(TOMLPLUSPLUS REQUIRED IMPORTED_TARGET tomlplusplus)
# NVENC (src/encode/nvenc/): nothing to link -- libcuda and
# libnvidia-encode come with the NVIDIA driver and are dlopen()ed at
# runtime through the vendored nv-codec-headers loader (third_party/
# ffnvcodec, MIT), so this build runs unchanged on hosts without them.
# Vulkan is linked for the zero-copy path's dmabuf import
# (cuda_dmabuf_importer.hpp); spectre already requires it.
find_package(Vulkan REQUIRED)
# The "pyrowave" codec (src/encode/pyrowave/), only when libpyrowave-shared
# is installed (packaging/build-pyrowave.sh): no distro ships it yet.
if(GHOST_ENABLE_PYROWAVE)
  pkg_check_modules(PYROWAVE IMPORTED_TARGET pyrowave-shared)
endif()
# Session audio: a PipeWire virtual sink captures the
# session's audio, libopus encodes it -- see src/audio/.
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
# The screencast backends (docs/design/capture-backends.md): wraith as a
# client of a top-level compositor -- frames over PipeWire (src/screencast/
# pipewire_capture.*), input over libei (src/screencast/ei_input.*), the
# per-compositor handshake over D-Bus (sd-bus, hence libsystemd -- mutter's
# private API for screencast-gnome, kwin's EIS method for screencast-kwin)
# and, for kwin, a Wayland client connection of wraith's own
# (src/screencast/wayland_client.*, kwin_remote_session.*).
pkg_check_modules(LIBEI REQUIRED IMPORTED_TARGET libei-1.0)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
# screencast-ext (src/screencast/ext_*): wraith allocates its own capture
# buffers on the compositor's DRM device through gbm.
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
# Screencast dmabuf read-back for CPU-frame encoders (screencast/dmabuf_reader.cpp).
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)

pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(WRAITH_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocol")
file(MAKE_DIRECTORY "${WRAITH_PROTOCOL_DIR}")

set(WRAITH_WLR_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wlr-protocol")

# The screencast backends' RemoteSessions (src/screencast/kwin_remote_
# session.*, ext_*) are Wayland clients -- each protocol they speak needs a
# client-header plus the matching private-code (wayland-scanner's
# client-header emits no interface definitions of its own; private-code
# supplies the wl_interface/wl_message tables the client proxy calls
# marshal against). Vendored:
# third_party/kde-protocol (zkde_screencast, LGPL-2.1-or-later, and the
# kde_output_management/device v2 pair screencast-kwin sizes kwin's
# virtual output with, MIT-CMU -- all KDE protocols) and
# third_party/wlr-protocol (the two wlr input protocols,
# MIT); the ext-* and linux-dmabuf XMLs come from wayland-protocols.
set(WRAITH_CLIENT_PROTOCOL_XMLS
	"${CMAKE_CURRENT_SOURCE_DIR}/third_party/kde-protocol/zkde-screencast-unstable-v1.xml"
	"${CMAKE_CURRENT_SOURCE_DIR}/third_party/kde-protocol/kde-output-device-v2.xml"
	"${CMAKE_CURRENT_SOURCE_DIR}/third_party/kde-protocol/kde-output-management-v2.xml"
	"${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml"
	"${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml"
	# Clipboard sync for screencast-ext and screencast-kwin
	# (src/screencast/data_control_clipboard.*). The ext_ variant, not the
	# older zwlr_ one: kwin 6.6 exports only this one, while labwc/wlroots
	# export both.
	"${WAYLAND_PROTOCOLS_DIR}/staging/ext-data-control/ext-data-control-v1.xml"
	# Only because ext-image-capture-source's toplevel source manager
	# references ext_foreign_toplevel_handle_v1's interface table; unused.
	"${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
	"${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml"
	"${WRAITH_WLR_PROTOCOL_DIR}/wlr-virtual-pointer-unstable-v1.xml"
	"${WRAITH_WLR_PROTOCOL_DIR}/virtual-keyboard-unstable-v1.xml"
	"${WRAITH_WLR_PROTOCOL_DIR}/wlr-output-management-unstable-v1.xml"
)
set(WRAITH_CLIENT_PROTOCOL_HEADERS "")
set(WRAITH_CLIENT_PROTOCOL_SOURCES "")
foreach(xml ${WRAITH_CLIENT_PROTOCOL_XMLS})
	if(NOT EXISTS "${xml}")
		message(FATAL_ERROR "wraith: missing protocol XML ${xml}")
	endif()
	get_filename_component(name "${xml}" NAME_WE)
	set(header "${WRAITH_PROTOCOL_DIR}/${name}-client-protocol.h")
	set(code "${WRAITH_PROTOCOL_DIR}/${name}-client-protocol.c")
	add_custom_command(
		OUTPUT "${header}"
		COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${header}"
		DEPENDS "${xml}"
		COMMENT "Generating ${name}-client-protocol.h"
	)
	add_custom_command(
		OUTPUT "${code}"
		COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${code}"
		DEPENDS "${xml}"
		COMMENT "Generating ${name}-client-protocol.c"
	)
	list(APPEND WRAITH_CLIENT_PROTOCOL_HEADERS "${header}")
	list(APPEND WRAITH_CLIENT_PROTOCOL_SOURCES "${code}")
endforeach()
add_custom_target(wraith_client_protocols DEPENDS ${WRAITH_CLIENT_PROTOCOL_HEADERS} ${WRAITH_CLIENT_PROTOCOL_SOURCES})

# host/proto/control.proto (the ghostd <-> wraith control socket) and
# wraith.proto (wraith's own socket, for `wraith --report`) are wraith-only --
# spectre never speaks them, so they live here rather than in libgdp's shared
# proto codegen (that mirrors lobby.proto/session.proto's foreach in
# libgdp/CMakeLists.txt).
set(WRAITH_CONTROL_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto")
set(WRAITH_CONTROL_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${WRAITH_CONTROL_PROTO_GEN_DIR}")
set(WRAITH_LOCAL_PROTO_SOURCES)
foreach(proto control wraith)
  add_custom_command(
    OUTPUT "${WRAITH_CONTROL_PROTO_GEN_DIR}/${proto}.pb.cc" "${WRAITH_CONTROL_PROTO_GEN_DIR}/${proto}.pb.h"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}" "--cpp_out=${WRAITH_CONTROL_PROTO_GEN_DIR}"
            "--proto_path=${WRAITH_CONTROL_PROTO_DIR}" "${WRAITH_CONTROL_PROTO_DIR}/${proto}.proto"
    DEPENDS "${WRAITH_CONTROL_PROTO_DIR}/${proto}.proto"
    COMMENT "Generating ${proto}.pb.{h,cc}"
  )
  list(APPEND WRAITH_LOCAL_PROTO_SOURCES "${WRAITH_CONTROL_PROTO_GEN_DIR}/${proto}.pb.cc")
endforeach()

add_executable(wraith
  src/main.cpp
  src/session/session_process.cpp
  src/session/session_services.cpp
  src/encode/encoder_factory.cpp
  src/encode/vaapi/nal_bitstream.cpp
  src/encode/vaapi/h264_bitstream.cpp
  src/encode/vaapi/hevc_bitstream.cpp
  src/encode/vaapi/av1_obu.cpp
  src/encode/vaapi/vaapi_encoder_base.cpp
  src/encode/vaapi/vaapi_h264_encoder.cpp
  src/encode/vaapi/vaapi_hevc_encoder.cpp
  src/encode/vaapi/vaapi_av1_encoder.cpp
  src/encode/nvenc/nvenc_runtime.cpp
  src/encode/nvenc/cuda_dmabuf_importer.cpp
  src/encode/nvenc/nvenc_encoder.cpp
  src/encode/software/x264_encoder.cpp
  src/encode/software/xrgb_convert.cpp
  src/encode/refine/refine_encoder.cpp
  src/encode/refine/tile_tracker.cpp
  src/encode/refine/tile_source.cpp
  src/util/band_pool.cpp
  src/audio/pipewire_capture.cpp
  src/audio/opus_encode.cpp
  src/audio/audio_pipeline.cpp
  src/audio/microphone_source.cpp
  src/audio/default_node.cpp
  src/session/gdp_session.cpp
  src/session/report_cli.cpp
  src/session/report_server.cpp
  src/session/rate_controller.cpp
  src/session/path_rate_estimator.cpp
  src/session/leader_host.cpp
  src/session/clipboard_sync.cpp
  src/session/hid_evdev.cpp
  src/session/gamepad_evdev.cpp
  src/session/gamepad_devices.cpp
  src/session/raw_controllers.cpp
  src/session/uhid_codec.cpp
  src/session/seat_client.cpp
  src/session/session_cert.cpp
  src/session/token.cpp
  src/session/session_profile.cpp
  src/screencast/pipewire_capture.cpp
  src/screencast/dmabuf_reader.cpp
  src/screencast/dmabuf_tile_source.cpp
  src/screencast/ei_input.cpp
  src/screencast/frame_hold.cpp
  src/screencast/gnome_remote_session.cpp
  src/screencast/wayland_client.cpp
  src/screencast/kwin_remote_session.cpp
  src/screencast/kwin_output_size.cpp
  src/screencast/ext_image_copy_capture.cpp
  src/screencast/virtual_input.cpp
  src/screencast/ext_remote_session.cpp
  src/screencast/data_control_clipboard.cpp
  src/screencast/gnome_clipboard.cpp
  src/screencast/screencast_host.cpp
  src/util/config.cpp
  src/util/log.cpp
  src/util/pipewire_init.cpp
  src/util/render_node.cpp
  ${WRAITH_LOCAL_PROTO_SOURCES}
  ${WRAITH_CLIENT_PROTOCOL_SOURCES}
)

if(PYROWAVE_FOUND)
  message(STATUS "wraith: pyrowave codec enabled (libpyrowave-shared ${PYROWAVE_VERSION})")
  target_sources(wraith PRIVATE src/encode/pyrowave/pyrowave_encoder.cpp)
  target_compile_definitions(wraith PRIVATE WRAITH_HAVE_PYROWAVE)
  target_link_libraries(wraith PRIVATE PkgConfig::PYROWAVE)
else()
  message(STATUS "wraith: pyrowave codec disabled (no libpyrowave-shared; see packaging/build-pyrowave.sh)")
endif()

add_dependencies(wraith wraith_client_protocols)
target_include_directories(wraith PRIVATE "${WRAITH_PROTOCOL_DIR}" "${WRAITH_CONTROL_PROTO_GEN_DIR}" src)
target_include_directories(wraith SYSTEM PRIVATE third_party)

target_compile_definitions(wraith PRIVATE
  GHOST_DATADIR="${CMAKE_INSTALL_PREFIX}/share/ghost")

target_link_libraries(wraith PRIVATE
  PkgConfig::WAYLAND_SERVER
  PkgConfig::XKBCOMMON
  PkgConfig::LIBDRM
  PkgConfig::LIBVA
  PkgConfig::LIBVA_DRM
  PkgConfig::X264
  PkgConfig::TOMLPLUSPLUS
  PkgConfig::PIPEWIRE
  PkgConfig::OPUS
  PkgConfig::LIBEI
  PkgConfig::LIBSYSTEMD
  PkgConfig::WAYLAND_CLIENT
  PkgConfig::GBM
  PkgConfig::EGL
  PkgConfig::GLESV2
  Vulkan::Vulkan
  ${CMAKE_DL_LIBS}
  gdp
  protobuf::libprotobuf
  OpenSSL::Crypto
)

target_compile_options(wraith PRIVATE -Wall -Wextra -Wno-unused-parameter)

# $PREFIX/bin, where the top-level CMakeLists.txt's generated wraith.service
# ExecStart expects to find it.
install(TARGETS wraith RUNTIME DESTINATION bin)

# --- Tests ---
# Only the pure translation units have unit tests; the screencast
# backends are exercised end to end (README.md).

add_executable(wraith_token_test tests/token_test.cpp src/session/token.cpp)
target_include_directories(wraith_token_test PRIVATE src)
target_link_libraries(wraith_token_test PRIVATE OpenSSL::Crypto)
target_compile_options(wraith_token_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_token_test COMMAND wraith_token_test)

# The -G session certificate (session/session_cert.hpp), served by a real
# libgdp listener and checked from a real client connection.
add_executable(wraith_session_cert_test tests/session_cert_test.cpp src/session/session_cert.cpp)
target_include_directories(wraith_session_cert_test PRIVATE src)
target_link_libraries(wraith_session_cert_test PRIVATE gdp OpenSSL::Crypto)
target_compile_options(wraith_session_cert_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_session_cert_test COMMAND wraith_session_cert_test)

add_executable(wraith_hid_evdev_test tests/hid_evdev_test.cpp src/session/hid_evdev.cpp)
target_include_directories(wraith_hid_evdev_test PRIVATE src)
target_compile_options(wraith_hid_evdev_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_hid_evdev_test COMMAND wraith_hid_evdev_test)

# The wire's SDL gamepad order (gdp/gamepad.hpp) to evdev mapping. Links
# gdp for the shared enums but nothing else -- it's a pure function over
# two snapshots, no uinput device involved.
add_executable(wraith_gamepad_evdev_test tests/gamepad_evdev_test.cpp src/session/gamepad_evdev.cpp)
target_include_directories(wraith_gamepad_evdev_test PRIVATE src)
target_link_libraries(wraith_gamepad_evdev_test PRIVATE gdp)
target_compile_options(wraith_gamepad_evdev_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_gamepad_evdev_test COMMAND wraith_gamepad_evdev_test)

add_executable(wraith_uhid_codec_test tests/uhid_codec_test.cpp src/session/uhid_codec.cpp)
target_include_directories(wraith_uhid_codec_test PRIVATE src)
target_compile_options(wraith_uhid_codec_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_uhid_codec_test COMMAND wraith_uhid_codec_test)

# The SCM_RIGHTS device-fd handoff (session/seat_client.hpp): a forked
# stand-in ghostd sends a real descriptor, and the test reads through what
# create_device() returned. Needs control.pb.cc, hence the generated
# source and the protobuf link.
add_executable(wraith_seat_device_fd_test tests/seat_device_fd_test.cpp src/session/seat_client.cpp
  "${WRAITH_CONTROL_PROTO_GEN_DIR}/control.pb.cc")
target_include_directories(wraith_seat_device_fd_test PRIVATE src "${WRAITH_CONTROL_PROTO_GEN_DIR}")
target_link_libraries(wraith_seat_device_fd_test PRIVATE gdp protobuf::libprotobuf)
target_compile_options(wraith_seat_device_fd_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_seat_device_fd_test COMMAND wraith_seat_device_fd_test)

# wraith.sock's wire (session/report_server.hpp): a forked client against a
# real ReportServer. GdpSession is stood in for by the test itself.
add_executable(wraith_report_server_test tests/report_server_test.cpp src/session/report_server.cpp
  src/util/log.cpp "${WRAITH_CONTROL_PROTO_GEN_DIR}/wraith.pb.cc")
target_include_directories(wraith_report_server_test PRIVATE src "${WRAITH_CONTROL_PROTO_GEN_DIR}")
target_link_libraries(wraith_report_server_test PRIVATE gdp protobuf::libprotobuf PkgConfig::WAYLAND_SERVER)
target_compile_options(wraith_report_server_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_report_server_test COMMAND wraith_report_server_test)

# The bitrate controller (session/rate_controller.hpp) against synthetic
# reports. Links gdp for session.proto's NetworkProfile enum.
add_executable(wraith_rate_controller_test tests/rate_controller_test.cpp src/session/rate_controller.cpp)
target_include_directories(wraith_rate_controller_test PRIVATE src)
target_link_libraries(wraith_rate_controller_test PRIVATE gdp)
target_compile_options(wraith_rate_controller_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_rate_controller_test COMMAND wraith_rate_controller_test)

# The path rate estimator (session/path_rate_estimator.hpp) against
# synthetic frame trains.
add_executable(wraith_path_rate_estimator_test tests/path_rate_estimator_test.cpp src/session/path_rate_estimator.cpp)
target_include_directories(wraith_path_rate_estimator_test PRIVATE src)
target_compile_options(wraith_path_rate_estimator_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_path_rate_estimator_test COMMAND wraith_path_rate_estimator_test)

add_executable(wraith_session_profile_test tests/session_profile_test.cpp src/session/session_profile.cpp)
target_include_directories(wraith_session_profile_test PRIVATE src)
target_compile_options(wraith_session_profile_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_session_profile_test COMMAND wraith_session_profile_test)

add_executable(wraith_frame_hold_test tests/frame_hold_test.cpp src/screencast/frame_hold.cpp)
target_include_directories(wraith_frame_hold_test PRIVATE src)
target_compile_options(wraith_frame_hold_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_frame_hold_test COMMAND wraith_frame_hold_test)

# Lossless refinement's "which tiles go out losslessly this frame" policy
# (docs/design/refinement.md). Links gdp for gdp/refine.hpp's layer types, but
# no encoder backend -- the tracker is pure CPU logic over a
# framebuffer.
add_executable(wraith_tile_tracker_test tests/tile_tracker_test.cpp src/encode/refine/tile_tracker.cpp
  src/encode/refine/tile_source.cpp src/util/band_pool.cpp)
target_include_directories(wraith_tile_tracker_test PRIVATE src)
target_link_libraries(wraith_tile_tracker_test PRIVATE gdp)
target_compile_options(wraith_tile_tracker_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_tile_tracker_test COMMAND wraith_tile_tracker_test)

# RefineEncoder's pause (the client's Lossless switch) against a fake base
# encoder: the reset and empty layers around a pause and a resume.
add_executable(wraith_refine_encoder_test tests/refine_encoder_test.cpp src/encode/refine/refine_encoder.cpp
  src/encode/refine/tile_tracker.cpp src/encode/refine/tile_source.cpp src/util/band_pool.cpp src/util/config.cpp
  src/util/log.cpp)
target_include_directories(wraith_refine_encoder_test PRIVATE src)
target_link_libraries(wraith_refine_encoder_test PRIVATE gdp PkgConfig::TOMLPLUSPLUS)
target_compile_options(wraith_refine_encoder_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_refine_encoder_test COMMAND wraith_refine_encoder_test)

# Lossless refinement's GPU tile hashing (screencast/dmabuf_reader.cpp's
# shaders) bit for bit against the CPU hash and gather. Needs a render node
# with GLES 3.1; skipped without one.
add_executable(wraith_gpu_tile_hash_test tests/gpu_tile_hash_test.cpp src/screencast/dmabuf_reader.cpp
  src/screencast/dmabuf_tile_source.cpp src/encode/refine/tile_source.cpp src/util/band_pool.cpp
  src/util/log.cpp)
target_include_directories(wraith_gpu_tile_hash_test PRIVATE src)
target_link_libraries(wraith_gpu_tile_hash_test PRIVATE gdp PkgConfig::GBM PkgConfig::EGL PkgConfig::GLESV2)
target_compile_options(wraith_gpu_tile_hash_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_gpu_tile_hash_test COMMAND wraith_gpu_tile_hash_test)
set_tests_properties(wraith_gpu_tile_hash_test PROPERTIES SKIP_RETURN_CODE 77)

# PyroWave's dmabuf input against its CPU input: the same pixels must make
# the same bitstream, linear and tiled. Needs a GPU; skipped without one.
if(PYROWAVE_FOUND)
  add_executable(wraith_pyrowave_input_test tests/pyrowave_input_test.cpp src/encode/pyrowave/pyrowave_encoder.cpp
    src/util/log.cpp)
  target_include_directories(wraith_pyrowave_input_test PRIVATE src)
  target_compile_definitions(wraith_pyrowave_input_test PRIVATE WRAITH_HAVE_PYROWAVE)
  target_link_libraries(wraith_pyrowave_input_test PRIVATE gdp PkgConfig::PYROWAVE Vulkan::Vulkan PkgConfig::GBM
    PkgConfig::EGL PkgConfig::GLESV2)
  target_compile_options(wraith_pyrowave_input_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
  add_test(NAME wraith_pyrowave_input_test COMMAND wraith_pyrowave_input_test)
  set_tests_properties(wraith_pyrowave_input_test PROPERTIES SKIP_RETURN_CODE 77)
endif()

# wraith.toml (src/util/config.hpp): the shipped file at the defaults,
# and the rejection rules.
add_executable(wraith_config_test tests/config_test.cpp src/util/config.cpp)
target_include_directories(wraith_config_test PRIVATE src)
target_link_libraries(wraith_config_test PRIVATE PkgConfig::TOMLPLUSPLUS)
target_compile_definitions(wraith_config_test PRIVATE
  WRAITH_SHIPPED_CONFIG="${CMAKE_SOURCE_DIR}/packaging/config/wraith.toml")
target_compile_options(wraith_config_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_config_test COMMAND wraith_config_test)

add_executable(wraith_xrgb_convert_test tests/xrgb_convert_test.cpp src/encode/software/xrgb_convert.cpp
  src/util/band_pool.cpp)
target_include_directories(wraith_xrgb_convert_test PRIVATE src)
target_link_libraries(wraith_xrgb_convert_test PRIVATE Threads::Threads)
target_compile_options(wraith_xrgb_convert_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wraith_xrgb_convert_test COMMAND wraith_xrgb_convert_test)
