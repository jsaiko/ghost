# Wisp image: a Debian trixie rootfs that boots into wisp-greeter under labwc
# (a Wayland compositor, configured as a kiosk), which signs in to Veil and
# runs spectre. `make rootfs` (client/wisp/Makefile) builds the `out` stage with
# the repo root as context and drops vmlinuz, initrd and wisp.squashfs into
# content/x86_64/; `make rootfs ARCH=arm64` builds the same for arm64 (UEFI
# machines) into content/arm64/. Trixie, not bookworm: spectre needs OpenSSL
# 3.5+ and SDL3, which bookworm doesn't have.
#
# The build stage runs on the build machine's own architecture and
# cross-compiles for the target's, so an x86 host builds arm64 (and an ARM
# Mac x86_64) at native compiler speed. Only the rootfs stage, which installs
# packages and builds the initramfs, runs as the target, under QEMU emulation
# when the architectures differ.

# --- build: spectre, wisp-greeter and wisp-agent ---------------------------
FROM --platform=$BUILDPLATFORM debian:trixie-slim AS build
ARG BUILDARCH
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
# Tools that run during the build (compilers, protoc, glslangValidator,
# wayland-scanner, Qt's moc/rcc/uic) are the build machine's; libraries are
# the target's (Debian multiarch, :$TARGETARCH). Qt's host tools come from
# the build machine's own qt6-base-dev, which co-installs with the target's.
RUN if [ "$TARGETARCH" != "$BUILDARCH" ]; then dpkg --add-architecture "$TARGETARCH"; fi \
 && apt-get update \
 && if [ "$TARGETARCH" = "$BUILDARCH" ]; then cc=build-essential; else cc="crossbuild-essential-$TARGETARCH"; fi \
 && apt-get install -y --no-install-recommends \
        $cc make cmake ninja-build pkgconf git ca-certificates binutils \
        glslang-tools protobuf-compiler libwayland-bin wayland-protocols qt6-base-dev \
        $(for pkg in libssl-dev libavcodec-dev libavutil-dev libopus-dev \
                libva-dev libvulkan-dev libprotobuf-dev libzstd-dev qt6-base-dev \
                libwayland-dev libxkbcommon-dev libdecor-0-dev libasound2-dev \
                libdrm-dev libgbm-dev libegl-dev libgles-dev libudev-dev libdbus-1-dev; do \
            echo "$pkg:$TARGETARCH"; done) \
 && rm -rf /var/lib/apt/lists/*
# Every CMake configure below (and in build-pyrowave.sh) picks this up from
# the environment. Empty for a native build.
ENV CMAKE_TOOLCHAIN_FILE=/toolchain.cmake
# Libraries built from source install here, for the target.
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
RUN triplet_of() { case "$1" in amd64) echo x86_64-linux-gnu ;; arm64) echo aarch64-linux-gnu ;; esac; } \
 && triplet=$(triplet_of "$TARGETARCH") && host_triplet=$(triplet_of "$BUILDARCH") \
 && [ -n "$triplet" ] && [ -n "$host_triplet" ] \
 || { echo "unsupported architecture: $BUILDARCH -> $TARGETARCH" >&2; exit 1; } \
 && cpu=${triplet%%-*} \
 && echo "$triplet" > /triplet \
 && if [ "$TARGETARCH" = "$BUILDARCH" ]; then : > /toolchain.cmake; else \
        printf '%s\n' \
        'set(CMAKE_SYSTEM_NAME Linux)' \
        "set(CMAKE_SYSTEM_PROCESSOR $cpu)" \
        "set(CMAKE_C_COMPILER $triplet-gcc)" \
        "set(CMAKE_CXX_COMPILER $triplet-g++)" \
        "set(CMAKE_LIBRARY_ARCHITECTURE $triplet)" \
        "set(PKG_CONFIG_EXECUTABLE /usr/bin/$triplet-pkg-config)" \
        '# Run the build machine'"'"'s moc/rcc/uic, protoc and glslangValidator.' \
        'set(QT_HOST_PATH /usr)' \
        "set(QT_HOST_PATH_CMAKE_DIR /usr/lib/$host_triplet/cmake)" \
        'set(Protobuf_PROTOC_EXECUTABLE /usr/bin/protoc)' \
        > /toolchain.cmake; \
    fi \
 && cat /toolchain.cmake
# Trixie's ngtcp2 (1.11) predates the crypto helper for OpenSSL 3.5's own
# QUIC API (libngtcp2_crypto_ossl), so build it; static, so nothing to ship.
ARG NGTCP2_REF=v1.16.0
RUN git clone --depth 1 --branch "$NGTCP2_REF" https://github.com/ngtcp2/ngtcp2.git /ngtcp2 \
 && cmake -S /ngtcp2 -B /ngtcp2/build -DCMAKE_BUILD_TYPE=Release -DENABLE_LIB_ONLY=ON -DBUILD_TESTING=OFF \
        -DENABLE_OPENSSL=ON -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/usr/local \
 && cmake --build /ngtcp2/build -j"$(nproc)" && cmake --install /ngtcp2/build
# Trixie's SDL3 is 3.2; spectre uses 3.4 API. Wayland, KMS/DRM and ALSA
# only: there is no X server or sound server in the image.
ARG SDL_REF=release-3.4.2
RUN git clone --depth 1 --branch "$SDL_REF" https://github.com/libsdl-org/SDL.git /sdl \
 && cmake -S /sdl -B /sdl/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_X11=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
 && cmake --build /sdl/build -j"$(nproc)" && cmake --install /sdl/build
# PyroWave's C API, for spectre's "pyrowave" codec: the same script and pinned
# commit the desktop builds use (both ends of a session must run the same draft
# bitstream). Without it spectre builds without the codec and never offers it.
COPY packaging/build-pyrowave.sh /tmp/build-pyrowave.sh
RUN bash /tmp/build-pyrowave.sh /usr/local
COPY . /src
RUN cmake -S /src -B /build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DGHOST_BUILD_HOST=OFF -DGHOST_BUILD_LAUNCHER=OFF -DGHOST_BUILD_WISP=ON \
 && cmake --build /build -j"$(nproc)" --target spectre wisp-greeter wisp-agent \
 && mkdir -p /out/usr/local/bin /out/usr/local/lib \
 && cp /build/bin/spectre /build/bin/wisp-greeter /build/bin/wisp-agent /out/usr/local/bin/ \
 && cp -a /usr/local/lib/libSDL3.so.0* /usr/local/lib/libpyrowave-shared.so.0* /out/usr/local/lib/ \
 && mkdir -p /out/usr/local/share/doc/ghost \
 && cp /src/debian/copyright /out/usr/local/share/doc/ghost/copyright \
 && cp -a /usr/local/share/doc/pyrowave /out/usr/local/share/doc/ \
 && strip=strip && if [ -s /toolchain.cmake ]; then strip="$(cat /triplet)-strip"; fi \
 && $strip --strip-debug /out/usr/local/bin/* /out/usr/local/lib/*
# The runtime packages, from the libraries the binaries name (DT_NEEDED;
# ldd can't run a binary of another architecture). Their own dependencies
# come along with them in the rootfs stage.
RUN triplet=$(cat /triplet) \
 && readelf -d /out/usr/local/bin/* /out/usr/local/lib/*.so.* > /tmp/dynamic \
 && needed=$(sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' /tmp/dynamic | sort -u) \
 && [ -n "$needed" ] \
 && for so in $needed; do \
        [ -e "/out/usr/local/lib/$so" ] && continue; \
        path=$(ls "/usr/lib/$triplet/$so" "/lib/$triplet/$so" 2>/dev/null | head -1); \
        [ -n "$path" ] || { echo "no package has $so" >&2; exit 1; }; \
        dpkg -S "$path" 2>/dev/null || dpkg -S "$(realpath "$path")"; \
    done | cut -d: -f1 | sort -u > /out/runtime-packages \
 && [ -s /out/runtime-packages ] \
 && cat /out/runtime-packages

# --- rootfs ----------------------------------------------------------------
FROM debian:trixie-slim AS rootfs
ENV DEBIAN_FRONTEND=noninteractive
# Set by BuildKit from --platform: amd64 or arm64, Debian's names too.
ARG TARGETARCH
# non-free-firmware: AMD/Intel GPU firmware, without which amdgpu/i915 fall
# back to a dumb framebuffer, and NVIDIA's GSP firmware, without which nouveau
# fails to probe Turing and newer and leaves no DRM device at all.
RUN sed -i 's/^Components: main$/Components: main non-free-firmware/' /etc/apt/sources.list.d/debian.sources
# live-boot's initramfs fetches the squashfs (fetch=URL) into RAM and puts a
# writable overlay on top. MODULES=most keeps the initrd bootable on NICs we
# haven't seen yet. The PC GPU firmware only on amd64.
RUN case "$TARGETARCH" in \
        amd64) firmware="firmware-amd-graphics firmware-intel-graphics firmware-nvidia-graphics firmware-realtek" ;; \
        arm64) firmware="" ;; \
        *) echo "unsupported architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac \
 && apt-get update && apt-get install -y --no-install-recommends \
        initramfs-tools \
 && echo 'MODULES=most' >> /etc/initramfs-tools/initramfs.conf \
 && apt-get install -y --no-install-recommends \
        linux-image-$TARGETARCH live-boot \
        systemd systemd-sysv systemd-resolved systemd-timesyncd libpam-systemd udev dbus kmod polkitd \
        bash iproute2 iputils-ping curl ca-certificates less pciutils usbutils \
        $firmware \
 && rm -rf /var/lib/apt/lists/*
# The kiosk: labwc (not cage: games' mouselook needs pointer-constraints,
# which cage lacks; labwc's hard dependency on xwayland comes along, but it
# only starts if an X client connects), and what spectre and the greeter
# load at runtime beyond their direct links (Qt's Wayland plugin, SDL3's dlopen'd Wayland and ALSA,
# Mesa's GL/Vulkan/VA-API drivers incl. lavapipe for GPU-less clients; Intel's
# iHD VA-API driver on amd64, since mesa-va-drivers only has AMD and nouveau --
# without it an N100-class client has no hardware decode over VA-API).
# PipeWire in the wisp user's session gives the client a volume (spectre's
# kiosk menu, through wpctl) even on HDMI outputs without a hardware mixer;
# pipewire-alsa routes ALSA's default device, which SDL plays to, through it.
# swayidle and wlopm turn the display off after the profile's idle time
# (the greeter's display_sleep.cpp).
COPY --from=build /out/runtime-packages /tmp/runtime-packages
RUN apt-get update && apt-get install -y --no-install-recommends \
        labwc qt6-wayland fonts-dejavu-core swayidle wlopm \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libdecor-0-0 \
        libegl1 libegl-mesa0 libgl1-mesa-dri mesa-vulkan-drivers mesa-va-drivers \
        libasound2t64 alsa-ucm-conf pipewire pipewire-alsa wireplumber \
        $(if [ "$TARGETARCH" = amd64 ]; then echo intel-media-va-driver; fi) \
        $(cat /tmp/runtime-packages) \
 && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages
COPY --from=build /out/usr/local/ /usr/local/
RUN ldconfig && for bin in /usr/local/bin/spectre /usr/local/bin/wisp-greeter /usr/local/bin/wisp-agent; do \
        ! ldd "$bin" | grep 'not found' || exit 1; \
    done
COPY client/wisp/image/rootfs/ /
# The kiosk user: no password, no shell login; logind hands its session the
# seat (DRM, input, sound) while it is the active one on tty1.
RUN useradd --create-home --shell /usr/sbin/nologin --groups audio,video,render wisp \
 && systemctl enable wisp-hostname wisp-ntp wisp-agent wisp-kiosk systemd-networkd systemd-resolved systemd-timesyncd getty@tty2 \
 && systemctl set-default graphical.target \
 && systemctl mask systemd-networkd-wait-online \
 && update-initramfs -u \
 && rm -rf /var/cache/apt /var/log/*.log
# What wisp-agent reports to Veil as the image version (make rootfs sets
# it). Last, so a new version doesn't redo the layers above.
ARG WISP_VERSION=unknown
RUN echo "$WISP_VERSION" > /etc/wisp-release

# --- pack ------------------------------------------------------------------
# Only copies files and runs mksquashfs, so on the build machine's own
# architecture: emulated, compressing the squashfs takes minutes.
FROM --platform=$BUILDPLATFORM debian:trixie-slim AS pack
RUN apt-get update && apt-get install -y --no-install-recommends squashfs-tools \
 && rm -rf /var/lib/apt/lists/*
COPY --from=rootfs / /rootfs
# One kernel per image: the newest, should a package ever leave two.
RUN mkdir /out \
 && kernel=$(ls /rootfs/boot/vmlinuz-* | sort -V | tail -1) && version=${kernel#/rootfs/boot/vmlinuz-} \
 && cp "$kernel" /out/vmlinuz \
 && cp "/rootfs/boot/initrd.img-$version" /out/initrd \
 && chmod 644 /out/vmlinuz /out/initrd \
 && mksquashfs /rootfs /out/wisp.squashfs -comp zstd -noappend -quiet \
        -e rootfs/boot rootfs/proc rootfs/sys rootfs/dev rootfs/run rootfs/tmp

FROM scratch AS out
COPY --from=pack /out/ /
