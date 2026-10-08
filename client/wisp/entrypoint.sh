#!/bin/sh
# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only
# Wisp boot server entrypoint: render boot.ipxe, then run the TFTP daemon
# (first stage) and nginx (everything else) until either exits.
set -eu

: "${VEIL_HOST:?set VEIL_HOST to the Veil broker address (host or host:port)}"
case "$VEIL_HOST" in
    *[!A-Za-z0-9.:_\[\]-]*) echo "wisp: VEIL_HOST has unexpected characters: $VEIL_HOST" >&2; exit 1 ;;
esac

# Veil's certificate fingerprint is the clients' only trust root, and a
# kiosk has nobody to answer a TOFU prompt: refuse to serve without it.
: "${VEIL_CERT_SHA256:?set VEIL_CERT_SHA256 to the SHA-256 fingerprint of Veil's certificate}"
VEIL_CERT_SHA256=$(printf %s "$VEIL_CERT_SHA256" | tr -d ': ' | tr 'A-F' 'a-f')
case "$VEIL_CERT_SHA256" in
    *[!0-9a-f]*|"") echo "wisp: VEIL_CERT_SHA256 is not hex" >&2; exit 1 ;;
esac
[ "${#VEIL_CERT_SHA256}" -eq 64 ] || { echo "wisp: VEIL_CERT_SHA256 must be 64 hex digits" >&2; exit 1; }

# The key clients present to Veil to report in (docs/design/wisp.md).
: "${WISP_KEY:?set WISP_KEY to the Wisp key (on the Veil host: veild wisp-env)}"
WISP_KEY=$(printf %s "$WISP_KEY" | tr 'A-F' 'a-f')
case "$WISP_KEY" in
    *[!0-9a-f]*|"") echo "wisp: WISP_KEY is not hex" >&2; exit 1 ;;
esac
[ "${#WISP_KEY}" -eq 64 ] || { echo "wisp: WISP_KEY must be 64 hex digits" >&2; exit 1; }

# /srv/http is rebuilt each start from the baked-in content (/srv/wisp) so
# an architecture directory bind-mounted over /srv/wisp/<arch> wins.
rm -rf /srv/http
mkdir -p /srv/http
for d in /srv/wisp/*/; do
    [ -d "$d" ] && ln -s "${d%/}" "/srv/http/$(basename "$d")"
done
# UEFI HTTP Boot fetches the same first-stage files over HTTP.
for f in /srv/tftp/*; do ln -s "$f" "/srv/http/$(basename "$f")"; done
# Legacy BIOS iPXE reports buildarch i386; the x86_64 kernel boots from it.
[ -e /srv/http/i386 ] || { [ -e /srv/http/x86_64 ] && ln -s x86_64 /srv/http/i386; } || true

for p in "${HTTP_PORT:=80}" "${TFTP_PORT:=69}"; do
    case "$p" in ''|*[!0-9]*) echo "wisp: bad port: $p" >&2; exit 1 ;; esac
done

# Stage 1.5, fetched over TFTP by the iPXE binary: on to HTTP at our port.
printf '#!ipxe\nchain http://${next-server}:%s/boot.ipxe\n' "$HTTP_PORT" > /srv/tftp/chain.ipxe
sed "s|@HTTP_PORT@|$HTTP_PORT|g" /etc/wisp/nginx.conf.template > /run/nginx.conf

# WISP_SHELL=1 gives clients a root shell on tty2 and the serial console
# (debugging only: anyone at the keyboard gets root). A login on the serial
# port needs the kernel to name it a console (console=ttyS0, which systemd's
# getty generator looks for), and that console is slow and can stall a boot
# on real hardware, so WISP_SERIAL_CONSOLE says where it goes: "qemu" (the
# default) only to QEMU guests, which boot.ipxe.template tells apart by their
# SMBIOS manufacturer; "all" to every client, for a real serial line; "off".
extra=""
qemu_console=""
if [ "${WISP_SHELL:-0}" = 1 ]; then
    extra="wisp.shell"
    case "${WISP_SERIAL_CONSOLE:-qemu}" in
        qemu) qemu_console="console=ttyS0,115200" ;;
        all) extra="$extra console=ttyS0,115200" ;;
        off) ;;
        *) echo "wisp: WISP_SERIAL_CONSOLE must be qemu, all or off" >&2; exit 1 ;;
    esac
fi

# WISP_NTP: servers separated by spaces or commas, passed as wisp_ntp=a,b
# (a kernel argument can't hold spaces).
if [ -n "${WISP_NTP:-}" ]; then
    ntp=$(printf %s "$WISP_NTP" | tr -s ' ,' ',,' | sed 's/^,//; s/,$//')
    case "$ntp" in
        *[!A-Za-z0-9.:_,\[\]-]*|"") echo "wisp: WISP_NTP has unexpected characters: $WISP_NTP" >&2; exit 1 ;;
    esac
    extra="$extra wisp_ntp=$ntp"
fi

# WISP_KERNEL_ARGS: extra kernel arguments for every client, as given
# (e.g. "nouveau.runpm=0"). The line they land on is an iPXE script, so
# what iPXE or sed would treat as syntax is refused rather than escaped.
if [ -n "${WISP_KERNEL_ARGS:-}" ]; then
    case "$WISP_KERNEL_ARGS" in
        *[!A-Za-z0-9._=,:/+@\ -]*) echo "wisp: WISP_KERNEL_ARGS may only contain letters, digits and . _ = , : / + @ - and spaces: $WISP_KERNEL_ARGS" >&2; exit 1 ;;
    esac
    extra="$extra $WISP_KERNEL_ARGS"
fi

sed -e "s|@VEIL_HOST@|$VEIL_HOST|g" -e "s|@VEIL_CERT_SHA256@|$VEIL_CERT_SHA256|g" -e "s|@WISP_KEY@|$WISP_KEY|g" -e "s|@QEMU_CONSOLE@|$qemu_console|g" -e "s|@EXTRA@|$extra|g" /etc/wisp/boot.ipxe.template > /srv/http/boot.ipxe

for arch in /srv/http/*/; do
    [ -e "$arch/vmlinuz" ] || echo "wisp: warning: ${arch} has no vmlinuz (no Wisp build there yet)" >&2
done

in.tftpd -L -s -a "[::]:$TFTP_PORT" /srv/tftp &
tftp=$!
nginx -c /run/nginx.conf &
web=$!
trap 'kill $tftp $web 2>/dev/null' TERM INT
# wait -n is not POSIX sh; poll instead.
while kill -0 "$tftp" 2>/dev/null && kill -0 "$web" 2>/dev/null; do
    sleep 1 & wait $! || true
done
echo "wisp: a server process exited, stopping" >&2
kill "$tftp" "$web" 2>/dev/null || true
exit 1
