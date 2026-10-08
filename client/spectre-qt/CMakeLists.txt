# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# The login form (connect_window.cpp) speaks the lobby phase itself
# (gdp-spec.md §4) via libgdp's gdp::LobbyClient; the only thing it
# shares with the headless `spectre` binary is the binary itself, which it
# launches as a child process (see src/main.cpp's top comment).
add_executable(spectre-qt
  src/main.cpp
  src/connect_window.cpp
  src/connect_window.h
  src/connect_window.ui
  src/session_type_dialog.cpp
  src/session_type_dialog.h
  src/session_type_dialog.ui
  src/fixed_size.h
  src/known_hosts.cpp
  src/known_hosts.h
  src/settings_dialog.cpp
  src/settings_dialog.h
  src/settings_dialog.ui
  resources/app.qrc
)
if(APPLE)
  # spectre-qt.app. Finder, the Dock and Launchpad take a GUI app's name
  # and icon from its bundle; a bare executable gets neither.
  # packaging/macos/make_icns.cmake turns the same assets/app-icon.png the
  # other platforms use into the .icns.
  set(SPECTRE_QT_ICNS "${CMAKE_CURRENT_BINARY_DIR}/spectre.icns")
  add_custom_command(
    OUTPUT "${SPECTRE_QT_ICNS}"
    COMMAND "${CMAKE_COMMAND}"
      "-DINPUT=${CMAKE_SOURCE_DIR}/assets/app-icon.png" "-DOUTPUT=${SPECTRE_QT_ICNS}"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_SOURCE_DIR}/packaging/macos/make_icns.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/assets/app-icon.png" "${CMAKE_SOURCE_DIR}/packaging/macos/make_icns.cmake"
    COMMENT "Building spectre.icns"
  )
  set_source_files_properties("${SPECTRE_QT_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(spectre-qt PRIVATE "${SPECTRE_QT_ICNS}")
  set_target_properties(spectre-qt PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Spectre"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.ghost.spectre"
    MACOSX_BUNDLE_ICON_FILE "spectre.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1"
  )
  # find_spectre_binary() looks next to the launcher's own executable,
  # which inside a bundle is Contents/MacOS/ -- so that's where spectre
  # goes, in the build tree as in an install. (The launcher's executable
  # keeps its own name rather than taking the bundle's "Spectre": APFS is
  # case-insensitive by default, so it and spectre would be one file.)
  add_dependencies(spectre-qt spectre)
  add_custom_command(TARGET spectre-qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:spectre>" "$<TARGET_FILE_DIR:spectre-qt>/"
    COMMENT "Copying spectre into spectre-qt.app"
  )
endif()
if(WIN32)
  # /SUBSYSTEM:WINDOWS rather than the default console subsystem: without
  # this Windows gives the launcher a console window of its own, which sits
  # behind the login form for the life of the app. Nothing in spectre-qt
  # writes to stdout/stderr, so there's nothing to lose by dropping it.
  # main() stays the entry point -- Qt6::Core pulls in
  # Qt6::EntryPointPrivate, whose WinMain() forwards to it.
  set_target_properties(spectre-qt PROPERTIES WIN32_EXECUTABLE TRUE)

  # The .qrc icon only covers the *running* app's window icon; Explorer's
  # icon for the .exe file itself has to come from a linked-in Win32
  # resource (assets/app-icon.ico via resources/app.rc).
  target_sources(spectre-qt PRIVATE resources/app.rc)
endif()
target_include_directories(spectre-qt PRIVATE src)
target_link_libraries(spectre-qt PRIVATE Qt6::Widgets gdp spectre_settings)
if(MSVC)
  target_compile_options(spectre-qt PRIVATE /W4)
else()
  target_compile_options(spectre-qt PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Alongside `spectre` (client/spectre/CMakeLists.txt installs it to the same
# bin/), which find_spectre_binary() relies on. On macOS the bundle
# already carries spectre, and the whole .app is what's installed.
install(TARGETS spectre-qt
  RUNTIME DESTINATION bin
  BUNDLE DESTINATION .)

if(WIN32)
  # Qt loads qwindows.dll itself at runtime (it's a plugin, not something
  # linked in) from a platforms/ dir next to the exe -- VCPKG_APPLOCAL_DEPS
  # only copies *linked* DLL dependencies, so it never picks this up on its
  # own. Without it: "Could not find the Qt platform plugin 'windows'".
  add_custom_command(TARGET spectre-qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:spectre-qt>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
      "$<TARGET_FILE_DIR:spectre-qt>/platforms/"
    COMMENT "Deploying qwindows.dll platform plugin"
  )
endif()
