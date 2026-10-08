# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

# wisp-agent: the Wisp thin client's link to Veil
# (docs/design/wisp.md). A root system service that reports the
# client to Veil over `wisp/1` with libgdp's QUIC client, writes the spectre
# profile Veil sends to /run/wisp/profile.json for wisp-greeter, and passes
# on the sessions the greeter records in /run/wisp/session.json. No Qt.
add_executable(wisp-agent
  src/main.cpp
  src/boot_args.cpp
  src/boot_args.h
  src/system_info.cpp
  src/system_info.h
  src/wisp_client.cpp
  src/wisp_client.h
)
target_include_directories(wisp-agent PRIVATE src)
target_link_libraries(wisp-agent PRIVATE gdp)
target_compile_options(wisp-agent PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS wisp-agent RUNTIME DESTINATION bin)
