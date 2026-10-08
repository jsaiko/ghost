# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

# Thin wrapper so `make` at the repo root drives the CMake build
# (libgdp, wraith, spectre, spectre-qt, the host/ Rust workspace)
# without typing the cmake invocation by hand.

# The default build is optimized with debug info (-O2 -g; cargo's release
# profile, with symbols): what the install targets put on a host, and
# still debuggable. `make debug` builds unoptimized into build-debug/.
BUILD_DIR           ?= build
BUILD_TYPE          ?= RelWithDebInfo
CMAKE               ?= cmake
JOBS                ?= $(shell nproc 2>/dev/null || echo 4)
# Install prefix: binaries land in $(PREFIX)/bin, the systemd units in
# $(PREFIX)/lib/systemd, the PAM service files in /etc/pam.d regardless
# (see the top-level CMakeLists.txt). Fixed at configure time, so pass it
# to `make` as well as `make install`. DESTDIR is honoured for staging.
PREFIX              ?= /usr/local
# Where `make install` generates the host's certificate and key and puts
# ghostd.toml and wraith.toml, each only if absent (the cert and config
# targets). ghostd reads /etc/ghost/ghostd.toml unless run with -f, and
# wraith /etc/ghost/wraith.toml unless run with -C, so only change this for
# a staged install that also sets those.
GHOST_CERT_DIR      ?= /etc/ghost
# The distro stack the PAM services (ghostd, ghostseat, veild) include:
# auto (probe /etc/pam.d), debian (common-*) or system-auth (Arch, Fedora).
# CMake renders ghostd's and ghostseat's at configure time, so changing it
# for an existing build dir needs `make reconfigure`.
# See packaging/system/pam-render.sh.
PAM_STACK           ?= auto

.PHONY: all build configure reconfigure test clean distclean debug install uninstall reload cert config authkey accounts install-veil uninstall-veil deb

all: build

CONFIGURE_FLAGS = -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_INSTALL_PREFIX=$(PREFIX) -DGHOST_PAM_STACK=$(PAM_STACK)

# A build dir configured with another build type or prefix (`make
# PREFIX=/usr` over a /usr/local build, say) is reconfigured rather than
# silently kept: the prefix is baked
# into ghostd and the units at configure time, and `cmake --install`
# refuses any other.
configure: $(BUILD_DIR)/CMakeCache.txt
	@grep -q '^CMAKE_BUILD_TYPE:STRING=$(BUILD_TYPE)$$' $(BUILD_DIR)/CMakeCache.txt && \
	 grep -q '^CMAKE_INSTALL_PREFIX:PATH=$(PREFIX)$$' $(BUILD_DIR)/CMakeCache.txt || \
		$(CMAKE) -S . -B $(BUILD_DIR) $(CONFIGURE_FLAGS)

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CONFIGURE_FLAGS)

reconfigure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DGHOST_PAM_STACK=$(PAM_STACK)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

debug:
	$(MAKE) build BUILD_DIR=build-debug BUILD_TYPE=Debug

# Deliberately does not depend on `build`: the ghostd custom target (top-level
# CMakeLists.txt) reruns `cargo build --workspace` on every `cmake --build`
# invocation regardless of staleness, and `sudo make install` running that as
# root would leave root-owned files in the user-owned build/ and
# host/target/ trees, breaking later unprivileged builds. Build first with
# a plain `make build`, then install (with sudo if $(PREFIX) needs it) --
# `cmake --install` alone never touches the build graph.
install:
	@test -f $(BUILD_DIR)/CMakeCache.txt || { echo "error: $(BUILD_DIR) is not configured/built yet -- run 'make build' first (as your normal user, not root)" >&2; exit 1; }
	$(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX)
	@$(MAKE) --no-print-directory accounts
	@$(MAKE) --no-print-directory cert
	@$(MAKE) --no-print-directory authkey
	@$(MAKE) --no-print-directory config
	@$(MAKE) --no-print-directory reload
	@# The gamepad seat rule (packaging/system/udev/72-ghost-input.rules) is only
	@# read by udevd after a reload; without this it takes a reboot, and
	@# ghostd would offer gamepads that then leak to the console seat.
	@if [ -z "$(DESTDIR)" ] && command -v udevadm >/dev/null 2>&1; then \
		udevadm control --reload || echo "warning: udevadm control --reload failed -- gamepad rule takes effect at next boot" >&2; \
	fi
	@# The ghostlogin hook (packaging/system/pam-configs) only takes effect once
	@# pam-auth-update writes it into common-account. Elsewhere its pam_exec
	@# line goes into the display managers' stacks by hand.
	@if [ -z "$(DESTDIR)" ] && [ -f /usr/share/pam-configs/ghostlogin ] && command -v pam-auth-update >/dev/null 2>&1; then \
		echo "-- enabling the ghostlogin PAM hook (pam-auth-update)..."; \
		DEBIAN_FRONTEND=noninteractive pam-auth-update --package || echo "warning: pam-auth-update failed -- run 'pam-auth-update --enable ghostlogin'" >&2; \
	fi
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		echo "-- enabling ghostauth.socket, ghostseat.socket and ghostd.service..."; \
		systemctl enable --now ghostauth.socket ghostseat.socket || echo "warning: enabling the ghostauth/ghostseat sockets failed -- enable manually" >&2; \
		systemctl enable ghostd.service || echo "warning: systemctl enable ghostd.service failed -- enable manually" >&2; \
		echo "-- starting ghostd.service..."; \
		systemctl start ghostd.service || echo "warning: systemctl start ghostd.service failed -- start manually" >&2; \
	fi

# The accounts the units run as (packaging/system/sysusers.d): the ghost
# user and group for ghostd, the ghostauth user for the PAM
# helper. systemd only reads /usr/lib/sysusers.d and /usr/lib/tmpfiles.d
# on its own, so the installed fragments are applied here by path, which
# works under any prefix. /run/ghost is created now too, so the sockets
# can bind before the first boot with the unit's RuntimeDirectory.
accounts:
	@if [ -z "$(DESTDIR)" ]; then \
		echo "-- creating the ghost and ghostauth accounts (systemd-sysusers)..."; \
		systemd-sysusers "$(PREFIX)/lib/sysusers.d/ghostauth.conf" "$(PREFIX)/lib/sysusers.d/ghostd.conf" && \
		systemd-tmpfiles --create "$(PREFIX)/lib/tmpfiles.d/ghost.conf" \
			|| { echo "error: creating the accounts failed" >&2; exit 1; }; \
	fi

# The key ghostauth mints its tickets with and ghostseat verifies them
# (host/authticket, docs/design/login-and-sessions.md#authentication):
# 32 random bytes as hex, root:ghostauth 0640, generated once. Delete it
# and re-run to rotate; logins in flight at that moment fail.
authkey:
	@f="$(DESTDIR)$(GHOST_CERT_DIR)/auth-ticket.key"; \
	if [ -e "$$f" ]; then echo "-- keeping existing $$f"; \
	else \
		echo "-- generating the authentication ticket key in $$f"; \
		install -d -m 0755 "$(DESTDIR)$(GHOST_CERT_DIR)" && \
		( umask 077 && openssl rand -hex 32 > "$$f" ) && chmod 0640 "$$f" && \
		{ [ -n "$(DESTDIR)" ] || chgrp ghostauth "$$f"; } \
			|| { echo "error: generating the ticket key failed (is the openssl CLI installed?)" >&2; rm -f "$$f"; exit 1; }; \
	fi

# The host's identity (gdp-spec.md §2.3): a self-signed
# certificate ghostd's lobby presents and spectre pins on first use, at
# ghostd's --cert/--key defaults. Generated once and never overwritten --
# replacing it makes every client that trusted this host warn that its
# certificate changed -- and deliberately outside install_manifest.txt, so
# `make uninstall` leaves it too. The key is readable by the ghost group
# only: ghostd, which runs as that user, is its one reader. wraith
# sessions never see it; each generates its own throwaway certificate,
# which ghostd vouches for. A half-present pair is left alone with a
# warning rather than clobbered.
cert:
	@dir="$(DESTDIR)$(GHOST_CERT_DIR)"; crt="$$dir/host-cert.pem"; key="$$dir/host-key.pem"; \
	if [ -e "$$crt" ] && [ -e "$$key" ]; then \
		echo "-- keeping existing $$crt and $$key"; \
		[ -n "$(DESTDIR)" ] || { chgrp ghost "$$key" && chmod 0640 "$$key"; }; \
	elif [ -e "$$crt" ] || [ -e "$$key" ]; then \
		echo "warning: only one of $$crt / $$key exists -- not generating; fix or remove it and re-run 'make cert'" >&2; \
	else \
		host=$$(hostname -f 2>/dev/null || hostname); \
		echo "-- generating self-signed host certificate for $$host in $$dir"; \
		install -d -m 0755 "$$dir" && \
		( umask 077 && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -sha256 -nodes -days 36500 \
			-subj "/CN=$$host" -addext "subjectAltName=DNS:$$host" \
			-keyout "$$key" -out "$$crt" 2>/dev/null ) && \
		chmod 0640 "$$key" && chmod 0644 "$$crt" && \
		{ [ -n "$(DESTDIR)" ] || chgrp ghost "$$key"; } && \
		openssl x509 -in "$$crt" -noout -fingerprint -sha256 \
			|| { echo "error: certificate generation failed (is the openssl CLI installed?)" >&2; rm -f "$$crt" "$$key"; exit 1; }; \
	fi

# ghostd's and wraith's configs (docs/reference/configuration.md):
# packaging/config/ghostd.toml and packaging/config/wraith.toml, every setting commented
# out at its default, each put down once. Like the cert, never overwritten
# -- they're the admin's files from then on -- and outside
# install_manifest.txt so `make uninstall` leaves their edits alone.
# Pristine copies are always in share/doc/ghost to diff against.
config:
	@for name in ghostd.toml wraith.toml; do \
		f="$(DESTDIR)$(GHOST_CERT_DIR)/$$name"; \
		if [ -e "$$f" ]; then \
			echo "-- keeping existing $$f"; \
		else \
			echo "-- installing $$f"; \
			install -d -m 0755 "$(DESTDIR)$(GHOST_CERT_DIR)" && \
			install -m 0644 "packaging/config/$$name" "$$f" || exit 1; \
		fi; \
	done

# `cmake --install` writes install_manifest.txt (one absolute destination
# path per line, not DESTDIR-prefixed, no newline after the last) into
# $(BUILD_DIR); this replays it rather than re-deriving paths from the
# current PREFIX, so it removes exactly what the last install put down even
# if PREFIX has since changed. rmdir -p --ignore-fail-on-non-empty walks
# each file's now-empty parent directories back up, stopping at the first
# one still shared with something else (/usr/local/bin, /etc/pam.d, ...).
# The unit is stopped and disabled while systemctl can still resolve it by
# name; the one reload after removal picks up both.
uninstall:
	@test -f $(BUILD_DIR)/install_manifest.txt || { echo "error: no $(BUILD_DIR)/install_manifest.txt -- nothing to uninstall (run 'make install' first)" >&2; exit 1; }
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		echo "-- stopping and disabling ghostd.service and the sockets..."; \
		systemctl disable --now ghostd.service ghostauth.socket ghostseat.socket || echo "warning: stopping the units failed -- stop manually" >&2; \
	fi
	@# Out of common-account before the hook binary goes (pam-auth-update(8)).
	@if [ -z "$(DESTDIR)" ] && [ -f /usr/share/pam-configs/ghostlogin ] && command -v pam-auth-update >/dev/null 2>&1; then \
		echo "-- removing the ghostlogin PAM hook (pam-auth-update)..."; \
		DEBIAN_FRONTEND=noninteractive pam-auth-update --package --remove ghostlogin || echo "warning: pam-auth-update --remove failed -- remove the pam_exec line from common-account by hand" >&2; \
	fi
	@while IFS= read -r f || [ -n "$$f" ]; do \
		f="$(DESTDIR)$$f"; \
		if [ -e "$$f" ] || [ -L "$$f" ]; then \
			echo "rm $$f"; rm -f "$$f"; \
			rmdir --ignore-fail-on-non-empty -p "$$(dirname "$$f")" 2>/dev/null || true; \
		else \
			echo "-- already removed: $$f"; \
		fi; \
	done < $(BUILD_DIR)/install_manifest.txt
	@$(MAKE) --no-print-directory reload

# ghostd.service (a system unit) needs `systemctl daemon-reload`; wraith.service
# (a --user unit) needs the equivalent run inside each affected user's own
# manager, which `sudo make install/uninstall` can't reach for arbitrary logged-
# in users -- best-effort covers $(SUDO_USER), the account that ran sudo, since
# that's the common case of testing under your own login. Both calls are
# non-fatal: no systemd, no active user session yet, or an unprivileged install
# against a non-system $(PREFIX) shouldn't fail the whole target. Skipped
# entirely under DESTDIR, since a staged/packaging install isn't touching this
# machine's live systemd at all.
reload:
	@if [ -n "$(DESTDIR)" ]; then \
		echo "-- DESTDIR set: skipping systemctl daemon-reload (staged install, not this machine)"; \
	elif command -v systemctl >/dev/null 2>&1; then \
		echo "-- reloading systemd (systemctl daemon-reload)..."; \
		systemctl daemon-reload || echo "warning: systemctl daemon-reload failed -- reload manually" >&2; \
		if [ -n "$(SUDO_USER)" ]; then \
			uid=$$(id -u "$(SUDO_USER)" 2>/dev/null) && \
			echo "-- reloading $(SUDO_USER)'s user systemd (systemctl --user daemon-reload)..." && \
			sudo -u "$(SUDO_USER)" XDG_RUNTIME_DIR="/run/user/$$uid" systemctl --user daemon-reload \
				|| echo "warning: systemctl --user daemon-reload failed for $(SUDO_USER) -- reload manually" >&2; \
		fi; \
	fi

# Veil, the broker (docs/design/veil.md), is its own install: a GDP
# host gets `make install` and must never get veild, its unit or its
# certificate, and a Veil machine needs none of the host's pieces. Built by
# the same `make build` (the host/ Cargo workspace); like `install`, this
# only copies, so run it as root after building as your normal user.
#
# It puts down veild, its unit, /etc/pam.d/veild, veild.toml (once), the
# veil system user (in the ghost group, so it can reach the PAM helper)
# and the ghost-admins group, and generates Veil's lobby certificate
# once, at /etc/ghost/veil-{cert,key}.pem, the key readable by the veil
# group, and the Wisp thin clients' shared key once, at
# /etc/ghost/veil-wisp.key (`veild wisp-env` prints both for the Wisp boot
# server). veild authenticates through ghostauth (the PAM helper every
# host has), so a Veil that isn't also a host gets ghostauth, its socket
# units, its account and its ticket key here too. What it installs is
# listed in $(BUILD_DIR)/veil_install_manifest.txt for uninstall-veil.
VEIL_MANIFEST = $(BUILD_DIR)/veil_install_manifest.txt
# cargo's profile follows the CMake build type as in CMakeLists.txt: debug
# only for a Debug build.
VEIL_PROFILE = $(shell grep -q '^CMAKE_BUILD_TYPE:STRING=Debug$$' $(BUILD_DIR)/CMakeCache.txt 2>/dev/null && echo debug || echo release)

install-veil:
	@test -f host/target/$(VEIL_PROFILE)/veild || { echo "error: host/target/$(VEIL_PROFILE)/veild is not built yet -- run 'make build' first (as your normal user, not root)" >&2; exit 1; }
	@set -e; \
	bindir="$(DESTDIR)$(PREFIX)/bin"; unitdir="$(DESTDIR)$(PREFIX)/lib/systemd/system"; \
	docdir="$(DESTDIR)$(PREFIX)/share/doc/ghost"; pamdir="$(DESTDIR)/etc/pam.d"; \
	install -d "$$bindir" "$$unitdir" "$$docdir" "$$pamdir"; \
	install -m 0755 host/target/$(VEIL_PROFILE)/veild "$$bindir/veild"; \
	sed 's|@GHOST_BINDIR@|$(PREFIX)/bin|g' packaging/system/systemd/veild.service.in > "$$unitdir/veild.service"; \
	chmod 0644 "$$unitdir/veild.service"; \
	packaging/system/pam-render.sh "$(PAM_STACK)" packaging/system/pam.d/veild.in "$$pamdir/veild"; \
	chmod 0644 "$$pamdir/veild"; \
	install -m 0644 packaging/config/veild.toml "$$docdir/veild.toml"; \
	mkdir -p "$(dir $(VEIL_MANIFEST))"; \
	printf '%s\n' "$(PREFIX)/bin/veild" "$(PREFIX)/lib/systemd/system/veild.service" \
		"/etc/pam.d/veild" "$(PREFIX)/share/doc/ghost/veild.toml" > $(VEIL_MANIFEST); \
	echo "-- installed veild, veild.service, /etc/pam.d/veild"
	@# The PAM helper, unless `make install` already put it down (a host
	@# that is also a Veil): then it is the host's, and uninstall-veil
	@# leaves it.
	@set -e; libexecdir="$(DESTDIR)$(PREFIX)/lib/ghost"; unitdir="$(DESTDIR)$(PREFIX)/lib/systemd/system"; \
	sysusersdir="$(DESTDIR)$(PREFIX)/lib/sysusers.d"; \
	if [ -e "$$libexecdir/ghostauth" ] && [ -f $(BUILD_DIR)/install_manifest.txt ] && grep -q '/lib/ghost/ghostauth$$' $(BUILD_DIR)/install_manifest.txt; then \
		echo "-- ghostauth is the host's install; keeping it"; \
	else \
		install -d "$$libexecdir" "$$unitdir" "$$sysusersdir"; \
		install -m 0755 host/target/$(VEIL_PROFILE)/ghostauth "$$libexecdir/ghostauth"; \
		sed 's|@GHOST_LIBEXECDIR@|$(PREFIX)/lib/ghost|g' packaging/system/systemd/ghostauth@.service.in > "$$unitdir/ghostauth@.service"; \
		install -m 0644 packaging/system/systemd/ghostauth.socket "$$unitdir/ghostauth.socket"; \
		chmod 0644 "$$unitdir/ghostauth@.service"; \
		install -m 0644 packaging/system/sysusers.d/ghostauth.conf "$$sysusersdir/ghostauth.conf"; \
		printf '%s\n' "$(PREFIX)/lib/ghost/ghostauth" "$(PREFIX)/lib/systemd/system/ghostauth@.service" \
			"$(PREFIX)/lib/systemd/system/ghostauth.socket" "$(PREFIX)/lib/sysusers.d/ghostauth.conf" >> $(VEIL_MANIFEST); \
		echo "-- installed ghostauth, ghostauth.socket, ghostauth@.service"; \
	fi
	@if [ -z "$(DESTDIR)" ]; then \
		systemd-sysusers "$(PREFIX)/lib/sysusers.d/ghostauth.conf" || { echo "error: creating the ghostauth account failed" >&2; exit 1; }; \
		if ! getent passwd veil >/dev/null; then \
			echo "-- creating the veil system user"; \
			useradd --system --user-group --home-dir /var/lib/veil --no-create-home --shell /usr/sbin/nologin veil; \
		fi; \
		usermod -a -G ghost veil; \
		if ! getent group ghost-admins >/dev/null; then \
			echo "-- creating the ghost-admins group (add admins with: usermod -aG ghost-admins <user>)"; \
			groupadd --system ghost-admins; \
		fi; \
		install -d -o veil -g veil -m 0700 /var/lib/veil; \
	fi
	@f="$(DESTDIR)$(GHOST_CERT_DIR)/veild.toml"; \
	if [ -e "$$f" ]; then echo "-- keeping existing $$f"; \
	else echo "-- installing $$f"; install -d -m 0755 "$(DESTDIR)$(GHOST_CERT_DIR)"; install -m 0644 packaging/config/veild.toml "$$f"; fi
	@dir="$(DESTDIR)$(GHOST_CERT_DIR)"; crt="$$dir/veil-cert.pem"; key="$$dir/veil-key.pem"; \
	if [ -e "$$crt" ] && [ -e "$$key" ]; then \
		echo "-- keeping existing $$crt and $$key"; \
	elif [ -e "$$crt" ] || [ -e "$$key" ]; then \
		echo "warning: only one of $$crt / $$key exists -- not generating; fix or remove it and re-run 'make install-veil'" >&2; \
	else \
		host=$$(hostname -f 2>/dev/null || hostname); \
		echo "-- generating Veil's lobby certificate for $$host in $$dir"; \
		install -d -m 0755 "$$dir" && \
		( umask 077 && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -sha256 -nodes -days 36500 \
			-subj "/CN=$$host" -addext "subjectAltName=DNS:$$host" \
			-keyout "$$key" -out "$$crt" 2>/dev/null ) && \
		chmod 0644 "$$crt" && chmod 0640 "$$key" && \
		{ [ -n "$(DESTDIR)" ] || chgrp veil "$$key"; } && \
		openssl x509 -in "$$crt" -noout -fingerprint -sha256 \
			|| { echo "error: certificate generation failed (is the openssl CLI installed?)" >&2; rm -f "$$crt" "$$key"; exit 1; }; \
	fi
	@# The web UI's HTTPS pair, the [web] cert/key defaults: self-signed, so
	@# browsers warn until the admin points veild.toml at a real one. It
	@# names every name and address this machine has right now.
	@dir="$(DESTDIR)$(GHOST_CERT_DIR)"; crt="$$dir/veil-web-cert.pem"; key="$$dir/veil-web-key.pem"; \
	if [ -e "$$crt" ] && [ -e "$$key" ]; then \
		echo "-- keeping existing $$crt and $$key"; \
	elif [ -e "$$crt" ] || [ -e "$$key" ]; then \
		echo "warning: only one of $$crt / $$key exists -- not generating; fix or remove it and re-run 'make install-veil'" >&2; \
	else \
		fqdn=$$(hostname -f 2>/dev/null || hostname); short=$$(hostname -s 2>/dev/null || hostname); \
		san="DNS:$$fqdn"; [ "$$short" = "$$fqdn" ] || san="$$san,DNS:$$short"; san="$$san,DNS:localhost,IP:127.0.0.1,IP:::1"; \
		for ip in $$(hostname -I 2>/dev/null); do case "$$ip" in fe80:*) ;; *) san="$$san,IP:$$ip";; esac; done; \
		echo "-- generating a self-signed web certificate for $$san in $$dir"; \
		install -d -m 0755 "$$dir" && \
		( umask 077 && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -sha256 -nodes -days 825 \
			-subj "/CN=$$fqdn" -addext "subjectAltName=$$san" -addext "basicConstraints=critical,CA:FALSE" \
			-addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=serverAuth" \
			-keyout "$$key" -out "$$crt" 2>/dev/null ) && \
		chmod 0644 "$$crt" && chmod 0640 "$$key" && \
		{ [ -n "$(DESTDIR)" ] || chgrp veil "$$key"; } \
			|| { echo "error: web certificate generation failed (is the openssl CLI installed?)" >&2; rm -f "$$crt" "$$key"; exit 1; }; \
	fi
	@$(MAKE) --no-print-directory authkey
	@# The Wisp key ([thin_clients] key): 32 random bytes as hex. Delete it
	@# and re-run to rotate (docs/design/wisp.md).
	@f="$(DESTDIR)$(GHOST_CERT_DIR)/veil-wisp.key"; \
	if [ -e "$$f" ]; then echo "-- keeping existing $$f"; \
	else \
		echo "-- generating the Wisp key in $$f"; \
		install -d -m 0755 "$(DESTDIR)$(GHOST_CERT_DIR)" && \
		( umask 077 && openssl rand -hex 32 > "$$f" ) && chmod 0640 "$$f" && \
		{ [ -n "$(DESTDIR)" ] || chgrp veil "$$f"; } \
			|| { echo "error: generating the Wisp key failed (is the openssl CLI installed?)" >&2; rm -f "$$f"; exit 1; }; \
	fi
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		systemctl daemon-reload || true; \
		echo "-- enabling and starting ghostauth.socket and veild.service..."; \
		systemctl enable --now ghostauth.socket || echo "warning: enabling ghostauth.socket failed -- enable manually" >&2; \
		systemctl enable veild.service || echo "warning: systemctl enable veild.service failed -- enable manually" >&2; \
		systemctl restart veild.service || echo "warning: veild.service failed to start -- see 'systemctl status veild'" >&2; \
	fi

# The reverse of install-veil, from its manifest. Like uninstall, it
# leaves the admin's own files: veild.toml, the lobby and web
# certificates, the Wisp key, the ticket key, the database in
# /var/lib/veil, the veil, ghost and ghostauth accounts and the
# ghost-admins group. ghostauth's socket is only stopped when the helper
# was Veil's own (not a host's).
uninstall-veil:
	@test -f $(VEIL_MANIFEST) || { echo "error: no $(VEIL_MANIFEST) -- nothing to uninstall (run 'make install-veil' first)" >&2; exit 1; }
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		systemctl disable --now veild.service || echo "warning: stopping veild.service failed -- stop manually" >&2; \
		if grep -q 'ghostauth.socket$$' $(VEIL_MANIFEST); then systemctl disable --now ghostauth.socket || true; fi; \
	fi
	@while IFS= read -r f; do \
		f="$(DESTDIR)$$f"; \
		if [ -e "$$f" ]; then echo "rm $$f"; rm -f "$$f"; else echo "-- already removed: $$f"; fi; \
	done < $(VEIL_MANIFEST)
	@rm -f $(VEIL_MANIFEST)
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload || true; fi
	@echo "-- kept /etc/ghost/veild.toml, /etc/ghost/veil-{cert,key}.pem, /etc/ghost/veil-wisp.key, /etc/ghost/auth-ticket.key, /var/lib/veil, the accounts and ghost-admins"

# The Debian packages (ghostd, ghostauth, veild, spectre, libgdp-dev, libpyrowave-shared0):
# the standard `dpkg-buildpackage`, binary packages only, unsigned, which
# writes them to the parent directory; this then moves what that run produced
# (named by its .changes file) into ./target. Builds under debian/_build, not
# $(BUILD_DIR). Needs `apt build-dep .` (or the Build-Depends in
# debian/control) installed. Other architectures: run this on that
# architecture.
deb:
	dpkg-buildpackage -us -uc -b
	@set -e; \
	base=ghost_$$(dpkg-parsechangelog -S Version)_$$(dpkg-architecture -qDEB_HOST_ARCH); \
	mkdir -p target; \
	for f in $$(sed -n '/^Files:/,$$p' ../$$base.changes | awk 'NR>1 {print $$5}') $$base.changes; do \
		mv -f "../$$f" target/; \
	done; \
	echo "-- packages in target/:"; ls target

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

clean:
	if [ -f $(BUILD_DIR)/CMakeCache.txt ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi
	if [ -d host/target ]; then cd host && cargo clean; fi

# Also the `make deb` output (target/), dpkg-buildpackage's working files
# under debian/ (the same set .gitignore lists; no tracked file is among them)
# and the Wisp rootfs output (client/wisp's clean).
distclean:
	rm -rf $(BUILD_DIR) build-debug build-release build-asan host/target client/spectre-qt/build client/spectre-qt/.qtcreator
	rm -rf target debian/_build debian/_pyrowave debian/tmp debian/.debhelper debian/debhelper-build-stamp \
		debian/files debian/*.substvars debian/*.debhelper.log debian/*.debhelper debian/veild.service debian/veild.pam \
		debian/ghostd debian/veild debian/spectre debian/libgdp-dev debian/libpyrowave-shared0
	$(MAKE) -C client/wisp clean
