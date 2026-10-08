#!/bin/bash
# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only
# Builds and installs PyroWave's C API (libpyrowave-shared + pyrowave.pc),
# the optional dependency behind the "pyrowave" video codec
# (docs/design/encoding.md). Neither distro ships it, and its build
# needs a slice of Granite checked out next to it, so this pins both.
#
#   packaging/build-pyrowave.sh [PREFIX]     (default /usr/local; sudo for that)
#
# wraith and spectre pick it up through pkg-config at configure time (re-run
# `make configure` or distclean after installing it); without it they build
# without the codec.
set -euo pipefail

PYROWAVE_REPO=https://github.com/Themaister/pyrowave
# The commit the codec was brought up and measured against (2026-10-02). The
# bitstream is still a draft upstream, so both ends of a session must run
# the same one. The Windows client's vcpkg port
# (packaging/windows/ports/pyrowave) pins the same commit; change both.
PYROWAVE_COMMIT=89f7e47d4abbf650c91fae766728af866c5e32a0
PREFIX=${1:-/usr/local}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git clone --quiet "$PYROWAVE_REPO" "$work/pyrowave"
cd "$work/pyrowave"
git checkout --quiet "$PYROWAVE_COMMIT"
bash checkout_granite.sh >/dev/null
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build build
if [ "$(id -u)" -eq 0 ]; then
	as_root() { "$@"; }
	system=1
elif [ -w "$PREFIX" ]; then
	# A prefix we can write unprivileged is a staging tree (debian/rules
	# uses debian/_pyrowave), not a library directory the linker serves.
	as_root() { "$@"; }
	system=0
else
	as_root() { sudo "$@"; }
	system=1
fi
as_root cmake --install build
# The licence texts (both MIT), for anything that redistributes the library.
as_root install -D -m 644 LICENSE "$PREFIX/share/doc/pyrowave/LICENSE"
as_root install -D -m 644 Granite/LICENSE "$PREFIX/share/doc/pyrowave/LICENSE-Granite"
if [ "$system" -eq 1 ]; then
	as_root ldconfig
	# Arch's dynamic linker doesn't search /usr/local/lib (Debian's does), so
	# wraith and spectre wouldn't start. Register the library directory.
	if ! ldconfig -p | grep -q 'libpyrowave-shared\.so\.0 '; then
		libdir=$(dirname "$(find "$PREFIX" -name 'libpyrowave-shared.so.0' -print -quit)")
		echo "$libdir" | as_root tee /etc/ld.so.conf.d/pyrowave.conf >/dev/null
		as_root ldconfig
		echo "added $libdir to the dynamic linker's path (/etc/ld.so.conf.d/pyrowave.conf)"
	fi
fi
echo "pyrowave $PYROWAVE_COMMIT installed under $PREFIX"
