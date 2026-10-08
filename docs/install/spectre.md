# Installing the client

spectre is the GDP client and spectre-qt its login launcher. Run
`spectre-qt`, enter a host (or a Veil) and sign in; it starts `spectre`
for the session. spectre-qt finds `spectre` next to its own binary.

## Linux

Build as described in the [README](../../README.md#build-and-test),
then `sudo make install` puts `spectre` and `spectre-qt` in
`$PREFIX/bin`, with a desktop entry and icon. The Debian package
`spectre` carries the same files and recommends `mesa-va-drivers` and
`mesa-vulkan-drivers`.

A GPU decodes the video: Vulkan Video where the GPU has a decode queue,
VA-API otherwise, and software as the last resort
([decoding](../design/spectre-client.md#decoding)). `spectre
--probe-decoders` lists what works on the machine. A client with no
usable GPU decodes in software and presents with lavapipe.

## macOS

The client builds on macOS with vcpkg and MoltenVK, and decodes with
VideoToolbox. See [packaging/macos/README.md](../../packaging/macos/README.md).

## Windows

The client builds with MSVC and vcpkg, and decodes with Vulkan Video or
D3D11VA. See [packaging/windows/README.md](../../packaging/windows/README.md).

## During a session

Left Ctrl + left Alt + left Super opens the session menu (Ctrl+Option+Cmd
on a Mac): fullscreen, resolution, lossless refinement, volume, mouse
capture, disconnect and ending the session. It works in fullscreen and
kiosk mode, and while it is up no input reaches the remote desktop. The
keys are the left-hand ones only; `-k` picks others, such as `-k ctrl+alt+super` for either
side ([command line](../reference/command-line.md)). The toolbar's Menu
button opens it too: the toolbar sits at the top of a window, and in
fullscreen slides down when the pointer reaches the top edge.

Ctrl+Shift+F11 toggles fullscreen and Scroll Lock captures or releases
the mouse (the remote gets its raw motion, for games and 3D views).

Lossless refinement is on by default: once part of the screen stops
changing, the host re-sends it pixel-exact, so text stays sharp. It
costs the host a little time every frame, so turn it off in the menu for
games where that matters. spectre remembers the choice.

## Trust

spectre-qt remembers each login server's certificate on first use in
`~/.config/spectre/known_hosts`; delete a host's line to be asked again.
A login server with a certificate a trusted CA issued for the name typed
needs neither a prompt nor a pin. The platform's CAs count, and so does
any CA in `~/.config/spectre/ca-certificates.pem` (a PEM bundle, for an
organization's own CA) ([trust](../design/trust.md)).

Per-user preferences (the view, mouse sensitivity) are in
`~/.local/share/spectre/spectre/spectre.conf`.
