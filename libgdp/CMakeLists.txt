# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: MIT

# Windows and macOS take zstd and ngtcp2 from vcpkg (packaging/windows/
# and packaging/macos/ vcpkg.json); Linux takes them from distro packages.
if(WIN32 OR APPLE)
  set(GDP_DEPS_FROM_VCPKG ON)
else()
  set(GDP_DEPS_FROM_VCPKG OFF)
  find_package(PkgConfig REQUIRED)
endif()
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

# Zstd backs the lossless refinement layer's tiles
# (include/gdp/refine.hpp) -- shared here rather than duplicated in wraith
# and spectre so the compress and decompress sides can never drift.
# `apt install libzstd-dev` / vcpkg's zstd port.
if(GDP_DEPS_FROM_VCPKG)
  find_package(zstd CONFIG REQUIRED)
  set(GDP_ZSTD_TARGET zstd::libzstd)
else()
  pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
  set(GDP_ZSTD_TARGET PkgConfig::ZSTD)
endif()

# QUIC is ngtcp2 with its OpenSSL crypto helper (OpenSSL 3.5+'s own QUIC
# TLS API). `apt install libngtcp2-dev libngtcp2-crypto-ossl-dev` on
# Ubuntu; vcpkg's ngtcp2 port with its `openssl` feature elsewhere.
if(GDP_DEPS_FROM_VCPKG)
  find_package(ngtcp2 CONFIG REQUIRED)
  find_package(OpenSSL REQUIRED)
  # Static triplets (the macOS default) build the _static targets, dynamic
  # ones (x64-windows) the plain ones.
  if(TARGET ngtcp2::ngtcp2_static)
    set(GDP_QUIC_TARGETS ngtcp2::ngtcp2_crypto_ossl_static ngtcp2::ngtcp2_static)
  else()
    set(GDP_QUIC_TARGETS ngtcp2::ngtcp2_crypto_ossl ngtcp2::ngtcp2)
  endif()
  list(APPEND GDP_QUIC_TARGETS OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    # crypt32: the ROOT certificate store, for CaTrust::system (tls.cpp).
    list(APPEND GDP_QUIC_TARGETS ws2_32 crypt32)
  endif()
else()
  pkg_check_modules(NGTCP2 REQUIRED IMPORTED_TARGET libngtcp2 libngtcp2_crypto_ossl)
  find_package(OpenSSL 3.5 REQUIRED)
  set(GDP_QUIC_TARGETS PkgConfig::NGTCP2 OpenSSL::SSL OpenSSL::Crypto)
endif()

# Explicit protoc invocation rather than protobuf_generate_cpp(): the
# latter's output-path handling varies across CMake/Protobuf versions, and
# proto/ is also compiled by the host/ Rust build (host/ipc/build.rs), so
# the generated files must not land in the source tree.
# This mirrors wraith's wayland-scanner custom_command pattern.
set(GDP_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GDP_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${GDP_PROTO_GEN_DIR}")

set(GDP_PROTO_SRCS "")
foreach(proto_name lobby session wisp)
  set(proto_file "${GDP_PROTO_DIR}/${proto_name}.proto")
  set(gen_cc "${GDP_PROTO_GEN_DIR}/${proto_name}.pb.cc")
  set(gen_h "${GDP_PROTO_GEN_DIR}/${proto_name}.pb.h")
  add_custom_command(
    OUTPUT "${gen_cc}" "${gen_h}"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}" "--cpp_out=${GDP_PROTO_GEN_DIR}" "--proto_path=${GDP_PROTO_DIR}" "${proto_file}"
    DEPENDS "${proto_file}"
    COMMENT "Generating ${proto_name}.pb.{h,cc}"
  )
  list(APPEND GDP_PROTO_SRCS "${gen_cc}")
endforeach()
add_custom_target(gdp_proto_gen DEPENDS ${GDP_PROTO_SRCS})

add_library(gdp STATIC
  src/cert_fingerprint.cpp
  src/datagram.cpp
  src/framing.cpp
  src/clock.cpp
  src/error_codes.cpp
  src/negotiation.cpp
  src/clipboard.cpp
  src/video_codec.cpp
  src/refine.cpp
  src/video_reassembler.cpp
  src/client_connection.cpp
  src/lobby_client.cpp
  src/transport/net_loop.cpp
  src/transport/quic_conn.cpp
  src/transport/server_demux.cpp
  src/transport/sock_addr.cpp
  src/transport/tls.cpp
  src/transport/transport.cpp
  $<IF:$<BOOL:${WIN32}>,src/transport/udp_socket_win.cpp,src/transport/udp_socket_posix.cpp>
  ${GDP_PROTO_SRCS}
)
add_dependencies(gdp gdp_proto_gen)

target_include_directories(gdp
  PUBLIC
    include
    "${GDP_PROTO_GEN_DIR}" # generated lobby.pb.h / session.pb.h / wisp.pb.h, included as-is (no gdp/ prefix)
)

target_link_libraries(gdp
  PUBLIC
    protobuf::libprotobuf
  PRIVATE
    Threads::Threads
    ${GDP_ZSTD_TARGET}
    ${GDP_QUIC_TARGETS}
)
if(MSVC)
  target_compile_options(gdp PRIVATE /W4)
else()
  target_compile_options(gdp PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# --- Tests ---

enable_testing()

add_executable(gdp_datagram_test tests/datagram_test.cpp)
target_link_libraries(gdp_datagram_test PRIVATE gdp)
add_test(NAME gdp_datagram_test COMMAND gdp_datagram_test)

add_executable(gdp_cert_fingerprint_test tests/cert_fingerprint_test.cpp)
target_link_libraries(gdp_cert_fingerprint_test PRIVATE gdp)
add_test(NAME gdp_cert_fingerprint_test COMMAND gdp_cert_fingerprint_test)

add_executable(gdp_framing_test tests/framing_test.cpp)
target_link_libraries(gdp_framing_test PRIVATE gdp)
add_test(NAME gdp_framing_test COMMAND gdp_framing_test)

add_executable(gdp_negotiation_test tests/negotiation_test.cpp)
target_link_libraries(gdp_negotiation_test PRIVATE gdp)
add_test(NAME gdp_negotiation_test COMMAND gdp_negotiation_test)

add_executable(gdp_error_codes_test tests/error_codes_test.cpp)
target_link_libraries(gdp_error_codes_test PRIVATE gdp)
add_test(NAME gdp_error_codes_test COMMAND gdp_error_codes_test)

add_executable(gdp_clipboard_test tests/clipboard_test.cpp)
target_link_libraries(gdp_clipboard_test PRIVATE gdp)
add_test(NAME gdp_clipboard_test COMMAND gdp_clipboard_test)

add_executable(gdp_refine_test tests/refine_test.cpp)
target_link_libraries(gdp_refine_test PRIVATE gdp)
add_test(NAME gdp_refine_test COMMAND gdp_refine_test)

if(NOT WIN32)
  # POSIX only: the transport and CA tests shell out to the openssl CLI
  # and write under /tmp, and lobby_client_test needs a running ghostd.
  add_executable(gdp_transport_test tests/transport_test.cpp)
  target_link_libraries(gdp_transport_test PRIVATE gdp)
  add_test(NAME gdp_transport_test COMMAND gdp_transport_test)

  # Also the openssl CLI and /tmp: a private CA and leaves it signs.
  add_executable(gdp_ca_trust_test tests/ca_trust_test.cpp)
  target_link_libraries(gdp_ca_trust_test PRIVATE gdp OpenSSL::Crypto)
  add_test(NAME gdp_ca_trust_test COMMAND gdp_ca_trust_test)

  add_executable(gdp_stream_loss_test tests/stream_loss_test.cpp)
  target_link_libraries(gdp_stream_loss_test PRIVATE gdp Threads::Threads)
  add_test(NAME gdp_stream_loss_test COMMAND gdp_stream_loss_test)

  # Not add_test()'d: run by hand against a running ghostd (see the
  # file's header comment).
  add_executable(gdp_lobby_client_test tests/lobby_client_test.cpp)
  target_link_libraries(gdp_lobby_client_test PRIVATE gdp)
endif()

# --- Fuzzing (clang + libFuzzer only) ---

option(GDP_ENABLE_FUZZING "Build libgdp's libFuzzer targets (requires clang)" OFF)
if(GDP_ENABLE_FUZZING)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "GDP_ENABLE_FUZZING requires CXX=clang++ (libFuzzer is a clang runtime feature)")
  endif()
  # The parsers under test live in gdp itself, so it needs the coverage
  # and ASan instrumentation too -- otherwise libFuzzer is blind to them.
  target_compile_options(gdp PRIVATE -fsanitize=fuzzer-no-link,address)
  target_link_options(gdp INTERFACE -fsanitize=address)
  foreach(fuzzer datagram refine session_control server_demux)
    add_executable(gdp_${fuzzer}_fuzzer fuzz/${fuzzer}_fuzzer.cpp)
    target_link_libraries(gdp_${fuzzer}_fuzzer PRIVATE gdp)
    # server_demux drives the transport's internals directly.
    target_include_directories(gdp_${fuzzer}_fuzzer PRIVATE src/transport)
    target_link_libraries(gdp_${fuzzer}_fuzzer PRIVATE ${GDP_QUIC_TARGETS})
    target_compile_options(gdp_${fuzzer}_fuzzer PRIVATE -fsanitize=fuzzer,address)
    target_link_options(gdp_${fuzzer}_fuzzer PRIVATE -fsanitize=fuzzer,address)
  endforeach()
endif()

# --- Install (the libgdp-dev package) ---
#
# For anyone building their own GDP client against libgdp. Off by default
# so a plain `make install` of the host or client doesn't scatter headers
# and a static library into the prefix; the Debian packaging
# (debian/rules) turns it on. libgdp is static, so everything it links
# (protobuf, zstd, ngtcp2, OpenSSL) is needed again at the consumer's link
# step -- gdp.pc names those as Requires.
option(GHOST_INSTALL_LIBGDP_DEV "Install libgdp's headers, static library and pkg-config file" OFF)
if(GHOST_INSTALL_LIBGDP_DEV AND NOT GDP_DEPS_FROM_VCPKG)
  include(GNUInstallDirs)
  install(TARGETS gdp ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(DIRECTORY include/gdp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  # The generated protobuf headers are included as plain "lobby.pb.h"
  # (no gdp/ prefix), so they get their own directory that gdp.pc adds to
  # the include path rather than landing loose in <includedir>. The .proto
  # files are the wire schema for clients in other languages.
  install(FILES
    "${GDP_PROTO_GEN_DIR}/lobby.pb.h"
    "${GDP_PROTO_GEN_DIR}/session.pb.h"
    "${GDP_PROTO_GEN_DIR}/wisp.pb.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gdp/proto")
  install(DIRECTORY proto/ DESTINATION "${CMAKE_INSTALL_DATADIR}/gdp/proto" FILES_MATCHING PATTERN "*.proto")
  set(GDP_PC_VERSION "${PROJECT_VERSION}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/gdp.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/gdp.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gdp.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif()
