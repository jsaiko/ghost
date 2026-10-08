# Wisp

PXE-booted thin client running spectre against Veil. This directory holds
the **boot server** (a container that serves the iPXE first stage over
TFTP and the boot files over HTTP) and the client image's sources.

- Installing it: [docs/install/wisp.md](../../docs/install/wisp.md).
- How it works: [docs/design/wisp.md](../../docs/design/wisp.md).

| Path | What it holds |
|---|---|
| `Dockerfile`, `compose.yaml`, `entrypoint.sh`, `nginx.conf`, `boot.ipxe.template`, `ipxe/` | The boot server |
| `Makefile` | `make image` (the container) and `make rootfs` (the client image) |
| `image/` | The client image: Dockerfile and root filesystem overlay |
| `greeter/` | `wisp-greeter`, the Qt sign-in kiosk |
| `agent/` | `wisp-agent`, which reports the client to Veil |
| `content/` | The built `vmlinuz`, `initrd` and `wisp.squashfs` per architecture |
