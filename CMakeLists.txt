# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 0.6.66 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimized with debug info unless asked otherwise: what gets installed on
# hosts must be fast, and stays debuggable (the Makefile's `make debug` is
# the unoptimized build).
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Install layout: every user-facing binary (wraith, ghostd, spectre,
# spectre-qt) goes to $PREFIX/bin, the systemd units to
# $PREFIX/lib/systemd/{system,user} (systemd searches those under both /usr
# and /usr/local), the desktop files to $PREFIX/share/applications, and the
# PAM service files to /etc/pam.d -- PAM has no prefix-relative search
# path, so that one is absolute and separately overridable via
# GHOST_PAM_DIR. Same story for the polkit rules file
# (GHOST_POLKIT_RULES_DIR, /etc/polkit-1/rules.d): polkit only ever reads
# that one system path, no prefix-relative equivalent.
# Override the prefix with -DCMAKE_INSTALL_PREFIX=... at configure time:
# ghostd, the units' ExecStart lines and the session profiles are built
# for it, so `cmake --install --prefix` with another prefix is refused.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "Install prefix" FORCE)
endif()
# First of the install rules (CMP0082 runs them in declaration order),
# so a refused install writes nothing.
install(CODE "
  if(NOT CMAKE_INSTALL_PREFIX STREQUAL \"${CMAKE_INSTALL_PREFIX}\")
    message(FATAL_ERROR \"configured for prefix ${CMAKE_INSTALL_PREFIX}, installing to \${CMAKE_INSTALL_PREFIX}: reconfigure with -DCMAKE_INSTALL_PREFIX=\${CMAKE_INSTALL_PREFIX}\")
  endif()
")
set(GHOST_PAM_DIR "/etc/pam.d" CACHE PATH "Where packaging/system/pam.d/ghostd is installed")
# The PAM services include the distro's own stacks, which are named
# differently: "debian" (common-auth, ...) or "system-auth" (Arch, Fedora).
# "auto" probes /etc/pam.d; packagers building elsewhere pass one explicitly.
# packaging/system/pam-render.sh expands the pam.d/*.in templates.
set(GHOST_PAM_STACK "auto" CACHE STRING "PAM stack the ghostd/ghostseat services include: auto, debian or system-auth")
set_property(CACHE GHOST_PAM_STACK PROPERTY STRINGS auto debian system-auth)
set(GHOST_POLKIT_RULES_DIR "/etc/polkit-1/rules.d" CACHE PATH "Where packaging/system/polkit/*.rules is installed")
# udev likewise reads fixed system paths. /usr/lib/udev/rules.d is where
# packages put rules (/etc/udev/rules.d is the admin's); ghostd looks in
# both before offering gamepad forwarding (host/ghostd/src/devices.rs).
set(GHOST_UDEV_RULES_DIR "/usr/lib/udev/rules.d" CACHE PATH "Where packaging/system/udev/*.rules is installed")
# Debian's pam-auth-update reads its profiles from this one path. Empty
# (the default wherever it doesn't exist) skips installing the profile;
# other distros add ghostlogin's pam_exec line to their display
# managers' stacks by hand (docs/design/login-and-sessions.md#one-graphical-login-per-user).
if(EXISTS "/usr/share/pam-configs")
  set(GHOST_PAM_CONFIGS_DEFAULT "/usr/share/pam-configs")
else()
  set(GHOST_PAM_CONFIGS_DEFAULT "")
endif()
set(GHOST_PAM_CONFIGS_DIR "${GHOST_PAM_CONFIGS_DEFAULT}" CACHE PATH "Where packaging/system/pam-configs/* is installed (pam-auth-update)")

enable_testing()

# Every executable in the tree lands in one bin/ directory. spectre-qt
# launches the `spectre` binary as a child process and locates it next to
# itself (client/spectre-qt/src/connect_window.cpp's find_spectre_binary()), so
# the build tree has to mirror what `cmake --install` produces. On Windows
# this also means Qt's platform-plugin deploy step and vcpkg's applocal
# DLL copies all target the same directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# The "pyrowave" codec needs libpyrowave-shared, which no distro ships
# (packaging/build-pyrowave.sh builds it into /usr/local); it is picked up
# automatically when installed. The Debian build (debian/rules) builds its own
# pinned copy and packages it as libpyrowave-shared0.
option(GHOST_ENABLE_PYROWAVE "Build the pyrowave codec when libpyrowave-shared is installed" ON)
# Arch's pkg-config doesn't search /usr/local (Debian's does), so add it
# after anything the caller set.
if(GHOST_ENABLE_PYROWAVE)
  set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig")
endif()

option(GHOST_ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
if(GHOST_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# The host side (host/: wraith, the Rust host agent, and everything installed into a
# Linux system: units, PAM, polkit, udev, desktop files) is Linux only.
# Windows and macOS build the client alone: libgdp, spectre and spectre-qt.
# A Linux client-only build (the Wisp image, client/wisp/image) turns it off.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(GHOST_BUILD_HOST "Build the host side (wraith, the Rust host agent)" ON)
else()
  set(GHOST_BUILD_HOST OFF)
endif()

add_subdirectory(libgdp)
if(GHOST_BUILD_HOST)
  add_subdirectory(host/wraith)
endif()
add_subdirectory(client/spectre)
# The launcher is the one part of the client that needs Qt; turning it off
# builds libgdp and spectre alone (a machine without Qt, or CI).
option(GHOST_BUILD_LAUNCHER "Build spectre-qt (needs Qt 6 Widgets)" ON)
# Wisp's kiosk login screen (client/wisp/greeter) and its link to Veil
# (client/wisp/agent); only the Wisp image builds them.
option(GHOST_BUILD_WISP "Build wisp-greeter (needs Qt 6 Widgets) and wisp-agent" OFF)
# spectre-qt and wisp-greeter share their settings-to-flags mapping.
if(GHOST_BUILD_LAUNCHER OR GHOST_BUILD_WISP)
  add_subdirectory(client/spectre-settings)
endif()
if(GHOST_BUILD_LAUNCHER)
  add_subdirectory(client/spectre-qt)
endif()
if(GHOST_BUILD_WISP)
  add_subdirectory(client/wisp/greeter)
  add_subdirectory(client/wisp/agent)
endif()

if(GHOST_BUILD_HOST)
  find_program(CARGO_EXECUTABLE cargo REQUIRED)

  # cargo's release profile (with debug info, host/Cargo.toml) for every
  # optimized build type; its dev profile only for Debug.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(GHOSTD_CARGO_PROFILE_FLAG "")
    set(GHOSTD_CARGO_PROFILE_DIR debug)
  else()
    set(GHOSTD_CARGO_PROFILE_FLAG --release)
    set(GHOSTD_CARGO_PROFILE_DIR release)
  endif()

  # One cargo workspace (host/Cargo.toml, ARCHITECTURE.md)
  # builds the Rust binaries: ghostd; ghostauth, the PAM helper systemd
  # starts per login; ghostseat, each session's root process, started the
  # same way; ghostlogin, the display managers' PAM account hook; and
  # veild, the broker. Output lands in <repo>/host/target/, distinct from
  # CMake's build/. veild is built here but never installed by this file:
  # a Veil machine gets the Makefile's install-veil instead
  # (docs/design/veil.md).
  add_custom_target(ghostd ALL
    COMMAND ${CMAKE_COMMAND} -E env GHOST_PREFIX=${CMAKE_INSTALL_PREFIX}
      ${CARGO_EXECUTABLE} build --workspace ${GHOSTD_CARGO_PROFILE_FLAG}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/host
    COMMENT "Building ghostd + ghostauth + ghostseat + ghostlogin + veild (Rust)"
    VERBATIM
  )
  # cargo owns the output path (<repo>/host/target/<profile>/*); install
  # picks it up from there rather than teaching cargo about CMake's tree.
  install(PROGRAMS "${CMAKE_SOURCE_DIR}/host/target/${GHOSTD_CARGO_PROFILE_DIR}/ghostd"
    DESTINATION bin)
  # lib/ghost, not bin: only systemd's socket units run ghostauth and
  # ghostseat (GHOST_LIBEXECDIR below), and only PAM's pam_exec runs
  # ghostlogin (packaging/system/pam-configs).
  foreach(helper ghostauth ghostseat ghostlogin)
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/host/target/${GHOSTD_CARGO_PROFILE_DIR}/${helper}"
      DESTINATION lib/ghost)
  endforeach()

  # Shipped session-type profiles, by id (sessions.d/<id>.conf.in).
  set(GHOST_SESSION_TYPES terminal plasma steam gnome lxqt)

  # The units' ExecStart must name the installed binaries, and the session
  # profiles' Exec= paths and arguments the installed launchers, so the
  # templates are expanded here with the configured prefix. ghostd bakes
  # the same prefix in at build time (GHOST_PREFIX above), so installing
  # under another one would break it either way: the check at the top refuses.
  set(GHOST_BINDIR "${CMAKE_INSTALL_PREFIX}/bin")
  set(GHOST_LIBEXECDIR "${CMAKE_INSTALL_PREFIX}/lib/ghost")
  set(GHOST_DATADIR "${CMAKE_INSTALL_PREFIX}/share/ghost")
  configure_file("${CMAKE_SOURCE_DIR}/packaging/system/systemd/wraith.service.in"
    "${CMAKE_BINARY_DIR}/packaging/wraith.service" @ONLY)
  foreach(type ${GHOST_SESSION_TYPES})
    configure_file("${CMAKE_SOURCE_DIR}/packaging/ghost/sessions.d/${type}.conf.in"
      "${CMAKE_BINARY_DIR}/packaging/ghost/sessions.d/${type}.conf" @ONLY)
  endforeach()
  configure_file("${CMAKE_SOURCE_DIR}/packaging/desktop/wraith.desktop.in"
    "${CMAKE_BINARY_DIR}/packaging/desktop/wraith.desktop" @ONLY)
  configure_file("${CMAKE_SOURCE_DIR}/packaging/system/systemd/ghostd.service.in"
    "${CMAKE_BINARY_DIR}/packaging/ghostd.service" @ONLY)
  configure_file("${CMAKE_SOURCE_DIR}/packaging/system/systemd/ghostauth@.service.in"
    "${CMAKE_BINARY_DIR}/packaging/ghostauth@.service" @ONLY)
  configure_file("${CMAKE_SOURCE_DIR}/packaging/system/systemd/ghostseat@.service.in"
    "${CMAKE_BINARY_DIR}/packaging/ghostseat@.service" @ONLY)
  configure_file("${CMAKE_SOURCE_DIR}/packaging/desktop/spectre-qt.desktop.in"
    "${CMAKE_BINARY_DIR}/packaging/desktop/spectre-qt.desktop" @ONLY)
  configure_file("${CMAKE_SOURCE_DIR}/packaging/system/pam-configs/ghostlogin.in"
    "${CMAKE_BINARY_DIR}/packaging/ghostlogin" @ONLY)
  install(FILES "${CMAKE_BINARY_DIR}/packaging/wraith.service" DESTINATION lib/systemd/user)
  # ghostd runs as the `ghost` user; the ghostauth and ghostseat socket
  # units start a root-or-helper instance per login (docs/design/
  # login-and-sessions.md). The sysusers and tmpfiles fragments create
  # the accounts and /run/ghost; the Makefile's install target applies
  # them on this machine, a package's maintainer scripts on theirs.
  install(FILES "${CMAKE_BINARY_DIR}/packaging/ghostd.service"
    "${CMAKE_BINARY_DIR}/packaging/ghostauth@.service"
    "${CMAKE_BINARY_DIR}/packaging/ghostseat@.service"
    "${CMAKE_SOURCE_DIR}/packaging/system/systemd/ghostauth.socket"
    "${CMAKE_SOURCE_DIR}/packaging/system/systemd/ghostseat.socket"
    DESTINATION lib/systemd/system)
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/system/sysusers.d/ghostd.conf"
    "${CMAKE_SOURCE_DIR}/packaging/system/sysusers.d/ghostauth.conf"
    DESTINATION lib/sysusers.d)
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/system/tmpfiles.d/ghost.conf"
    DESTINATION lib/tmpfiles.d)

  # ghostauth (host/ghostauth) opens the "ghostd" PAM service for ghostd's
  # lobby; ghostseat's session-only stack (host/ghostseat/src/pam_session.rs)
  # opens "ghostseat" -- both only exist once these are in /etc/pam.d.
  foreach(svc ghostd ghostseat)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/packaging")
    execute_process(
      COMMAND "${CMAKE_SOURCE_DIR}/packaging/system/pam-render.sh" "${GHOST_PAM_STACK}"
              "${CMAKE_SOURCE_DIR}/packaging/system/pam.d/${svc}.in" "${CMAKE_BINARY_DIR}/packaging/${svc}.pam"
      RESULT_VARIABLE pam_rc)
    if(NOT pam_rc EQUAL 0)
      message(FATAL_ERROR "cannot render the ${svc} PAM service -- set -DGHOST_PAM_STACK=debian or system-auth")
    endif()
    install(FILES "${CMAKE_BINARY_DIR}/packaging/${svc}.pam" DESTINATION "${GHOST_PAM_DIR}" RENAME "${svc}")
  endforeach()
  # The display managers' account hook (docs/design/login-and-sessions.md#one-graphical-login-per-user):
  # a console login ends the same user's ghost session. Only installed
  # here; the Makefile's install target runs pam-auth-update to add it to
  # common-account.
  if(GHOST_PAM_CONFIGS_DIR)
    install(FILES "${CMAKE_BINARY_DIR}/packaging/ghostlogin" DESTINATION "${GHOST_PAM_CONFIGS_DIR}")
  endif()

  # Denies org.freedesktop.NetworkManager.network-control for ghostseat-opened
  # sessions instead of letting it fall through to polkit's allow_any default
  # (auth_admin) -- see the rule file's own comment for why (a ghost session
  # is always seatless + Remote=yes, and network-control has no per-interface
  # scoping to grant safely). Without this, plasmashell's NetworkManager
  # applet pops an authentication dialog nobody in the session can answer.
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/system/polkit/49-ghost-sessions.rules"
    DESTINATION "${GHOST_POLKIT_RULES_DIR}")

  # Keeps the virtual gamepads ghostseat creates for sessions off the
  # console seat (docs/design/audio-cursor-gamepad.md#gamepads; see the rule's
  # own comment for the default behaviour it corrects). ghostseat refuses
  # to create devices at all when this isn't installed, so a missing rule
  # degrades to "no gamepads" rather than to pads leaking to the console user.
  # udev only picks it up after `udevadm control --reload` (the Makefile's
  # install target does that) or a reboot.
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/system/udev/72-ghost-input.rules"
    DESTINATION "${GHOST_UDEV_RULES_DIR}")

  # Session-type hosting profiles (docs/reference/session-profiles.md): shipped
  # defaults in share/ghost/sessions.d, overridable per filename stem by an
  # admin's /etc/ghost/sessions.d (host/ghostd/src/profiles.rs, wraith's
  # session_profile.cpp). The launchers below are the Exec= targets the
  # profiles point at; GHOST_LIBEXECDIR above is where they land.
  list(TRANSFORM GHOST_SESSION_TYPES
    REPLACE "(.+)" "${CMAKE_BINARY_DIR}/packaging/ghost/sessions.d/\\1.conf"
    OUTPUT_VARIABLE GHOST_SESSION_PROFILES)
  install(FILES ${GHOST_SESSION_PROFILES} DESTINATION share/ghost/sessions.d)
  install(PROGRAMS "${CMAKE_SOURCE_DIR}/packaging/ghost/sessions/labwc-screencast"
    "${CMAKE_SOURCE_DIR}/packaging/ghost/sessions/gnome-screencast"
    "${CMAKE_SOURCE_DIR}/packaging/ghost/sessions/plasma-screencast"
    "${CMAKE_SOURCE_DIR}/packaging/ghost/sessions/lxqt-screencast"
    DESTINATION lib/ghost/sessions)
  # labwc-screencast's per-session-type labwc config dirs (labwc -C).
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/packaging/ghost/labwc/" DESTINATION share/ghost/labwc)
  # Every user's kwin gets the extra arguments plasma-screencast sets
  # (docs/design/capture-backends.md): --virtual inside a ghost session,
  # none otherwise. GNOME needs none of this: mutter goes headless by
  # itself in a seatless logind session (packaging/ghost/sessions/gnome-screencast).
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/system/systemd/plasma-kwin_wayland.service.d/50-ghost.conf"
    DESTINATION lib/systemd/user/plasma-kwin_wayland.service.d)

  # wraith's own desktop file (docs/design/capture-backends.md):
  # not a launchable application (NoDisplay=true) -- its only purpose is
  # X-KDE-Wayland-Interfaces=, which is what makes kwin trust wraith with
  # zkde_screencast_unstable_v1 (and the output-management pair that sizes
  # its virtual output) for screencast-kwin. KWin canonicalizes
  # Exec='s path against the connecting client's /proc/<pid>/exe, so this
  # must name the same wraith the session runs -- @GHOST_BINDIR@/wraith,
  # i.e. the installed ${CMAKE_INSTALL_PREFIX}/bin/wraith the units also
  # launch. (A symlinked install still matches, since both sides are
  # canonicalized to the real file.)
  install(FILES "${CMAKE_BINARY_DIR}/packaging/desktop/wraith.desktop" DESTINATION share/applications)

  # Desktop launcher for spectre-qt (freedesktop.org Desktop Entry spec),
  # so it shows up in a session's application menu; templated for the same
  # reason as the units above -- Exec= must name the installed binary.
  install(FILES "${CMAKE_BINARY_DIR}/packaging/desktop/spectre-qt.desktop" DESTINATION share/applications)

  # The icon both ways: KWin's window decoration only looks up a hicolor
  # theme entry, Plasma's taskbar falls back to share/pixmaps.
  # gtk-update-icon-cache refreshes hicolor's cache so the entry is seen
  # without a session restart.
  install(FILES "${CMAKE_SOURCE_DIR}/assets/app-icon.png"
    DESTINATION share/icons/hicolor/256x256/apps RENAME spectre-qt.png)
  install(FILES "${CMAKE_SOURCE_DIR}/assets/app-icon.png" DESTINATION share/pixmaps RENAME spectre-qt.png)
  install(CODE "
    if(NOT DEFINED ENV{DESTDIR})
      find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)
      if(GTK_UPDATE_ICON_CACHE)
        execute_process(COMMAND \"\${GTK_UPDATE_ICON_CACHE}\" -f -t \"${CMAKE_INSTALL_PREFIX}/share/icons/hicolor\")
      endif()
    endif()
  ")

  # Pristine reference copies of ghostd's and wraith's configs. The live
  # ones, /etc/ghost/ghostd.toml and wraith.toml, are the Makefile's config
  # target: put down once and never overwritten, like the host certificate.
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/config/ghostd.toml" "${CMAKE_SOURCE_DIR}/packaging/config/wraith.toml"
    DESTINATION share/doc/ghost)
endif()
