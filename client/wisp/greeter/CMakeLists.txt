# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Wisp's kiosk login screen: signs in to Veil (the one in the kernel command
# line, pinned to the certificate fingerprint next to it) with libgdp's
# gdp::LobbyClient and launches `spectre -K` as a child process, the way
# spectre-qt does on a desktop, with the settings Veil sent through
# wisp-agent (../agent). Runs fullscreen under labwc
# (client/wisp/image).
add_executable(wisp-greeter
  src/main.cpp
  src/agent_files.cpp
  src/agent_files.h
  src/boot_config.cpp
  src/boot_config.h
  src/display_sleep.cpp
  src/display_sleep.h
  src/greeter_window.cpp
  src/greeter_window.h
  resources/greeter.qrc
)
target_include_directories(wisp-greeter PRIVATE src)
target_link_libraries(wisp-greeter PRIVATE Qt6::Widgets gdp spectre_settings)
target_compile_options(wisp-greeter PRIVATE -Wall -Wextra -Wno-unused-parameter)
# spectre is launched from next to this binary, as spectre-qt does.
add_dependencies(wisp-greeter spectre)

install(TARGETS wisp-greeter RUNTIME DESTINATION bin)
