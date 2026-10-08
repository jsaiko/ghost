# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

find_package(Qt6 REQUIRED COMPONENTS Gui)

# SpectreSettings and the one mapping from it to spectre(1) flags, shared by
# spectre-qt (which also loads and saves it, through QSettings) and
# wisp-greeter (which fills it from the profile Veil sends,
# docs/design/wisp.md).
add_library(spectre_settings STATIC
  spectre_settings.cpp
  spectre_settings.h
)
target_include_directories(spectre_settings PUBLIC .)
target_link_libraries(spectre_settings PUBLIC Qt6::Gui)
if(MSVC)
  target_compile_options(spectre_settings PRIVATE /W4)
else()
  target_compile_options(spectre_settings PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
