# Wisp boot server: TFTP serves the iPXE first stage, nginx serves
# everything else (docs/design/wisp.md). Build with `make image`.

# --- stage 1: iPXE first-stage binaries, one per architecture -------------
FROM debian:trixie-slim AS ipxe
ARG IPXE_REF=v2.0.0
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git make gcc libc6-dev perl xz-utils liblzma-dev \
        gcc-aarch64-linux-gnu gcc-riscv64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "$IPXE_REF" https://github.com/ipxe/ipxe.git /ipxe
COPY ipxe/embed.ipxe /ipxe/src/embed.ipxe
WORKDIR /ipxe/src
RUN make -j"$(nproc)" EMBED=embed.ipxe bin-x86_64-efi/ipxe.efi \
 && make -j"$(nproc)" EMBED=embed.ipxe bin/undionly.kpxe \
 && make -j"$(nproc)" EMBED=embed.ipxe CROSS=aarch64-linux-gnu- ARCH=arm64 \
        bin-arm64-efi/ipxe.efi \
 && make -j"$(nproc)" EMBED=embed.ipxe CROSS=riscv64-linux-gnu- ARCH=riscv64 \
        bin-riscv64-efi/ipxe.efi \
 && mkdir /out \
 && cp bin-x86_64-efi/ipxe.efi   /out/ipxe-x86_64.efi \
 && cp bin-arm64-efi/ipxe.efi    /out/ipxe-arm64.efi \
 && cp bin-riscv64-efi/ipxe.efi  /out/ipxe-riscv64.efi \
 && cp bin/undionly.kpxe         /out/undionly.kpxe

# --- stage 2: the server ---------------------------------------------------
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        nginx tftpd-hpa gettext-base wget \
    && rm -rf /var/lib/apt/lists/*

COPY --from=ipxe /out/ /srv/tftp/
COPY nginx.conf /etc/wisp/nginx.conf.template
COPY boot.ipxe.template /etc/wisp/boot.ipxe.template
COPY entrypoint.sh /usr/local/bin/wisp-boot-server
# One directory per architecture: vmlinuz, initrd, wisp.squashfs.
COPY content/ /srv/wisp/

ENV VEIL_HOST="" VEIL_CERT_SHA256="" WISP_KEY="" HTTP_PORT=80 TFTP_PORT=69
EXPOSE 69/udp 80/tcp
HEALTHCHECK --interval=30s --timeout=3s CMD wget -q -O /dev/null http://127.0.0.1:${HTTP_PORT}/boot.ipxe || exit 1
ENTRYPOINT ["/usr/local/bin/wisp-boot-server"]
