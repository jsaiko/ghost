# Installing Wisp

Wisp is a network-booted thin client that signs in to a Veil
([Wisp](../design/wisp.md), [Veil](veil.md)). There are two parts: a boot
server, a small container that serves the iPXE first stage over TFTP and
the boot script plus a per-architecture kernel, initrd and squashfs over
HTTP, and the client image it serves. Both are built from `client/wisp/`.

## Build the image

`make rootfs` (`DOCKER="sudo docker"` if needed) builds a Debian trixie
image from `image/Dockerfile`, with the repo root as context, and writes
`vmlinuz`, `initrd` and `wisp.squashfs` into `content/x86_64/`. The build
compiles spectre, `wisp-greeter` (`greeter/`) and `wisp-agent` (`agent/`) on
trixie, plus SDL 3.4
and ngtcp2 from source (trixie's are too old). `live-boot`'s initramfs
fetches the squashfs into RAM (`fetch=`, `ip=dhcp`) and overlays it.

`make rootfs ARCH=arm64` builds the arm64 image (UEFI machines) into
`content/arm64/`. The compile stage runs on the build machine's
own architecture and cross-compiles, so it takes about as long as a native
build; only the rootfs stage (package installs, initramfs) runs as arm64.
On an x86 host that needs QEMU user emulation registered with Docker, once
per boot of the build machine:

    docker run --privileged --rm tonistiigi/binfmt --install arm64

Docker Desktop has it built in. The same holds the other way round: an
ARM Mac builds the x86_64 image with `ARCH=x86_64`.

## Client options

Set on the boot server, they reach every client as kernel arguments.

**Time.** The image has no battery-backed clock it can trust (a PC's holds local time, or nothing), so `systemd-timesyncd` sets the time: from the NTP servers the
site's DHCP offers if any, else Debian's pool. To name the servers on the
boot server instead, set `WISP_NTP` (space or comma separated); it reaches
the client as `wisp_ntp=`, and `wisp-ntp.service` then configures timesyncd
with exactly those servers and turns off the NTP servers from DHCP, which
timesyncd would otherwise try first. Veil dates everything it
shows (boot time, session start) on its own clock from the uptime the agent
sends, so a client with a wrong clock still reports correctly.

`WISP_KERNEL_ARGS` adds whatever kernel arguments you give it to every client
(for example `nouveau.runpm=0`, a workaround for the nouveau driver failing
to wake a GPU whose display has slept), for trying driver or debugging
options without rebuilding the image. It takes effect at each client's next
boot.

`WISP_SHELL=1` on the boot server adds `wisp.shell` to the kernel command
line, which turns on a root autologin on tty2 (Ctrl+Alt+F2) and on the first
serial port. Debugging only: anyone at the keyboard, or on the serial line,
gets root. The serial login needs the kernel to name the port a console
(`console=ttyS0,115200`), which on real hardware can slow the boot to a
crawl, so `WISP_SERIAL_CONSOLE` picks who gets it: `qemu` (the default)
only QEMU guests, recognised by their SMBIOS manufacturer, `all` for a
machine with a real serial line, `off` for none. For the test VM, libvirt's
serial pty is that port: `virsh console <vm>`, or open the pty path in
`virsh dumpxml` directly.

Only x86_64 and arm64 images exist; the container serves `content/` via a
bind mount, so updating the image needs no container rebuild.

## Run the boot server

    make image
    cp .env.example .env    # then set VEIL_HOST
    veild wisp-env >> .env  # on the Veil host: VEIL_CERT_SHA256 and WISP_KEY
    docker compose up -d

`VEIL_HOST`, `VEIL_CERT_SHA256` and `WISP_KEY` are required; they land on the
kernel command line as `veil=...`, `veil_cert=...` and `wisp_key=...`. The
fingerprint is the clients' only trust root (no TOFU prompt on a kiosk): the
container refuses to start without it, and clients refuse a Veil that does
not match. Use a DNS name for `VEIL_HOST`: it is baked in at container start.
`veild wisp-env`, run on the Veil host as root, prints both of the other two:
the lobby certificate's fingerprint (not `veil-web-cert.pem`'s) and the key
in `/etc/ghost/veil-wisp.key`.

The key lets clients report to Veil (`wisp-agent`): Veil's admin UI lists
them under Thin Clients, and their spectre settings come from Veil's Thin
Client Settings page, applied at the next sign-in. It is one key for every
client, readable by anyone who can boot the image, so it only keeps out
machines that can't reach the boot network. To rotate it, delete the key file
on the Veil host, rerun `make install-veil`, restart veild, update `.env` and
restart this container; clients show as offline until their next boot.
Optional `HTTP_PORT` (default 80) and `TFTP_PORT` (default 69). The iPXE
binaries fetch a small `chain.ipxe` over TFTP that points at the HTTP port,
so changing `HTTP_PORT` needs no rebuild. DHCP cannot name a TFTP port and
firmware always uses 69, so a different `TFTP_PORT` is only useful behind a
port mapping or for testing.
To override content without rebuilding, bind-mount a directory over
`/srv/wisp/<arch>`.

## DHCP

Wisp runs no DHCP. On the site's server, point next-server at this host and
choose the filename by client architecture (option 93):

| Client                | Filename            |
|-----------------------|---------------------|
| x86_64 UEFI (7, 9)    | `ipxe-x86_64.efi`   |
| arm64 UEFI (11)       | `ipxe-arm64.efi`    |
| riscv64 UEFI (27)     | `ipxe-riscv64.efi`  |
| legacy BIOS (0)       | `undionly.kpxe`     |

For UEFI HTTP Boot (option 60 `HTTPClient`) give the URL
`http://<host>/<same filename>`; nginx serves the first-stage files too.

Boot flow: firmware -> TFTP iPXE -> HTTP `boot.ipxe` -> HTTP
`/<arch>/{vmlinuz,initrd,wisp.squashfs}`. Legacy BIOS iPXE reports arch
`i386`; the entrypoint aliases it to `x86_64`.
