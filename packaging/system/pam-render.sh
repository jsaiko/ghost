#!/bin/sh
# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only
#
# Expand a PAM service template (pam.d/*.in) for this distro's stacks.
#
#   pam-render.sh FLAVOUR TEMPLATE OUTPUT
#
# FLAVOUR names the stack files the services include:
#   debian       common-auth / common-account / common-session
#                (Debian, Ubuntu, SUSE)
#   system-auth  system-auth, and system-login for the session when it
#                exists (Arch has it, and pam_systemd is there); Fedora and
#                RHEL put pam_systemd in system-auth itself
#   auto         whichever of the two this machine has in /etc/pam.d
#
# The templates' @PAM_AUTH@, @PAM_ACCOUNT@ and @PAM_SESSION@ become those
# names. Nothing is guessed silently: an unknown flavour, or "auto" on a
# machine with neither, fails.
set -eu

flavour=$1 in=$2 out=$3
pamd=${GHOST_PAM_PROBE_DIR:-/etc/pam.d}

if [ "$flavour" = auto ]; then
	if [ -e "$pamd/common-auth" ]; then
		flavour=debian
	elif [ -e "$pamd/system-auth" ]; then
		flavour=system-auth
	else
		echo "pam-render: neither common-auth nor system-auth in $pamd -- pass the stack explicitly (cmake -DGHOST_PAM_STACK=debian|system-auth)" >&2
		exit 1
	fi
fi

case $flavour in
debian)
	auth=common-auth account=common-account session=common-session ;;
system-auth)
	auth=system-auth account=system-auth session=system-auth
	[ -e "$pamd/system-login" ] && session=system-login ;;
*)
	echo "pam-render: unknown PAM stack '$flavour' (debian, system-auth or auto)" >&2
	exit 1 ;;
esac

sed -e "s|@PAM_AUTH@|$auth|g" -e "s|@PAM_ACCOUNT@|$account|g" \
	-e "s|@PAM_SESSION@|$session|g" "$in" > "$out"
