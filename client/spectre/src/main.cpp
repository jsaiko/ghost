// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "gdp/cert_fingerprint.hpp"
#include "decode/codec_support.hpp"
#include "stream/stream_session.hpp"
#include "ui/hotkey.hpp"
#include "log.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void usage(const char *argv0) {
	printf(
		"Usage: %s -h host -p port [-t token] -P sha256 [-X decoder] [-f | -K] [-T] [-r WIDTHxHEIGHT]\n"
		"       [-A] [-V view] [-c scale] [-k chord] [-C codec] [-N profile] [-R on|off] [-D] [-G | -g] [-M] [-W]\n",
		argv0);
	printf("  -t token  the session token; without -t, spectre takes it from $SPECTRE_TOKEN,\n");
	printf("            as spectre-qt passes it. The command line is visible to other users\n");
	printf("  -P sha256 the session host's certificate fingerprint (64 hex digits, colons allowed);\n");
	printf("            spectre refuses any host presenting a different certificate. spectre-qt\n");
	printf("            passes the one the login server vouched for. For a direct-connect wraith\n");
	printf("            (-l), run `openssl x509 -in cert.pem -noout -fingerprint -sha256` on the host\n");
#if defined(_WIN32)
	printf("  -X decoder  video decoder to try first: vulkan (default), d3d11va, or software;\n");
	printf("            a GPU decoder that can't open falls back to the other, then software\n");
#elif defined(__APPLE__)
	printf("  -X decoder  video decoder to try first: vulkan (default), videotoolbox, or software;\n");
	printf("            a GPU decoder that can't open falls back to the other, then software. MoltenVK\n");
	printf("            has no Vulkan Video, so in practice that's videotoolbox\n");
#else
	printf("  -X decoder  video decoder to try first: vulkan (default), vaapi, v4l2, or software;\n");
	printf("            a GPU decoder that can't open falls back to the other, then a V4L2 one\n");
	printf("            (a Raspberry Pi's), then software. NVIDIA never tries vaapi, having no VA-API\n");
#endif
	printf("  -f        open the session window fullscreen\n");
	printf("  -T        take over: if this user's session is open on another client, disconnect\n");
	printf("            that client and attach to it instead of failing (exit status 4 means\n");
	printf("            it was refused for that reason)\n");
	printf("  -K        kiosk: fullscreen for good -- no way out of it from spectre and no\n");
	printf("            minimize button; the toolbar's close, Disconnect and End Session still work\n");
	printf("  -r WIDTHxHEIGHT  request this output resolution (sent in SessionHello.displays);\n");
	printf("            the host may ignore it -- SessionAccept's negotiated size always wins\n");
	printf("  -A        follow the window: when the window settles at a new size, ask the host\n");
	printf("            for that resolution. Off by default, when the picture scales to the\n");
	printf("            window and the resolution changes only from the session menu\n");
	printf("  -V view   fit (scale the picture to the window) or actual (1:1, centred when\n");
	printf("            smaller, panned by pushing the pointer against an edge when bigger).\n");
	printf("            Default: whichever the session menu last picked\n");
	printf("  -c scale  extra cursor size multiplier (default 1); the pointer is drawn at the size\n");
	printf("            the host sends it, scaled with the window like the rest of the video\n");
	printf("  -C codec  preferred video codec, offered ahead of the rest (pyrowave, h264, h265,\n");
	printf("            av1); the host picks the first codec it can encode, so this is a\n");
	printf("            preference, not a demand. -C pyrowave offers it whatever the link\n");
	printf("  -W        never offer pyrowave. Otherwise it is offered first when the host is\n");
	printf("            directly reachable over a wired link of 1 Gbit/s or more (Linux, Windows)\n");
	printf("  -N profile  network profile for the host's rate control: auto (default -- the host\n");
	printf("            classifies the link from what it measures), lan, internet, or mobile\n");
	printf("  -R on|off lossless refinement: a lossless tile layer over the video (any codec)\n");
	printf("            that makes settled text/UI bit-exact, at the cost of a per-frame readback\n");
	printf("            on the host. Default: whichever the session menu last picked, on at first\n");
	printf("  -D        debug: briefly outline every lossless refinement tile in red as it lands,\n");
	printf("            so it's visible which regions the host is refreshing losslessly\n");
	printf("  -G        forward gamepads: local controllers are mirrored onto the host, if it\n");
	printf("            supports that -- raw where the controller and host allow (the host's own\n");
	printf("            driver for it, touchpad, motion, lights, rumble, Steam Input), otherwise as\n");
	printf("            a virtual Xbox pad. Off by default\n");
	printf("  -g        forward gamepads, starting with every one as a virtual Xbox pad (the\n");
	printf("            session menu switches to raw)\n");
	printf("  -M        send this machine's microphone to the host, as a virtual microphone in the\n");
	printf("            session, if it supports that. Off by default\n");
	printf("  -k chord  keys that open the session menu, '+'-joined (default: %s);\n",
		spectre::kDefaultMenuHotkey);
	printf("            ctrl/alt/shift/super match either side, lctrl/ralt/... one side,\n");
	printf("            anything else is an SDL key name with '_' for spaces (f12, scroll_lock)\n");
	printf("\n");
	printf("%s --probe-decoders: print each decode path that works here, one per line, as\n", argv0);
	printf("            <path>:<codec> (vulkan:av1, vaapi:h264, software:h264, ...), then exit\n");
}

// getopt() is POSIX-only (no MSVC equivalent), so the flags are parsed with a
// plain manual scan that behaves the same on every platform.
int main(int argc, char *argv[]) {
	spectre::Log::init();
#ifdef __APPLE__
	// MoltenVK logs at info by default -- its whole extension list and GPU
	// description on every instance, and spectre makes two. Keep only its
	// errors unless spectre itself is at debug, or MoltenVK's own setting
	// was given.
	if (!spectre::Log::enabled(spectre::LogLevel::Debug)) {
		setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
	}
#endif
	// Run by Wisp's agent to report a client's working decoders to Veil
	// (docs/design/wisp.md).
	if (argc == 2 && strcmp(argv[1], "--probe-decoders") == 0) {
		for (const std::string &path : spectre::probe_decode_paths(spectre::kDrmRenderNode)) {
			printf("%s\n", path.c_str());
		}
		return 0;
	}

	spectre::StreamOptions options;
	std::string menu_hotkey = spectre::kDefaultMenuHotkey;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		auto next_value = [&]() -> const char * {
			if (i + 1 >= argc) {
				return nullptr;
			}
			return argv[++i];
		};

		if (strcmp(arg, "-h") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			options.host = v;
		} else if (strcmp(arg, "-p") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			char *end = nullptr;
			unsigned long port = strtoul(v, &end, 10);
			if (*end != '\0' || port == 0 || port > 65535) {
				SLOG_ERROR("spectre: -p must be a port number (1-65535)");
				return 1;
			}
			options.port = (uint16_t)port;
		} else if (strcmp(arg, "-t") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			options.token = v;
		} else if (strcmp(arg, "-P") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			// Accept openssl's colon-separated uppercase form as well as
			// the bare lowercase one.
			options.cert_sha256.clear();
			for (const char *c = v; *c; c++) {
				if (*c != ':') {
					options.cert_sha256 += (char)tolower((unsigned char)*c);
				}
			}
			if (!gdp::is_sha256_hex(options.cert_sha256)) {
				SLOG_ERROR("spectre: -P must be a SHA-256 fingerprint (64 hex digits)");
				return 1;
			}
		} else if (strcmp(arg, "-X") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			if (!spectre::Decoder::backend_from_token(v, &options.decoder)) {
				SLOG_ERROR("spectre: unknown decoder \"%s\" (see -X in the usage)", v);
				return 1;
			}
		} else if (strcmp(arg, "-f") == 0) {
			options.start_fullscreen = true;
		} else if (strcmp(arg, "-T") == 0) {
			options.take_over = true;
		} else if (strcmp(arg, "-K") == 0) {
			options.kiosk = true;
		} else if (strcmp(arg, "-R") == 0) {
			const char *v = next_value();
			if (v && strcmp(v, "on") == 0) {
				options.lossless = true;
			} else if (v && strcmp(v, "off") == 0) {
				options.lossless = false;
			} else {
				usage(argv[0]);
				return 1;
			}
		} else if (strcmp(arg, "-D") == 0) {
			options.debug_tile_outlines = true;
		} else if (strcmp(arg, "-G") == 0) {
			options.forward_gamepads = true;
		} else if (strcmp(arg, "-g") == 0) {
			options.forward_gamepads = true;
			options.raw_gamepads = false;
		} else if (strcmp(arg, "-M") == 0) {
			options.microphone = true;
		} else if (strcmp(arg, "-W") == 0) {
			options.allow_pyrowave = false;
		} else if (strcmp(arg, "-A") == 0) {
			options.follow_window = true;
		} else if (strcmp(arg, "-V") == 0) {
			const char *v = next_value();
			if (v && strcmp(v, "fit") == 0) {
				options.actual_size = false;
			} else if (v && strcmp(v, "actual") == 0) {
				options.actual_size = true;
			} else {
				usage(argv[0]);
				return 1;
			}
		} else if (strcmp(arg, "-r") == 0) {
			const char *v = next_value();
			unsigned w, h;
			if (!v || sscanf(v, "%ux%u", &w, &h) != 2) {
				usage(argv[0]);
				return 1;
			}
			options.requested_width = w;
			options.requested_height = h;
		} else if (strcmp(arg, "-c") == 0) {
			const char *v = next_value();
			if (!v || (options.cursor_scale = strtof(v, nullptr)) <= 0.0f) {
				usage(argv[0]);
				return 1;
			}
		} else if (strcmp(arg, "-C") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			options.preferred_codec = v;
		} else if (strcmp(arg, "-N") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			options.network_profile = v;
		} else if (strcmp(arg, "-k") == 0) {
			const char *v = next_value();
			if (!v) {
				usage(argv[0]);
				return 1;
			}
			menu_hotkey = v;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	// The token stays out of argv, which other local users can read; the
	// environment is this user's alone. Unset so nothing spectre starts
	// inherits it.
	if (const char *env_token = getenv("SPECTRE_TOKEN")) {
		if (options.token.empty()) {
			options.token = env_token;
		}
#if defined(_WIN32)
		_putenv_s("SPECTRE_TOKEN", "");
#else
		unsetenv("SPECTRE_TOKEN");
#endif
	}

	if (options.host.empty() || options.port == 0) {
		usage(argv[0]);
		return 1;
	}
	if (options.cert_sha256.empty()) {
		// gdp-spec.md §2.3: spectre never speaks to a host whose certificate
		// it wasn't given.
		SLOG_ERROR("spectre: -P is required -- the host's certificate fingerprint to pin");
		return 1;
	}
	std::string hotkey_error;
	if (!spectre::parse_hotkey_chord(menu_hotkey, &options.menu_hotkey, &hotkey_error)) {
		SLOG_ERROR("spectre: bad -k hotkey: %s", hotkey_error.c_str());
		return 1;
	}

	return spectre::StreamSession(options).run();
}
