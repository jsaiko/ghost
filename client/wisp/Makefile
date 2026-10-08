# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

# Wisp boot-server container and image (docs/design/wisp.md).
IMAGE     ?= wisp-boot
IPXE_REF  ?= v2.0.0
DOCKER    ?= docker

.PHONY: image rootfs run clean
image:
	$(DOCKER) build --build-arg IPXE_REF=$(IPXE_REF) -t $(IMAGE) .

# The Wisp rootfs: vmlinuz, initrd and wisp.squashfs into content/$(ARCH)/.
# Served from there by the container (bind mount in compose.yaml). The
# compile stage cross-compiles on the build machine's own architecture; the
# rootfs stage runs as the target, so building the other architecture needs
# QEMU user emulation registered with Docker (docs/install/wisp.md#build-the-image).
# The context is the repo root: the image compiles spectre, wisp-greeter and
# wisp-agent. The version is what the agent reports to Veil.
# The commit alone (--match with nothing to match ignores tags, which would
# name the build after whatever old tag is nearest).
WISP_VERSION ?= $(shell git describe --always --dirty --match '' 2>/dev/null || echo unknown) $(shell date -u +%Y-%m-%d)
ARCH ?= x86_64
PLATFORM_x86_64 = linux/amd64
PLATFORM_arm64  = linux/arm64
rootfs:
	@test -n "$(PLATFORM_$(ARCH))" || { echo "ARCH must be x86_64 or arm64" >&2; exit 1; }
	$(DOCKER) build -f image/Dockerfile --target out --platform $(PLATFORM_$(ARCH)) \
		--build-arg WISP_VERSION="$(WISP_VERSION)" --output type=local,dest=content/$(ARCH) ../..

# In the foreground, from compose.yaml and .env (the same settings, host
# networking and content mount as a deployed server). Host networking on
# purpose: docker-proxy hides client addresses and TFTP breaks behind NAT.
run:
	$(DOCKER) compose up

# The rootfs output, every architecture's; the tracked .gitkeep stays. Not
# the image or build cache, which Docker keeps outside the tree. The files
# are removable without sudo even when a sudo rootfs left them root's: the
# directories are the user's.
clean:
	find content -mindepth 2 -maxdepth 2 ! -name .gitkeep -exec rm -rf {} +
