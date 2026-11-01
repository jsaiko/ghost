// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "stream/stream_session.hpp"

#include "gdp/error_codes.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "app_icon_png.h"
#include "decode/codec_support.hpp"
#include "gdp/clock.hpp"
#include "gdp/file_transfer.hpp"
#include "gdp/framing.hpp"
#include "gdp/negotiation.hpp"
#include "gdp/refine.hpp"
#include "log.hpp"
#include "net/clipboard_files.hpp"
#include "net/lan_link.hpp"
#include "net/my_folders.hpp"
#include "session.pb.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace spectre {

namespace {

#ifndef _WIN32
// SIGUSR1: end the session as the menu's End Session does. Wisp's agent
// sends it when an administrator logs a thin client's user out from Veil.
volatile sig_atomic_t g_logout_signalled = 0;

void on_logout_signal(int) {
	g_logout_signalled = 1;
}
#endif

#ifdef __linux__
// VkPhysicalDeviceProperties::vendorID for NVIDIA (its PCI vendor ID).
constexpr uint32_t kNvidiaVendorId = 0x10de;
#endif

// Mac keyboards have no Scroll Lock, and their function keys need Fn, so
// on macOS mouse capture and fullscreen get Ctrl+Cmd chords as well --
// Ctrl+Cmd+F being the system's own fullscreen shortcut. Cmd is SDL's GUI
// modifier.
#ifdef __APPLE__
bool is_ctrl_cmd_chord(const SDL_KeyboardEvent &key, SDL_Scancode scancode) {
	return key.scancode == scancode && (key.mod & SDL_KMOD_CTRL) && (key.mod & SDL_KMOD_GUI) &&
		!(key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_ALT));
}
constexpr const char *kCaptureMouseKeyName = "Ctrl+Cmd+G";
#else
constexpr const char *kCaptureMouseKeyName = "Scroll Lock";
#endif

constexpr uint64_t kAcceptTimeoutUs = 10'000'000;
constexpr uint64_t kStatsIntervalUs = 250'000; // gdp-spec.md §7.5: every 250ms
// How often the statistics overlay's numbers are recomputed.
constexpr uint64_t kStatsWindowUs = 500'000;
// UI font em height at display scale 1.0 (see StreamSession::ui_font_).
constexpr float kUiFontPx = 14.0f;
// Actual-size view: the scroll indicators stay up this long after the
// last pan, then fade out over kIndicatorFadeUs.
constexpr uint64_t kIndicatorHoldUs = 1'000'000;
constexpr uint64_t kIndicatorFadeUs = 400'000;

// poll()'s notify_fd-based wakeup has no Windows equivalent (see libgdp's
// EventQueue), so the main loop there just sleeps a fixed short interval and
// dispatches unconditionally instead -- dispatch() is safe to call
// speculatively even when nothing's pending (gdp/transport.hpp).
// `others`: more fds to wake for, -1 for none.
#ifdef _WIN32
void wait_for_notify(int /*fd*/, int timeout_ms, std::initializer_list<int> /*others*/ = {}) {
	Sleep((DWORD)timeout_ms);
}
#else
void wait_for_notify(int fd, int timeout_ms, std::initializer_list<int> others = {}) {
	struct pollfd pfds[4];
	nfds_t n = 0;
	pfds[n++] = {fd, POLLIN, 0};
	for (int other : others) {
		if (other >= 0 && n < 4) {
			pfds[n++] = {other, POLLIN, 0};
		}
	}
	poll(pfds, n, timeout_ms);
}
#endif

// Before SDL_INIT_VIDEO, which reads some of them (the Wayland app-id).
void set_sdl_hints() {
	// Matches the "spectre-qt" desktop entry (packaging/desktop/spectre-qt.desktop.in)
	// so Wayland compositors/taskbars -- which resolve a window's icon by
	// app-id rather than accepting one at runtime -- show the same icon for
	// this streaming window as for the launcher. SDL_SetWindowIcon() in
	// open_window() covers X11/Windows, where SDL can set one directly.
	SDL_SetHint(SDL_HINT_APP_ID, "spectre-qt");
	// Fullscreen grabs the keyboard (apply_keyboard_grab()), and then
	// Alt+Tab is the remote desktop's too: SDL would otherwise keep
	// handling it locally so a fullscreen app can't trap the user, but
	// spectre has its own ways out (Ctrl+Shift+F11, the menu, the
	// toolbar).
	SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
	// SDL swallows the click that focuses an unfocused window (X11,
	// Windows, macOS) unless told otherwise; pass it through, so the
	// focusing click reaches the remote desktop.
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
	// SDL keeps the screen awake for as long as a window is open. A remote
	// desktop nobody is touching is as idle as a local one, so the local
	// screensaver and display sleep go ahead -- except while the remote
	// desktop holds itself awake, and after gamepad input
	// (note_gamepad_activity()), which the local desktop never sees.
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
}

// Linux input-event-codes.h BTN_* values (session.proto's PointerButton
// wire representation) for SDL3's 1-based mouse button index.
uint32_t sdl_button_to_btn_code(Uint8 sdl_button) {
	switch (sdl_button) {
	case SDL_BUTTON_LEFT: return 0x110;   // BTN_LEFT
	case SDL_BUTTON_RIGHT: return 0x111;  // BTN_RIGHT
	case SDL_BUTTON_MIDDLE: return 0x112; // BTN_MIDDLE
	case SDL_BUTTON_X1: return 0x113;     // BTN_SIDE
	case SDL_BUTTON_X2: return 0x114;     // BTN_EXTRA
	default: return 0;
	}
}

} // namespace

StreamSession::StreamSession(StreamOptions options) : options_(std::move(options)) {
	tile_debug_.set_enabled(options_.debug_tile_outlines);
	client_.on_accepted = [this](const SessionInfo &s, const DisplayInfo &d, const AudioInfo &a) {
		on_accepted(s, d, a);
	};
	client_.on_disconnected = [this](const std::string &reason) { on_disconnected(reason); };
	client_.on_video_frame = [this](bool /*keyframe*/, const uint8_t *data, size_t len) {
		return on_video_frame(data, len);
	};
	client_.on_cursor_shape = [this](uint32_t w, uint32_t h, int32_t hx, int32_t hy, const uint8_t *argb) {
		on_cursor_shape(w, h, hx, hy, argb);
	};
	client_.on_cursor_position = [this](double x, double y) { on_cursor_position(x, y); };
	client_.on_clipboard_text = [this](const std::string &utf8) { on_clipboard_text(utf8); };
	client_.on_displays_changed = [this](const DisplayInfo &d) { on_displays_changed(d); };
	client_.on_diagnostics_request = [this] { return diagnostics_text(); };
	client_.on_desktop_state = [this](const gdp::session::DesktopState &state) {
		if (state.idle_inhibited() != host_idle_inhibited_) {
			SLOG_INFO("spectre: the host's desktop %s",
				state.idle_inhibited() ? "is held awake" : "is no longer held awake");
		}
		host_idle_inhibited_ = state.idle_inhibited();
	};
	client_.on_file_list = [this](const gdp::session::FileList &list) { on_host_file_list(list); };
	client_.on_file_fetch = [this](const gdp::session::FileFetch &fetch) { local_files_.answer(fetch); };
	// What was sent is said once the batch is over (on_batch_done); a
	// failure is said at once, and the rest of the batch goes on.
	files_.on_failed = [this](const std::string &name, const std::string &why) {
		notify(NoticeLevel::kError, name + ": " + why, 5000000);
		mark_ui_dirty();
	};
	files_.on_batch_done = [this](const UploadProgress &p) {
		end_upload_card();
		ToastSpec summary = upload_summary(p);
		if (!summary.title.empty()) {
			notices_.toast(std::move(summary), gdp::monotonic_us());
		}
		mark_ui_dirty();
	};
	// Raw controllers (gdp-spec.md §8.6): the host's requests go to the
	// slot's worker thread, which replies straight onto the input stream.
	client_.on_hid_rejected = [this](uint32_t i, const std::string &reason) { on_hid_rejected(i, reason); };
	client_.on_hid_output = [this](uint32_t i, const std::string &data) {
		if (i < gdp::kMaxGamepads && gamepads_[i].raw) {
			gamepads_[i].raw->output(data);
		}
	};
	client_.on_hid_get_report = [this](uint32_t i, uint32_t request_id, uint32_t report_id, int type) {
		if (i < gdp::kMaxGamepads && gamepads_[i].raw) {
			gamepads_[i].raw->get_report(request_id, report_id, type);
		}
	};
	client_.on_hid_set_report = [this](uint32_t i, uint32_t request_id, uint32_t report_id, int type,
									const std::string &data) {
		if (i < gdp::kMaxGamepads && gamepads_[i].raw) {
			gamepads_[i].raw->set_report(request_id, report_id, type, data);
		}
	};
	// Security keys (gdp-spec.md §8.7): the host's packets go to the key's
	// writer thread.
	client_.on_security_key_rejected = [this](uint32_t i, const std::string &reason) {
		on_security_key_rejected(i, reason);
	};
	client_.on_security_key_output = [this](uint32_t i, const std::string &packet) {
		if (i < gdp::kMaxSecurityKeys && keys_[i]) {
			relay_security_key_packet(i, packet);
		}
	};
	// Smart cards (gdp-spec.md §8.8): the host's requests go through the
	// card's policy to the PC/SC worker.
	client_.on_smartcard_rejected = [this](uint32_t i, const std::string &reason) { on_smartcard_rejected(i, reason); };
	client_.on_smartcard_power = [this](uint32_t i, uint32_t sequence, int action) {
		on_smartcard_power(i, sequence, action);
	};
	client_.on_smartcard_transmit = [this](uint32_t i, uint32_t sequence, const std::string &apdu) {
		on_smartcard_transmit(i, sequence, apdu);
	};
	decoder_.on_frame = [this](AVFrame *frame) { on_decoded_frame(frame); };
}

StreamSession::~StreamSession() {
	// Explicit, in this order, before SDL_Quit(): the members would
	// otherwise be destroyed *after* this destructor body, i.e. after
	// SDL_Quit() already tore the video/audio subsystems down. Every
	// close() here is a no-op on something never opened.
	av_frame_free(&pending_frame_);
	// The mount goes before SDL: the clipboard may still offer paths into it.
	if (own_files_offer_ && sdl_initialized_) {
		SDL_ClearClipboardData();
	}
	host_files_.stop();
#ifdef _WIN32
	// A read Explorer is blocked in returns; host_files_clipboard_ then
	// takes the offer back and ends its thread as it goes.
	host_file_reads_->close();
#endif
	local_files_.cancel_all();
	decoder_.close();
	presenter_.shutdown();
	microphone_.close();
	audio_player_.close();
	audio_decoder_.close();
	close_gamepads();
	if (window_) {
		SDL_DestroyWindow(window_);
	}
	if (sdl_initialized_ || sdl_video_early_) {
		SDL_Quit();
	}
}

// The exit status for how the host closed the connection, when it said why
// in a way the launchers act on (stream_session.hpp's kExit*); `otherwise`
// for anything else.
int StreamSession::exit_status_for_close(int otherwise) const {
	switch (static_cast<gdp::ErrorCode>(client_.disconnect_code())) {
	case gdp::ErrorCode::kEndedByLocalLogin: return kExitEndedByLocalLogin;
	case gdp::ErrorCode::kAlreadyConnected: return kExitAlreadyConnected;
	case gdp::ErrorCode::kTakenOver: return kExitTakenOver;
	default: return otherwise;
	}
}

int StreamSession::run() {
#ifndef _WIN32
	struct sigaction logout_action = {};
	logout_action.sa_handler = on_logout_signal;
	sigemptyset(&logout_action.sa_mask);
	logout_action.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &logout_action, nullptr);
#endif
	if (!connect_and_wait_for_accept()) {
		// A refusal for being connected elsewhere comes before any accept.
		return exit_status_for_close(1);
	}
	if (!open_window()) {
		return 1;
	}
	open_audio();
	open_microphone();

	client_.request_keyframe(); // an IDR to start decoding from, whatever the stream's history

	// spectre always draws its own cursor overlay (server-supplied image,
	// gdp-spec.md §7.4) rather than the client OS's generic arrow -- hiding
	// the real one avoids showing both at once.
	SDL_HideCursor();
	last_cursor_x_ = (float)display_.width / 2.0f;
	last_cursor_y_ = (float)display_.height / 2.0f;

	SLOG_INFO("spectre: session menu hotkey: %s", options_.menu_hotkey.describe().c_str());
	prefs_ = load_prefs();
	SLOG_INFO("spectre: mouse sensitivity %.2fx (captured mouse)", prefs_.mouse_sensitivity);
	set_actual_size(options_.actual_size.value_or(prefs_.actual_size), false);
	set_lossless(options_.lossless.value_or(prefs_.lossless), false);
	set_low_latency(options_.low_latency.value_or(prefs_.low_latency), false);

	main_loop();
	if (prefs_dirty_) {
		// The session ended with the menu still up, slider moved.
		save_prefs(prefs_);
	}

	if (disconnected_) {
		if (logout_requested_) {
			SLOG_INFO("spectre: logged out (%s)",
				disconnect_reason_.empty() ? "disconnected" : disconnect_reason_.c_str());
		} else {
			SLOG_INFO("spectre: session ended (%s)",
				disconnect_reason_.empty() ? "disconnected" : disconnect_reason_.c_str());
		}
	}
	if (presenter_failed_) {
		return 1;
	}
	// The launchers say why the window went away (the kExit* statuses).
	return exit_status_for_close(0);
}

// --- run() steps ---

DecodableCodecs StreamSession::offerable_codecs() const {
	DecodableCodecs offer = probe_decodable_codecs(options_.decoder, kDrmRenderNode);
	std::vector<std::string> &codecs = offer.codecs;
	// pyrowave only on a wired LAN of 1 Gbit/s or more (gdp-spec.md §6.6),
	// unless asked for by name. It leads gdp's token order, so offering it
	// at all offers it first. (Its 10-bit entry, if any, stays in
	// `ten_bit`; SessionClient only declares depths for codecs offered.)
	auto pyrowave = std::find(codecs.begin(), codecs.end(), gdp::kVideoCodecPyrowave);
	if (pyrowave == codecs.end()) {
		SLOG_INFO("spectre: not offering pyrowave: this client can't decode it (see vulkan_device above)");
		return offer;
	}
	if (options_.preferred_codec == gdp::kVideoCodecPyrowave) {
		SLOG_INFO("spectre: offering pyrowave (-C pyrowave)");
		return offer;
	}
	if (!options_.allow_pyrowave) {
		codecs.erase(pyrowave);
		return offer;
	}
	LanLink link = probe_lan_link(options_.host, options_.port);
	if (!link.qualifies) {
		SLOG_INFO("spectre: not offering pyrowave: %s", link.description.c_str());
		codecs.erase(pyrowave);
		return offer;
	}
	SLOG_INFO("spectre: offering pyrowave first: %s", link.description.c_str());
	return offer;
}

// The stats overlay's colour line: depth, matrix, range, subsampling (the
// last is always 4:2:0, gdp-spec.md §7.3).
std::string StreamSession::describe_color(const VideoColor &color) {
	std::string matrix = color.matrix == ColorMatrix::Bt601 ? "BT.601"
		: color.matrix == ColorMatrix::Bt709                ? "BT.709"
															: "BT.2020";
	return std::to_string(color.bit_depth) + "-bit " + matrix +
		(color.range == ColorRange::Full ? " full" : " limited") + " 4:2:0";
}

std::string StreamSession::diagnostics_text() const {
	auto join = [](const std::vector<std::string> &names) {
		std::string out;
		for (const std::string &name : names) {
			out += out.empty() ? name : ", " + name;
		}
		return out.empty() ? std::string("none") : out;
	};
	auto yes_no = [](bool value) { return value ? "yes" : "no"; };
	std::string text = "spectre diagnostics\n";
	text += "platform: " + std::string(SDL_GetPlatform()) + "\n";
	text += "host: " + options_.host + ":" + std::to_string(options_.port) + "\n";
	text += "offered codecs: " + join(offered_codecs_) + " (10-bit: " + join(offered_ten_bit_) + ")\n";
	text += "session codec: " + session_info_.codec + " at " + std::to_string(display_.color.bit_depth) +
		" bits (host encoder: " + session_info_.encoder + ")\n";
	text += "capabilities: " + join(session_info_.capabilities) + "\n";
	text += std::string("decoder: ") + decoder_.backend_name() + "\n";
	if (window_) {
		// The platform decoder's import check; macOS copies its frames in.
		std::string native_import;
#if defined(__linux__)
		native_import = std::string(", dmabuf import ") + yes_no(presenter_.supports_dmabuf_import());
#elif defined(_WIN32)
		native_import = std::string(", D3D11 import ") + yes_no(presenter_.adapter_luid() != nullptr);
#endif
		char gpu[256];
		snprintf(gpu, sizeof(gpu),
			"gpu: vendor 0x%04x, Vulkan Video decode %s, PyroWave compute decode %s%s\n",
			presenter_.vendor_id(), yes_no(presenter_.decode_device()), yes_no(presenter_.compute_device()),
			native_import.c_str());
		text += gpu;
	}
	text += "\n--- log (what passed SPECTRE_LOG's level; error, info or debug) ---\n";
	text += Log::recent();
	return text;
}

bool StreamSession::connect_and_wait_for_accept() {
	set_sdl_hints();
	// Video now rather than in open_window(), for the screen's size and
	// scale that SessionHello's display carries (requested_scale()). Without
	// it there is no scale to send, and open_window() reports the failure.
	sdl_video_early_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
	client_.set_requested_display(options_.requested_width, options_.requested_height,
		options_.requested_width ? requested_scale(options_.requested_width, options_.requested_height) : 0.0,
		requested_refresh_mhz());
	DecodableCodecs offer = offerable_codecs();
	offered_codecs_ = offer.codecs;
	offered_ten_bit_ = offer.ten_bit;
	client_.set_decodable_codecs(offered_codecs_);
	client_.set_ten_bit_codecs(offered_ten_bit_);
	if (!options_.preferred_codec.empty()) {
		client_.set_preferred_codec(options_.preferred_codec);
	}
	client_.set_network_profile(options_.network_profile);
	client_.set_take_over(options_.take_over);
	client_.set_gamepad_forwarding(options_.forward_gamepads);
	// Offered whichever mode the session starts in: the menu can switch
	// to raw later, but a capability can't be added after SessionAccept.
	client_.set_raw_controllers(true);
	client_.set_security_keys(options_.forward_security_keys);
	client_.set_smartcards(options_.forward_smartcards);
	client_.set_microphone(options_.microphone);
	client_.set_file_share(!options_.shared_folders.empty() || options_.share_my_files);
	if (!client_.connect(options_.host, options_.port, options_.token, options_.cert_sha256)) {
		SLOG_ERROR("spectre: connect() failed locally");
		return false;
	}

	SLOG_INFO("spectre: connecting to %s:%u...", options_.host.c_str(), options_.port);
	uint64_t start_us = gdp::monotonic_us();
	while (!accepted_ && !disconnected_) {
		wait_for_notify(client_.notify_fd(), 20);
		client_.dispatch();
		if (gdp::monotonic_us() - start_us > kAcceptTimeoutUs) {
			SLOG_ERROR("spectre: timed out waiting for SessionAccept");
			return false;
		}
	}
	if (disconnected_) {
		SLOG_ERROR("spectre: session rejected/closed: %s",
			disconnect_reason_.empty() ? "(no reason given)" : disconnect_reason_.c_str());
		return false;
	}
	SLOG_INFO("spectre: accepted, %ux%u, codec %s%s", display_.width, display_.height,
		session_info_.codec.c_str(),
		gdp::has_capability(session_info_.capabilities, gdp::kCapabilityRefine) ? " + lossless refinement"
																				: "");
	return true;
}

bool StreamSession::open_window() {
	// SDL_INIT_GAMEPAD whether or not the session negotiated "gamepad"
	// (no spectre -G, or a host that can't create devices): the subsystem is
	// cheap, and handle_gamepad_added() is where the capability check
	// lives, so such a session just sees no controller ever opened.
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
		SLOG_ERROR("spectre: SDL_Init failed: %s", SDL_GetError());
		return false;
	}
	sdl_initialized_ = true;
	clipboard_ready_ = std::strcmp(SDL_GetCurrentVideoDriver(), "wayland") != 0;
	flush_pending_clipboard();

	// Whose session on which host, for the window title and the menu's
	// title line. A host may send neither (gdp-spec.md §6.5), in which case
	// the address we connected to is the best available name.
	std::string host = session_info_.host_name.empty() ? options_.host : session_info_.host_name;
	std::string session_title = session_info_.host_user.empty() ? host : session_info_.host_user + "@" + host;
	menu_.set_title(session_title);
	toolbar_.set_title(session_title);
	menu_.set_kiosk(options_.kiosk);
	toolbar_.set_native_size_shown(!options_.follow_window);
	toolbar_.set_kiosk(options_.kiosk);

	// HIGH_PIXEL_DENSITY: render at the monitor's physical pixels. Without
	// it a scaled desktop (say Wayland at 200%) hands SDL a logical-size
	// buffer, so a 4K stream in a fullscreen 4K window gets downscaled to
	// 1080p here and the compositor blows it back up 2x -- blurry, and the
	// lossless refinement layer is destroyed on the way. The stream is
	// already in remote pixels; the local desktop's scale has no business
	// resampling it.
	SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (options_.start_fullscreen || options_.kiosk) {
		window_flags |= SDL_WINDOW_FULLSCREEN;
	}
	window_ = SDL_CreateWindow((session_title + " - Spectre").c_str(), (int)display_.width,
		(int)display_.height, window_flags);
	if (!window_) {
		SLOG_ERROR("spectre: SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	toolbar_.set_fullscreen(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN);
	apply_keyboard_grab(toolbar_.fullscreen());
	SLOG_DEBUG("spectre: window pixel density %.2f", pixel_density());

	// A no-op on Wayland (SDL_HINT_APP_ID above is what matters there).
	// kAppIconPng is assets/app-icon-128.png, embedded by CMakeLists.txt.
	// SDL's PNG loader arrived in 3.4; Debian 13 ships 3.2 and goes without
	// the icon.
#if SDL_VERSION_ATLEAST(3, 4, 0)
	SDL_IOStream *icon_io = SDL_IOFromConstMem(kAppIconPng, kAppIconPng_len);
	if (icon_io) {
		SDL_Surface *icon = SDL_LoadPNG_IO(icon_io, true);
		if (icon) {
			SDL_SetWindowIcon(window_, icon);
			SDL_DestroySurface(icon);
		}
	}
#endif

	// The samplers are built for the colour description SessionAccept
	// carried (display_.color): matrix, range and the depth's formats.
	if (!presenter_.init(window_, kDrmRenderNode, display_.color)) {
		SLOG_ERROR("spectre: VulkanPresenter::init failed");
		return false;
	}
	presenter_.set_display_size(display_.width, display_.height);
	// Decode (below) and the first network round trip both take a moment;
	// show the splash right away instead of leaving the swapchain's first
	// image undefined until on_decoded_frame's first real present().
	presenter_.present_splash();

#ifdef _WIN32
	// Which GPU to decode on: whichever one the presenter's Vulkan device
	// landed on, since that's the only place the decoded texture can be
	// shared to. kDrmRenderNode plays this role on Linux, but it's the
	// render node that picks the Vulkan device there, not the other way
	// round -- see VulkanDevice::pick_physical_device().
	if (const uint8_t *luid = presenter_.adapter_luid()) {
		decoder_.set_adapter_luid(luid);
	}
#endif

	bool refine = gdp::has_capability(session_info_.capabilities, gdp::kCapabilityRefine);
	HardwareDecode hw;
	hw.backend = options_.decoder;
	hw.vulkan = presenter_.decode_device();
	hw.compute = presenter_.compute_device();
	hw.bit_depth = display_.color.bit_depth;
#if defined(__APPLE__)
	// VideoToolbox frames are copied in, not imported: nothing to check.
	hw.native_importable = true;
#elif !defined(_WIN32)
	hw.drm_render_node = kDrmRenderNode;
	// A client with no GPU presents through lavapipe (VulkanDevice::
	// pick_physical_device); there is then no render node to decode on and
	// nothing to import a VASurface into, so Decoder won't even try.
	hw.native_importable = presenter_.supports_dmabuf_import();
	// NVIDIA has no VA-API of its own -- at best a third-party shim that can
	// open and then fail on the dmabuf export -- so never try it there;
	// Decoder moves on to Vulkan Video.
	if (presenter_.vendor_id() == kNvidiaVendorId) {
		hw.native_importable = false;
	}
#else
	hw.native_importable = presenter_.adapter_luid() != nullptr;
#endif
	if (!decoder_.open(session_info_.codec, refine, hw)) {
		SLOG_ERROR("spectre: Decoder::open failed");
		return false;
	}
	// Nothing on the refinement overlay plane survives a new decoder: whatever
	// it held belonged to a stream that no longer exists. A session without
	// refinement leaves it empty from here on.
	presenter_.clear_lossless_plane();
	tile_debug_.clear();

	apply_ui_scale();
	// SDL_CreateWindow's size is in logical units, so on a scaled desktop
	// the window above came out `density` times the stream in real pixels;
	// and the docked toolbar (sized off the UI font, just built) takes a
	// band off the top. Resize so one stream pixel is one screen pixel
	// below the toolbar. A window opened fullscreen (-f) keeps this as the
	// size it comes back to (SDL applies it on leaving fullscreen); the
	// docked band is added either way.
	snap_window();
	apply_cursor_scale();
	window_ready_ = true;
	// -A: whatever the window settles at while it first appears -- the
	// snap above, a compositor applying -f's fullscreen -- is the starting
	// point, not a resize. The resolution asked for at launch stays until
	// the user changes the window.
	own_resize_until_us_ = gdp::monotonic_us() + 2000000;
	take_follow_baseline();
	if (pending_cursor_) {
		const PendingCursor &c = *pending_cursor_;
		presenter_.set_cursor_shape(c.width, c.height, c.hotspot_x, c.hotspot_y, c.argb8888.data());
		pending_cursor_.reset();
	}

	stats_sample_.width = display_.width;
	stats_sample_.height = display_.height;
	stats_sample_.codec = session_info_.codec;
	stats_sample_.color = describe_color(display_.color);
	stats_sample_.encoder = session_info_.encoder;
	stats_sample_.network_profile = client_.network_profile();
	stats_sample_.decoder = decoder_.backend_name();
	stats_accum_.window_start_us = gdp::monotonic_us();
	return true;
}

void StreamSession::open_audio() {
	if (!audio_info_.valid) {
		return;
	}
	if (audio_decoder_.open(audio_info_)) {
		jitter_buffer_ = std::make_unique<JitterBuffer>(&audio_decoder_);
	}
	// SDL's audio thread pulls from the jitter buffer directly (see
	// jitter_buffer.hpp) -- nothing audio-related runs in the main loop
	// except push() from the network callback.
	if (jitter_buffer_ &&
		audio_player_.open(audio_info_.sample_rate_hz, audio_info_.channels,
			audio_info_.samples_per_frame_all_channels(),
			[this](int16_t *pcm_out) { return jitter_buffer_->pull(pcm_out); })) {
		client_.on_audio_frame = [this](uint16_t seq, uint32_t pts, const uint8_t *data, size_t len) {
			jitter_buffer_->push(seq, pts, data, len);
		};
		SLOG_INFO("spectre: audio %s, %u Hz, %u ch, %ums frames", audio_info_.codec.c_str(),
			audio_info_.sample_rate_hz, audio_info_.channels, audio_info_.frame_ms);
	} else {
		SLOG_ERROR("spectre: audio setup failed, continuing without audio");
	}
}

void StreamSession::open_microphone() {
	if (!client_.microphone_enabled()) {
		return;
	}
	// The capture thread calls straight into the connection:
	// send_microphone_packet() is safe from any thread.
	if (microphone_.open(client_.microphone_format(),
			[this](const uint8_t *packet, size_t len) { client_.send_microphone_packet(packet, len); })) {
		SLOG_INFO("spectre: microphone on, %u Hz, %u ch, %ums frames",
			client_.microphone_format().sample_rate_hz, client_.microphone_format().channels,
			client_.microphone_format().frame_ms);
	} else {
		SLOG_ERROR("spectre: no microphone available, continuing without one");
	}
}

void StreamSession::main_loop() {
	uint64_t next_stats_report_us = gdp::monotonic_us() + kStatsIntervalUs;

	while (running_ && !disconnected_) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			handle_event(event);
		}
		flush_gamepads();
#ifndef _WIN32
		if (g_logout_signalled) {
			g_logout_signalled = 0;
			SLOG_INFO("spectre: logout requested by signal");
			request_logout();
		}
#endif

		wait_for_notify(client_.notify_fd(), 4,
			{host_files_.running() ? host_files_.notify_fd() : -1, local_files_.notify_fd(),
				card_readers_ ? card_readers_->notify_fd() : -1});
		client_.dispatch();
		host_files_.dispatch();
		local_files_.dispatch();
		host_files_.tick(gdp::monotonic_us());
#ifdef _WIN32
		host_file_reads_->dispatch();
		host_file_reads_->tick(gdp::monotonic_us());
#endif
		present_pending_frame();

		uint64_t now_us = gdp::monotonic_us();
		if (now_us >= next_stats_report_us) {
			client_.send_stats_report();
			next_stats_report_us = now_us + kStatsIntervalUs;
		}
		if (stats_enabled_ && now_us - stats_accum_.window_start_us >= kStatsWindowUs) {
			// An idle desktop sends no frames, so the overlay would
			// otherwise freeze on the last busy window's numbers.
			update_stats(now_us);
			mark_ui_dirty();
		}
		// Outlines age out on a timer, and an idle desktop sends no frame
		// to redraw them away -- so retire them here too.
		if (tile_debug_.expire(now_us)) {
			mark_ui_dirty();
		}
		if (notices_.tick(now_us)) {
			mark_ui_dirty();
		}
		for (const PromptAnswer &answer : notices_.take_answers()) {
			auto it = prompt_handlers_.find(answer.id);
			if (it != prompt_handlers_.end()) {
				std::function<void(const PromptAnswer &)> done = std::move(it->second);
				prompt_handlers_.erase(it);
				done(answer);
			}
		}
		sync_prompt_mode();
		if (options_.demo_notices) {
			run_demo_notices(now_us);
		}
		files_.tick(now_us);
		update_upload_card(now_us);
		if (next_my_scan_us_ != 0 && now_us >= next_my_scan_us_) {
			rescan_my_folders();
		}
		if (follow_due_us_ != 0 && now_us >= follow_due_us_) {
			follow_due_us_ = 0;
			follow_window();
		}
		tick_pan(now_us);
		// A held key request keeps the host's FIDO client waiting, as a key
		// waiting for its touch does.
		if (client_.security_keys_enabled() && now_us >= next_key_keepalive_us_) {
			constexpr uint64_t kKeepaliveUs = 100000;
			next_key_keepalive_us_ = now_us + kKeepaliveUs;
			for (uint32_t i = 0; i < gdp::kMaxSecurityKeys; i++) {
				if (keys_[i] && key_policy_[i].holding()) {
					for (const gdp::KeyPacket &p : key_policy_[i].keepalives()) {
						client_.send_security_key_input(i, p.data(), p.size());
					}
				}
			}
		}
		if (client_.smartcards_enabled()) {
			if (!card_readers_) {
				card_readers_ = std::make_unique<SmartcardReaders>();
				if (!card_readers_->start()) {
					// Nothing will come of it; keep the empty one so it isn't
					// started again every pass.
					SLOG_INFO("spectre: smartcard: not forwarding readers");
				}
			}
			handle_smartcard_events();
		}
		if (client_.security_keys_enabled() && now_us >= next_key_check_us_) {
			// Cheap: SDL keeps the count from its own hotplug watch.
			constexpr uint64_t kKeyCheckUs = 500000;
			next_key_check_us_ = now_us + kKeyCheckUs;
			uint32_t count = SDL_hid_device_change_count();
			if (!keys_scanned_ || count != hid_change_count_) {
				keys_scanned_ = true;
				hid_change_count_ = count;
				rescan_security_keys();
			}
		}
		uint64_t raw_report_us = raw_report_us_.load(std::memory_order_relaxed);
		if (raw_report_us > gamepad_active_us_ && now_us - raw_report_us < kGamepadIdleHoldUs) {
			note_gamepad_activity();
		}
		update_idle_inhibit(now_us);
		// The scroll indicators fade on a clock, not on anything arriving.
		if (indicators_drawn_ && now_us - indicators_shown_us_ >= kIndicatorHoldUs) {
			mark_ui_dirty();
		}
		// One redraw for everything the events and network changed this
		// iteration (see mark_ui_dirty). A video frame presented after
		// dispatch() above already drew the current UI and cleared it.
		flush_ui();
	}
}

// --- network / decoder callbacks ---

void StreamSession::on_accepted(const SessionInfo &session, const DisplayInfo &display,
	const AudioInfo &audio) {
	accepted_ = true;
	session_info_ = session;
	display_ = display;
	audio_info_ = audio;
	share_folders();
}

void StreamSession::on_displays_changed(const DisplayInfo &display) {
	constexpr uint64_t kToastUs = 3000000;
	if (display.color != display_.color) {
		// The host reopened its encoder with another depth (or matrix):
		// the samplers follow (VulkanPresenter::set_color). PyroWave's
		// decoder is sized by its planes' depth too; the next keyframe
		// reopens it through the ordinary path only for FFmpeg codecs, so
		// a pyrowave session reopens the decoder here.
		SLOG_INFO("spectre: host video now %u-bit (was %u-bit)", display.color.bit_depth,
			display_.color.bit_depth);
		display_.color = display.color;
		stats_sample_.color = describe_color(display_.color);
		if (!presenter_.set_color(display_.color)) {
			SLOG_ERROR("spectre: presenter can't follow the new colour description, ending session");
			presenter_failed_ = true;
			running_ = false;
			return;
		}
		if (session_info_.codec == gdp::kVideoCodecPyrowave) {
			bool refine = gdp::has_capability(session_info_.capabilities, gdp::kCapabilityRefine);
			HardwareDecode hw;
			hw.backend = options_.decoder;
			hw.compute = presenter_.compute_device();
			hw.bit_depth = display_.color.bit_depth;
			decoder_.close();
			if (!decoder_.open(session_info_.codec, refine, hw)) {
				SLOG_ERROR("spectre: reopening the pyrowave decoder at %u bits failed, ending session",
					display_.color.bit_depth);
				running_ = false;
				return;
			}
		}
	}
	bool changed = display.width != display_.width || display.height != display_.height;
	if (changed) {
		SLOG_INFO("spectre: host output now %ux%u (was %ux%u)", display.width, display.height, display_.width,
			display_.height);
		display_ = display;
		// The crop for a padded stream (AV1) and everything mapping window
		// pixels onto the remote follow display_; the decoder and the
		// lossless plane follow the frames themselves.
		presenter_.set_display_size(display_.width, display_.height);
		stats_sample_.width = display_.width;
		stats_sample_.height = display_.height;
		// A fullscreen window doesn't change size, so nothing else would
		// rescale the pointer to the new stretch.
		apply_cursor_scale();
		if (actual_size_) {
			centre_pan_on_pointer();
			show_scroll_indicators();
		}
	}
	if (requested_width_ != 0) {
		std::string size = std::to_string(display_.width) + "x" + std::to_string(display_.height);
		bool got_it = display_.width == requested_width_ && display_.height == requested_height_;
		if (request_from_menu_) {
			notify(got_it ? NoticeLevel::kInfo : NoticeLevel::kWarning,
				got_it ? "Resolution " + size : "The host kept " + size, kToastUs, "resolution");
			if (changed) {
				snap_window(); // fullscreen stays fullscreen; this sets the size it comes back to
			}
			take_follow_baseline(); // the pick stands until the user changes the window
		} else if (!got_it) {
			SLOG_INFO("spectre: the host kept %s rather than follow the window's %ux%u", size.c_str(),
				requested_width_, requested_height_);
			refused_width_ = requested_width_;
			refused_height_ = requested_height_;
		}
		requested_width_ = requested_height_ = 0;
		// -A's own request: the window may have moved on while it was out.
		// Not after a menu pick, which the window has just snapped to.
		if (!request_from_menu_) {
			schedule_follow_window();
		}
	}
	mark_ui_dirty();
}

void StreamSession::request_resolution(uint32_t width, uint32_t height, bool from_menu) {
	requested_width_ = width;
	requested_height_ = height;
	request_from_menu_ = from_menu;
	SLOG_INFO("spectre: asking the host for %ux%u%s", width, height,
		from_menu ? "" : " (following the window)");
	client_.send_resolution_change(width, height, requested_scale(width, height));
}

void StreamSession::schedule_follow_window() {
	// Long enough that dragging a window edge sends one request when the
	// drag stops, not one per step of it.
	constexpr uint64_t kSettleUs = 400000;
	if (!options_.follow_window) {
		return;
	}
	uint64_t now_us = gdp::monotonic_us();
	if (now_us < own_resize_until_us_) {
		take_follow_baseline(); // spectre's own size, not the user's
		return;
	}
	follow_due_us_ = now_us + kSettleUs;
}

void StreamSession::take_follow_baseline() {
	VideoRect video = video_rect();
	follow_baseline_w_ = (uint32_t)video.w;
	follow_baseline_h_ = (uint32_t)video.h;
	follow_due_us_ = 0;
}

void StreamSession::follow_window() {
	// The host's limits (gdp-spec.md §7.2): even, and 320..8192 a side. A
	// window smaller than that keeps scaling down.
	constexpr uint32_t kMinSide = 320, kMaxSide = 8192;
	if (!options_.follow_window || !window_ready_ || logout_requested_) {
		return;
	}
	if (actual_size_) {
		return; // the window shows part of the picture; its size is not a request
	}
	if (requested_width_ != 0) {
		return; // on_displays_changed() reschedules once this one is answered
	}
	VideoRect video = video_rect();
	if ((uint32_t)video.w == follow_baseline_w_ && (uint32_t)video.h == follow_baseline_h_) {
		return; // the user hasn't changed the window since
	}
	follow_baseline_w_ = (uint32_t)video.w;
	follow_baseline_h_ = (uint32_t)video.h;
	uint32_t width = std::min((uint32_t)video.w, kMaxSide) & ~1u;
	uint32_t height = std::min((uint32_t)video.h, kMaxSide) & ~1u;
	if (width < kMinSide || height < kMinSide) {
		return;
	}
	if ((width == display_.width && height == display_.height) ||
		(width == refused_width_ && height == refused_height_)) {
		return;
	}
	request_resolution(width, height, false);
}

void StreamSession::screen_pixel_size(uint32_t *width, uint32_t *height) const {
	*width = *height = 0;
	SDL_DisplayID id = window_ ? SDL_GetDisplayForWindow(window_) : SDL_GetPrimaryDisplay();
	const SDL_DisplayMode *mode = id ? SDL_GetCurrentDisplayMode(id) : nullptr;
	if (!mode) {
		return;
	}
	// The mode's size is in screen coordinates; pixel_density turns it
	// into pixels (a 200%-scaled 4K panel is 1920x1080 at 2.0).
	float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
	uint32_t w = (uint32_t)std::lround(mode->w * density);
	uint32_t h = (uint32_t)std::lround(mode->h * density);
	// The host needs even sizes for 4:2:0 video.
	*width = w & ~1u;
	*height = h & ~1u;
}

double StreamSession::requested_scale(uint32_t width, uint32_t height) const {
	if (options_.scale) {
		return *options_.scale;
	}
	uint32_t screen_w = 0, screen_h = 0;
	screen_pixel_size(&screen_w, &screen_h);
	if (screen_w == 0 || (width & ~1u) != screen_w || (height & ~1u) != screen_h) {
		return 0.0;
	}
	// The window's display scale once there is a window; before it, the
	// same thing from the display: pixel density (Wayland, macOS) times
	// content scale (Windows' DPI scaling).
	float scale = 0.0f;
	if (window_) {
		scale = SDL_GetWindowDisplayScale(window_);
	} else if (SDL_DisplayID id = SDL_GetPrimaryDisplay()) {
		const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(id);
		float density = mode && mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
		scale = density * SDL_GetDisplayContentScale(id);
	}
	return scale > 0.0f ? (double)scale : 0.0;
}

uint32_t StreamSession::requested_refresh_mhz() const {
	if (options_.refresh_hz != 0) {
		return options_.refresh_hz * 1000;
	}
	SDL_DisplayID id = SDL_GetPrimaryDisplay();
	const SDL_DisplayMode *mode = id ? SDL_GetCurrentDisplayMode(id) : nullptr;
	return mode && mode->refresh_rate > 0.0f ? (uint32_t)std::lround(mode->refresh_rate * 1000.0f) : 60000;
}

void StreamSession::on_disconnected(const std::string &reason) {
	disconnected_ = true;
	disconnect_reason_ = reason;
	// The desktop's hold ends with the connection (gdp-spec.md §7.13).
	host_idle_inhibited_ = false;
	update_idle_inhibit(gdp::monotonic_us());
	files_.cancel_all();
	end_upload_card();
	local_files_.cancel_all();
	if (own_files_offer_ && sdl_initialized_) {
		SDL_ClearClipboardData(); // paths into a mount about to go
		own_files_offer_ = false;
	}
	host_files_.stop();
#ifdef _WIN32
	host_file_reads_->close(); // a paste under way fails at once
	host_files_clipboard_.withdraw();
#endif
}

bool StreamSession::on_video_frame(const uint8_t *data, size_t len) {
	if (!window_ready_) {
		return false; // reported to wraith as lost, see SessionClient::on_video_frame
	}
	stats_accum_.video_bytes += len;
	frame_decode_start_us_ = gdp::monotonic_us();
	lossless_applied_ = false;
	if (!decoder_.decode(data, len)) {
		return false;
	}
	// A layer the decoder split off but no picture came out to carry it
	// (a layer-only frame from wraith's idle pump, or the decoder holding
	// the picture back a call): apply it to the frame on screen now and
	// redraw. Tiles are settled content and a clear a frame early only
	// shows the lossy video underneath for that frame, so neither needs
	// to wait for the picture -- and waiting would lose the layer
	// outright if the picture never came.
	if (!lossless_applied_) {
		if (const gdp::RefineLayer *layer = decoder_.lossless_update()) {
			apply_lossless_layer(*layer, gdp::monotonic_us());
			mark_ui_dirty(); // flush_ui() redraws the last picture with the plane
		}
	}
	return true;
}

void StreamSession::apply_lossless_layer(const gdp::RefineLayer &layer, uint64_t now_us) {
	presenter_.apply_lossless_update(layer, display_.width, display_.height);
	// Outline what just changed, if -D asked for it. Done here rather
	// than inside the plane so the boxes are UI primitives drawn over
	// the tiles, leaving the lossless pixels themselves untouched.
	tile_debug_.add_layer(layer, now_us);
	lossless_applied_ = true;
}

void StreamSession::on_decoded_frame(AVFrame *frame) {
	uint64_t decode_done_us = gdp::monotonic_us();
	uint32_t decode_us = (uint32_t)(decode_done_us - frame_decode_start_us_);

	// The lossless refinement layer that came with this frame, if any
	// (Decoder::lossless_update()). Applied now, in stream order, so the
	// plane holds every layer up to the picture that is finally shown.
	if (const gdp::RefineLayer *layer = decoder_.lossless_update()) {
		apply_lossless_layer(*layer, decode_done_us);
	}

	// Held until dispatch() has handed over everything that arrived
	// (present_pending_frame()), so a burst shows only its newest picture.
	// A cloned reference, since the decoder reuses `frame`.
	if (pending_frame_) {
		av_frame_free(&pending_frame_);
		frames_skipped_++;
	}
	pending_frame_ = av_frame_clone(frame);
	pending_decode_us_ = decode_us;
	if (!pending_frame_) {
		return;
	}
	// A decoder that can't keep up with what arrives never lets dispatch()
	// run dry; don't let the screen freeze behind it.
	constexpr uint64_t kMaxHoldUs = 100'000;
	if (decode_done_us - last_present_us_ >= kMaxHoldUs) {
		present_pending_frame();
	}
}

void StreamSession::present_pending_frame() {
	if (!pending_frame_) {
		return;
	}
	if (frames_skipped_ >= 5) {
		SLOG_INFO("spectre: caught up on a burst of %u frames, showed the newest", frames_skipped_ + 1);
	}
	frames_skipped_ = 0;
	AVFrame *frame = pending_frame_;
	pending_frame_ = nullptr;

	uint64_t present_start_us = gdp::monotonic_us();
	if (stats_enabled_ && present_start_us - stats_accum_.window_start_us >= kStatsWindowUs) {
		update_stats(present_start_us);
	}
	tile_debug_.expire(present_start_us);

	rebuild_ui();
	ui_dirty_ = false;
	if (!presenter_.present(frame)) {
		if (presenter_.failed()) {
			// Device lost or similar: there's no next frame that will
			// fare better, so end the session instead of logging once
			// per decoded frame forever.
			SLOG_ERROR("spectre: presenter failed, ending session");
			presenter_failed_ = true;
			running_ = false;
		}
		// Otherwise just this frame was lost, already logged where it
		// happened; the next one may well succeed.
	}
	av_frame_free(&frame);
	last_present_us_ = gdp::monotonic_us();
	uint32_t present_us = (uint32_t)(last_present_us_ - present_start_us);
	client_.record_frame_timing(pending_decode_us_, present_us);

	stats_accum_.frames++;
	stats_accum_.decode_us += pending_decode_us_;
	stats_accum_.present_us += present_us;
	last_latency_ms_ = (double)(pending_decode_us_ + present_us) / 1000.0;
}

void StreamSession::on_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
	const uint8_t *argb8888) {
	if (!window_ready_) {
		pending_cursor_ = PendingCursor{width, height, hotspot_x, hotspot_y,
			std::vector<uint8_t>(argb8888, argb8888 + (size_t)width * height * 4)};
		return;
	}
	presenter_.set_cursor_shape(width, height, hotspot_x, hotspot_y, argb8888);
}

void StreamSession::on_cursor_position(double x, double y) {
	// Absolute mode already draws at the local pointer; a late report from
	// before the toggle would just jerk it away from there. Likewise while
	// the menu has temporarily taken the pointer local.
	if (!relative_mouse_ || !window_ready_ || menu_.visible() || prompt_mode_) {
		return;
	}
	remote_pointer_x_ = (float)x;
	remote_pointer_y_ = (float)y;
	if (actual_size_) {
		pan_to_show((float)(x * display_.width), (float)(y * display_.height));
	}
	VideoRect picture = picture_rect();
	last_cursor_x_ = picture.x + (float)(x * picture.w);
	last_cursor_y_ = picture.y + (float)(y * picture.h);
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, !menu_.visible() && !prompt_mode_);
	// Redraw now: in relative mode the overlay cursor is driven by these
	// reports, not by a local pointer, and present() only runs on a
	// decoded video frame -- without this the cursor visibly moves only
	// when the screen repaints (see flush_ui() and handle_mouse_motion()).
	mark_ui_dirty();
}

void StreamSession::on_clipboard_text(const std::string &utf8) {
	// wraith pushes its clipboard as soon as the session is accepted, which
	// can arrive before open_window() has brought SDL up -- and SDL's
	// clipboard is part of the video subsystem. Hold it rather than lose it.
	if (!sdl_initialized_ || !clipboard_ready_) {
		pending_clipboard_ = utf8;
		return;
	}
	apply_clipboard_text(utf8);
}

void StreamSession::flush_pending_clipboard() {
	if (!clipboard_ready_) {
		return;
	}
	if (pending_clipboard_) {
		apply_clipboard_text(*pending_clipboard_);
		pending_clipboard_.reset();
	}
	if (pending_files_offer_) {
		offer_files_on_clipboard(*pending_files_offer_);
		pending_files_offer_.reset();
	}
}

void StreamSession::apply_clipboard_text(const std::string &utf8) {
	// Recorded *before* the SDL call: SDL raises
	// SDL_EVENT_CLIPBOARD_UPDATE for its own SDL_SetClipboardText, and
	// that echo must not be forwarded back to wraith as a fresh local
	// copy (gdp/clipboard.hpp's ClipboardEcho).
	client_.note_clipboard_applied(utf8);
	if (!SDL_SetClipboardText(utf8.c_str())) {
		SLOG_ERROR("spectre: SDL_SetClipboardText failed: %s", SDL_GetError());
	}
}

// --- SDL events ---

// Gamepad input goes straight to SDL, so the local desktop never counts it
// as activity: hold the screen awake for a while after each use, then let
// its idle timer run again.
void StreamSession::note_gamepad_activity() {
	gamepad_active_us_ = gdp::monotonic_us();
	update_idle_inhibit(gamepad_active_us_);
}

// From main_loop() every pass, so a host's DesktopState that came before
// the window waits for it here.
void StreamSession::update_idle_inhibit(uint64_t now_us) {
	bool gamepad = gamepad_active_us_ != 0 && now_us - gamepad_active_us_ < kGamepadIdleHoldUs;
	bool want = host_idle_inhibited_ || gamepad;
	if (want == idle_inhibited_ || !sdl_initialized_) {
		return;
	}
	idle_inhibited_ = want;
	if (want) {
		SDL_DisableScreenSaver();
	} else {
		SDL_EnableScreenSaver();
	}
}

void StreamSession::handle_event(const SDL_Event &event) {
	// SDL has the serial it sets the clipboard with once its window has
	// had a key, a click or a touch (clipboard_ready_).
	if (!clipboard_ready_ && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
								 event.type == SDL_EVENT_FINGER_DOWN)) {
		clipboard_ready_ = true;
		flush_pending_clipboard();
	}
	switch (event.type) {
	case SDL_EVENT_QUIT: running_ = false; break;
	case SDL_EVENT_WINDOW_RESIZED: presenter_.notify_resized(); break;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		// Also the only event when just the pixel density changes (window
		// dragged to a monitor with a different scale): the swapchain
		// must follow the pixel size, not the logical one.
		presenter_.notify_resized();
		// The video is being stretched by a new amount, so the pointer
		// drawn on top of it is too.
		apply_cursor_scale();
		clamp_pan();
		schedule_follow_window();
		mark_ui_dirty();
		break;
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		// Dragged onto a monitor with a different OS scale: the chrome
		// follows it, the cursor doesn't (see apply_ui_scale).
		apply_ui_scale();
		apply_cursor_scale(); // the docked toolbar's height follows the font
		mark_ui_dirty();
		break;
	case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
	case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
		handle_fullscreen_changed(event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
		if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) {
			schedule_follow_window(); // -A: the screen is the video area now
		} else {
			// Back to a window at the remote's aspect, keeping the remote
			// size (including one picked while fullscreen).
			snap_window();
		}
		break;
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		stop_edge_push();
		toolbar_.handle_pointer_left();
		if (pointer_on_toolbar_) {
			set_pointer_on_toolbar(false);
		}
		if (!prompt_mode_) {
			notices_.handle_pointer_left();
			pointer_on_notice_ = false;
		}
		mark_ui_dirty();
		break;
	case SDL_EVENT_WINDOW_EXPOSED:
		// A local window dragged across spectre and back uncovers a
		// region present() never redraws on its own -- it only runs
		// from present_pending_frame, i.e. when a new network frame
		// decodes. Re-present the last frame (coalesced like any other
		// UI redraw) so the reveal doesn't sit stale until the next
		// real one happens to arrive.
		mark_ui_dirty();
		break;
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP: handle_key(event.key); break;
	case SDL_EVENT_MOUSE_MOTION: handle_mouse_motion(event.motion); break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP: handle_mouse_button(event.button); break;
	case SDL_EVENT_CLIPBOARD_UPDATE: handle_clipboard_update(event.clipboard); break;
	case SDL_EVENT_DROP_FILE: handle_drop_file(event.drop.data); break;
	case SDL_EVENT_MOUSE_WHEEL:
		if (menu_.visible() || pointer_on_toolbar_ || pointer_on_notice_ || prompt_mode_) {
			break; // the menu, toolbar or a notice has the pointer
		}
		// gdp-spec.md §8.2: positive horizontal is right (SDL's too),
		// positive vertical is down (SDL's is up).
		client_.send_pointer_axis(event.wheel.x, -event.wheel.y);
		break;
	case SDL_EVENT_GAMEPAD_ADDED: handle_gamepad_added(event.gdevice.which); break;
	case SDL_EVENT_GAMEPAD_REMOVED: handle_gamepad_removed(event.gdevice.which); break;
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		// A stick at rest still jitters a little; that isn't someone playing.
		if (event.gaxis.value > kGamepadActiveAxis || event.gaxis.value < -kGamepadActiveAxis) {
			note_gamepad_activity();
		}
		mark_gamepad_dirty(event.gaxis.which);
		break;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		// Forwarded even while the menu is up: the menu is driven by
		// keyboard and pointer, and a game shouldn't lose its controller
		// because the user glanced at the stats overlay.
		note_gamepad_activity();
		mark_gamepad_dirty(event.gbutton.which);
		break;
	default: break;
	}
}

// --- gamepads ---

// gdp/gamepad.hpp pins the wire order to SDL3's enums by value. If SDL
// ever renumbers, this build stops here rather than sending a wire order
// the host would misread.
static_assert((int)gdp::GamepadAxis::kLeftX == SDL_GAMEPAD_AXIS_LEFTX);
static_assert((int)gdp::GamepadAxis::kLeftY == SDL_GAMEPAD_AXIS_LEFTY);
static_assert((int)gdp::GamepadAxis::kRightX == SDL_GAMEPAD_AXIS_RIGHTX);
static_assert((int)gdp::GamepadAxis::kRightY == SDL_GAMEPAD_AXIS_RIGHTY);
static_assert((int)gdp::GamepadAxis::kLeftTrigger == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
static_assert((int)gdp::GamepadAxis::kRightTrigger == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
static_assert((int)gdp::GamepadAxis::kCount == SDL_GAMEPAD_AXIS_COUNT);
static_assert((int)gdp::GamepadButton::kSouth == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert((int)gdp::GamepadButton::kEast == SDL_GAMEPAD_BUTTON_EAST);
static_assert((int)gdp::GamepadButton::kWest == SDL_GAMEPAD_BUTTON_WEST);
static_assert((int)gdp::GamepadButton::kNorth == SDL_GAMEPAD_BUTTON_NORTH);
static_assert((int)gdp::GamepadButton::kBack == SDL_GAMEPAD_BUTTON_BACK);
static_assert((int)gdp::GamepadButton::kGuide == SDL_GAMEPAD_BUTTON_GUIDE);
static_assert((int)gdp::GamepadButton::kStart == SDL_GAMEPAD_BUTTON_START);
static_assert((int)gdp::GamepadButton::kLeftStick == SDL_GAMEPAD_BUTTON_LEFT_STICK);
static_assert((int)gdp::GamepadButton::kRightStick == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
static_assert((int)gdp::GamepadButton::kLeftShoulder == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
static_assert((int)gdp::GamepadButton::kRightShoulder == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
static_assert((int)gdp::GamepadButton::kDpadUp == SDL_GAMEPAD_BUTTON_DPAD_UP);
static_assert((int)gdp::GamepadButton::kDpadDown == SDL_GAMEPAD_BUTTON_DPAD_DOWN);
static_assert((int)gdp::GamepadButton::kDpadLeft == SDL_GAMEPAD_BUTTON_DPAD_LEFT);
static_assert((int)gdp::GamepadButton::kDpadRight == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert((int)gdp::GamepadButton::kTouchpad == SDL_GAMEPAD_BUTTON_TOUCHPAD);
static_assert((int)gdp::GamepadButton::kCount == SDL_GAMEPAD_BUTTON_COUNT);

namespace {

// Linux only: whether `instance_id` is one of ghost's own virtual pads.
// When spectre runs on the host itself and connects to the *same*
// account's session, the virtual device ghostseat creates for a
// forwarded pad is owned by that account -- so spectre would enumerate
// it as a second controller and forward it straight back into the
// session that is driving it. ghostseat marks its devices with a
// "ghost/" phys (host/ghostseat/src/devices.rs): on a uinput pad's input device, or as
// HID_PHYS on a raw controller's HID device, and that's what this reads,
// whichever node SDL found the controller by (eventN or hidrawN).
// Elsewhere the host is another machine and there's nothing to skip.
bool is_ghost_virtual_pad(SDL_JoystickID instance_id) {
#ifdef __linux__
	const char *path = SDL_GetJoystickPathForID(instance_id);
	if (!path) {
		return false;
	}
	const char *name = strrchr(path, '/');
	if (!name) {
		return false;
	}
	std::string dir;
	if (strncmp(name + 1, "event", 5) == 0) {
		dir = std::string("/sys/class/input/") + (name + 1) + "/device";
	} else if (strncmp(name + 1, "hidraw", 6) == 0) {
		dir = std::string("/sys/class/hidraw/") + (name + 1);
	} else {
		return false;
	}
	auto starts_ghost = [](const std::string &file, const char *key) {
		FILE *f = fopen(file.c_str(), "r");
		if (!f) {
			return false;
		}
		char line[256];
		bool ghost = false;
		size_t key_len = strlen(key);
		while (!ghost && fgets(line, sizeof(line), f)) {
			ghost = strncmp(line, key, key_len) == 0 && strncmp(line + key_len, "ghost/", 6) == 0;
		}
		fclose(f);
		return ghost;
	};
	// eventN/device is the input device (phys); one level further up,
	// and hidrawN/device, is the HID device (HID_PHYS in its uevent).
	return starts_ghost(dir + "/phys", "") || starts_ghost(dir + "/device/uevent", "HID_PHYS=");
#else
	(void)instance_id;
	return false;
#endif
}

} // namespace

void StreamSession::handle_gamepad_added(uint32_t instance_id) {
	if (!client_.gamepad_enabled()) {
		return;
	}
	if (is_ghost_virtual_pad(instance_id)) {
		return;
	}
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		if (gamepads_[i].used()) {
			continue;
		}
		if (!open_raw_controller(i, instance_id)) {
			open_standard_gamepad(i, instance_id);
		}
		return;
	}
	SLOG_INFO("spectre: gamepad: all %u slots in use, ignoring a new controller", gdp::kMaxGamepads);
}

bool StreamSession::open_raw_controller(uint32_t i, uint32_t instance_id) {
	if (!options_.raw_gamepads || !client_.hid_enabled()) {
		return false;
	}
	std::string why;
	std::unique_ptr<RawController> raw = RawController::open(SDL_GetJoystickPathForID(instance_id), &why);
	const char *sdl_name = SDL_GetGamepadNameForID(instance_id);
	if (!raw) {
		SLOG_INFO("spectre: gamepad: \"%s\" can't go raw (%s), forwarding it as a standard pad",
			sdl_name ? sdl_name : "?", why.c_str());
		return false;
	}
	const RawController::Identity &id = raw->identity();
	gdp::session::HidConnect connect;
	connect.set_pad_index(i);
	connect.set_name(id.name);
	connect.set_bus(id.bus);
	connect.set_vendor_id(id.vendor_id);
	connect.set_product_id(id.product_id);
	connect.set_version(id.version);
	connect.set_uniq(id.uniq);
	connect.set_report_descriptor(id.report_descriptor.data(), id.report_descriptor.size());
	client_.send_hid_connect(connect);
	SLOG_INFO("spectre: gamepad: \"%s\" (%04x:%04x, %s) forwarded raw as pad %u", id.name.c_str(),
		id.vendor_id, id.product_id, id.bus == 5 ? "Bluetooth" : "USB", i);
	// After the connect: the reports it starts sending follow it on the
	// input stream.
	raw->start(
		[this, i](const uint8_t *data, size_t len) {
			client_.send_hid_input(i, data, len);
			raw_report_us_.store(gdp::monotonic_us(), std::memory_order_relaxed);
		},
		[this, i](uint32_t request_id, bool get, bool failed, const uint8_t *data, size_t len) {
			if (get) {
				client_.send_hid_get_report_reply(i, request_id, failed, data, len);
			} else {
				client_.send_hid_set_report_reply(i, request_id, failed);
			}
		});
	gamepads_[i].raw = std::move(raw);
	gamepads_[i].id = instance_id;
	return true;
}

void StreamSession::open_standard_gamepad(uint32_t i, uint32_t instance_id) {
	SDL_Gamepad *pad = SDL_OpenGamepad(instance_id);
	if (!pad) {
		SLOG_ERROR("spectre: gamepad: could not open controller: %s", SDL_GetError());
		return;
	}
	const char *name = SDL_GetGamepadName(pad);
	gamepads_[i].pad = pad;
	gamepads_[i].id = instance_id;
	gamepads_[i].dirty = true;
	SLOG_INFO("spectre: gamepad: \"%s\" forwarded as pad %u", name ? name : "?", i);
	client_.send_gamepad_connect(i, name ? name : "");
}

void StreamSession::set_raw_gamepads(bool raw) {
	options_.raw_gamepads = raw;
	// Every forwarded controller is unplugged on the host and plugged in
	// again the new way; one that can't go raw lands on the standard path
	// as it would have when it first appeared.
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		GamepadSlot &slot = gamepads_[i];
		if (!slot.used() || (raw ? !slot.pad : !slot.raw)) {
			continue;
		}
		uint32_t instance_id = slot.id;
		if (slot.pad) {
			SDL_CloseGamepad(slot.pad);
		}
		slot = GamepadSlot{}; // a raw controller's threads stop here
		client_.send_gamepad_disconnect(i);
		if (!open_raw_controller(i, instance_id)) {
			open_standard_gamepad(i, instance_id);
		}
	}
}

void StreamSession::on_hid_rejected(uint32_t i, const std::string &reason) {
	if (i >= gdp::kMaxGamepads || !gamepads_[i].raw) {
		return;
	}
	SLOG_INFO("spectre: gamepad: the host refused pad %u raw (%s), forwarding it as a standard pad", i,
		reason.c_str());
	uint32_t instance_id = gamepads_[i].id;
	gamepads_[i] = GamepadSlot{};
	open_standard_gamepad(i, instance_id);
}

void StreamSession::rescan_security_keys() {
	constexpr uint64_t kToastUs = 4000000;
	std::vector<SecurityKey::Identity> found = SecurityKey::find();
	auto present = [&](const std::string &path) {
		for (const SecurityKey::Identity &id : found) {
			if (id.path == path) {
				return true;
			}
		}
		return false;
	};
	for (uint32_t i = 0; i < gdp::kMaxSecurityKeys; i++) {
		if (keys_[i] && !present(keys_[i]->identity().path)) {
			SLOG_INFO("spectre: security key \"%s\" (slot %u) unplugged", keys_[i]->identity().name.c_str(),
				i);
			withdraw_key_prompts(i);
			keys_[i].reset(); // its threads stop here
			client_.send_security_key_disconnect(i);
		}
	}
	for (auto it = refused_keys_.begin(); it != refused_keys_.end();) {
		it = present(*it) ? std::next(it) : refused_keys_.erase(it);
	}
	for (const SecurityKey::Identity &id : found) {
		bool forwarded = false;
		for (const auto &key : keys_) {
			forwarded = forwarded || (key && key->identity().path == id.path);
		}
		if (forwarded || refused_keys_.count(id.path)) {
			continue;
		}
		uint32_t i = 0;
		while (i < gdp::kMaxSecurityKeys && keys_[i]) {
			i++;
		}
		if (i == gdp::kMaxSecurityKeys) {
			SLOG_INFO("spectre: security key \"%s\": both slots in use, not forwarded", id.name.c_str());
			refused_keys_.insert(id.path);
			continue;
		}
		std::string why;
		std::unique_ptr<SecurityKey> key = SecurityKey::open(id, &why);
		if (!key) {
			SLOG_ERROR("spectre: security key \"%s\" can't be forwarded: %s", id.name.c_str(), why.c_str());
			refused_keys_.insert(id.path);
			continue;
		}
		client_.send_security_key_connect(i, id.name, id.vendor_id, id.product_id, id.version);
		SLOG_INFO("spectre: security key \"%s\" (%04x:%04x) forwarded as slot %u", id.name.c_str(),
			id.vendor_id, id.product_id, i);
		// After the connect: its packets follow it on the input stream.
		key->start([this, i](const uint8_t *packet, size_t len) {
			client_.send_security_key_input(i, packet, len);
			// A bound, in case the main loop stalls: what's dropped only
			// costs a delete prompt its passkey's name.
			constexpr size_t kMaxQueued = 4096;
			std::lock_guard<std::mutex> lock(key_replies_mutex_);
			if (key_replies_.size() < kMaxQueued) {
				key_replies_.emplace_back(i, std::string(reinterpret_cast<const char *>(packet), len));
			}
		});
		keys_[i] = std::move(key);
		key_attached_us_[i] = gdp::monotonic_us();
		key_policy_[i] = gdp::KeyPolicy(key_attached_us_[i], !options_.refuse_key_maintenance);
		notify(NoticeLevel::kInfo, "Security key forwarded: " + id.name, kToastUs);
		mark_ui_dirty();
	}
}

void StreamSession::relay_security_key_packet(uint32_t i, const std::string &packet) {
	// Long enough to find the key and touch it; the next request replaces it.
	constexpr uint64_t kToastUs = 10000000;
	// What the key said so far first: a delete names the passkey from the
	// listing's replies.
	drain_key_replies();
	gdp::KeyVerdict verdict;
	key_policy_[i].feed(reinterpret_cast<const uint8_t *>(packet.data()), packet.size(), gdp::monotonic_us(),
		&verdict);
	for (uint32_t cid : verdict.withdrawn) {
		withdraw_key_prompt(i, cid);
	}
	for (const gdp::KeyPacket &p : verdict.to_key) {
		keys_[i]->output(std::string(reinterpret_cast<const char *>(p.data()), p.size()));
	}
	// The client's own answer rides the input stream like the key's would.
	for (const gdp::KeyPacket &p : verdict.to_host) {
		client_.send_security_key_input(i, p.data(), p.size());
	}
	const std::string &name = keys_[i]->identity().name;
	if (verdict.refused) {
		SLOG_ERROR("spectre: security key \"%s\": the host tried to %s; refused", name.c_str(),
			verdict.refused_what.c_str());
		notify(NoticeLevel::kWarning, "The host tried to " + verdict.refused_what, kToastUs, "key-request",
			verdict.refused_why.empty() ? "Refused." : "Refused. " + verdict.refused_why);
		mark_ui_dirty();
		return;
	}
	if (verdict.ask) {
		ask_key_consent(i, verdict);
		return;
	}
	if (!verdict.notice) {
		return;
	}
	std::string message;
	switch (verdict.request.kind) {
	case gdp::KeyRequest::kSignIn:
		message = "Touch your security key to sign in to " + verdict.request.rp_id;
		break;
	case gdp::KeyRequest::kRegister:
		message = "Touch your security key to register it with " + verdict.request.rp_id;
		break;
	case gdp::KeyRequest::kSelect: message = "Touch your security key to select it"; break;
	case gdp::KeyRequest::kU2f:
		message = "The host wants your security key (U2F): touch it to approve";
		break;
	}
	SLOG_INFO("spectre: security key \"%s\": %s", name.c_str(), message.c_str());
	notify(NoticeLevel::kInfo, message, kToastUs, "key-request");
	mark_ui_dirty();
}

void StreamSession::ask_key_consent(uint32_t i, const gdp::KeyVerdict &verdict) {
	constexpr int kMaxKeyDenials = 3;
	constexpr uint64_t kPromptUs = 30000000;
	// A reset's prompt can't outlast the key's own window, but leaves at
	// least a moment to read it.
	constexpr uint64_t kMinResetPromptUs = 3000000;
	const std::string &name = keys_[i]->identity().name;
	const uint32_t cid = verdict.held_cid;
	const gdp::KeyConsent::Kind kind = verdict.consent.kind;
	gdp::KeyConsentText text = gdp::describe_consent(verdict.consent, name);
	if (key_denials_ >= kMaxKeyDenials) {
		SLOG_INFO("spectre: security key \"%s\": %s -- refused unasked after %d denials", name.c_str(),
			text.title.c_str(), key_denials_);
		answer_key_consent(i, cid, false, kind);
		notify(NoticeLevel::kWarning, "Refused: " + text.title, 10000000, "key-request",
			"You turned down three requests from this host. Reconnect to be asked again.");
		return;
	}
	PromptSpec spec;
	spec.level = NoticeLevel::kWarning;
	spec.title = text.title;
	spec.body = text.body;
	spec.allow_label = text.allow_label;
	spec.deny_label = text.deny_label;
	spec.timeout_us = kPromptUs;
	if (kind == gdp::KeyConsent::kReset) {
		uint64_t now = gdp::monotonic_us();
		uint64_t ends = key_attached_us_[i] + gdp::KeyPolicy::kResetWindowUs;
		spec.timeout_us = std::max(kMinResetPromptUs, ends > now ? ends - now : 0);
	}
	SLOG_INFO("spectre: security key \"%s\": asking: %s", name.c_str(), text.title.c_str());
	uint64_t id = ask(std::move(spec), [this, i, cid, kind](const PromptAnswer &answer) {
		key_prompts_.erase({i, cid});
		if (!answer.allowed) {
			key_denials_++;
		}
		answer_key_consent(i, cid, answer.allowed, kind);
	});
	key_prompts_[{i, cid}] = id;
}

void StreamSession::answer_key_consent(uint32_t i, uint32_t cid, bool allowed, gdp::KeyConsent::Kind kind) {
	if (i >= gdp::kMaxSecurityKeys || !keys_[i]) {
		return;
	}
	gdp::KeyVerdict v;
	if (allowed) {
		key_policy_[i].approve(cid, gdp::monotonic_us(), &v);
	} else {
		key_policy_[i].deny(cid, &v);
	}
	for (const gdp::KeyPacket &p : v.to_key) {
		keys_[i]->output(std::string(reinterpret_cast<const char *>(p.data()), p.size()));
	}
	for (const gdp::KeyPacket &p : v.to_host) {
		client_.send_security_key_input(i, p.data(), p.size());
	}
	SLOG_INFO("spectre: security key \"%s\": request %s", keys_[i]->identity().name.c_str(),
		allowed ? "allowed" : "refused");
	// The key now waits for a touch the host's FIDO client may not ask for.
	if (allowed && !v.to_key.empty() &&
		(kind == gdp::KeyConsent::kReset || kind == gdp::KeyConsent::kEnrolFingerprint)) {
		notify(NoticeLevel::kInfo,
			kind == gdp::KeyConsent::kReset ? "Touch your security key now to erase it"
											: "Touch your security key's sensor when the session asks",
			10000000, "key-request");
	}
}

void StreamSession::withdraw_key_prompt(uint32_t i, uint32_t cid) {
	auto it = key_prompts_.find({i, cid});
	if (it == key_prompts_.end()) {
		return;
	}
	notices_.cancel(it->second);
	prompt_handlers_.erase(it->second);
	key_prompts_.erase(it);
	sync_prompt_mode();
	mark_ui_dirty();
}

void StreamSession::withdraw_key_prompts(uint32_t i) {
	for (auto it = key_prompts_.begin(); it != key_prompts_.end();) {
		if (it->first.first == i) {
			notices_.cancel(it->second);
			prompt_handlers_.erase(it->second);
			it = key_prompts_.erase(it);
		} else {
			++it;
		}
	}
	sync_prompt_mode();
	mark_ui_dirty();
}

void StreamSession::drain_key_replies() {
	std::vector<std::pair<uint32_t, std::string>> replies;
	{
		std::lock_guard<std::mutex> lock(key_replies_mutex_);
		replies.swap(key_replies_);
	}
	for (const auto &[i, packet] : replies) {
		if (i < gdp::kMaxSecurityKeys && keys_[i]) {
			key_policy_[i].observe_key(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
		}
	}
}

void StreamSession::on_security_key_rejected(uint32_t i, const std::string &reason) {
	if (i >= gdp::kMaxSecurityKeys || !keys_[i]) {
		return;
	}
	std::string name = keys_[i]->identity().name;
	SLOG_ERROR("spectre: the host refused security key \"%s\" (%s)", name.c_str(), reason.c_str());
	refused_keys_.insert(keys_[i]->identity().path);
	withdraw_key_prompts(i);
	keys_[i].reset();
	notify(NoticeLevel::kWarning, "The host refused " + name, 5000000);
	mark_ui_dirty();
}

int StreamSession::card_slot(const std::string &reader) const {
	for (uint32_t i = 0; i < gdp::kMaxSmartcardReaders; i++) {
		if (card_slots_[i].reader == reader) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void StreamSession::handle_smartcard_events() {
	constexpr uint64_t kToastUs = 4000000;
	for (SmartcardReaders::Event &e : card_readers_->take_events()) {
		const int slot = card_slot(e.reader);
		switch (e.kind) {
		case SmartcardReaders::Event::kReaderAdded: {
			if (slot >= 0 || refused_readers_.count(e.reader)) {
				break;
			}
			uint32_t i = 0;
			while (i < gdp::kMaxSmartcardReaders && !card_slots_[i].reader.empty()) {
				i++;
			}
			if (i == gdp::kMaxSmartcardReaders) {
				SLOG_INFO("spectre: smartcard: \"%s\": both slots in use, not forwarded", e.reader.c_str());
				refused_readers_.insert(e.reader);
				break;
			}
			card_slots_[i].reader = e.reader;
			card_slots_[i].policy = gdp::CardPolicy(!options_.refuse_key_maintenance, options_.pass_unknown_cards);
			card_slots_[i].prompt = 0;
			client_.send_smartcard_reader_added(i, e.reader, e.card, e.atr);
			SLOG_INFO("spectre: smartcard: reader \"%s\" forwarded as slot %u%s", e.reader.c_str(), i,
				e.card ? ", with a card" : "");
			notify(NoticeLevel::kInfo, "Smart card reader forwarded: " + gdp::plain_reader_name(e.reader), kToastUs);
			mark_ui_dirty();
			break;
		}
		case SmartcardReaders::Event::kReaderRemoved:
			refused_readers_.erase(e.reader);
			if (slot >= 0) {
				SLOG_INFO("spectre: smartcard: reader \"%s\" (slot %d) unplugged", e.reader.c_str(), slot);
				end_card_slot(static_cast<uint32_t>(slot));
				client_.send_smartcard_reader_removed(static_cast<uint32_t>(slot));
			}
			break;
		case SmartcardReaders::Event::kCardInserted:
			if (slot >= 0) {
				card_slots_[slot].policy.reset();
				client_.send_smartcard_card_inserted(static_cast<uint32_t>(slot), e.atr);
			}
			break;
		case SmartcardReaders::Event::kCardRemoved:
			if (slot >= 0) {
				// The host answers what it had waiting on the card itself.
				if (card_slots_[slot].prompt) {
					notices_.cancel(card_slots_[slot].prompt);
					prompt_handlers_.erase(card_slots_[slot].prompt);
					card_slots_[slot].prompt = 0;
					sync_prompt_mode();
					mark_ui_dirty();
				}
				card_slots_[slot].policy.reset();
				card_readers_->release(e.reader);
				client_.send_smartcard_card_removed(static_cast<uint32_t>(slot));
			}
			break;
		case SmartcardReaders::Event::kPowered:
			if (slot >= 0) {
				if (e.error == gdp::session::SMARTCARD_ERROR_BUSY) {
					notify(NoticeLevel::kWarning, "Another program here is using " + gdp::plain_reader_name(e.reader),
						10000000, "card-busy",
						"Close it to use the card in the session. For GnuPG, run: gpgconf --kill scdaemon");
					mark_ui_dirty();
				}
				client_.send_smartcard_powered(static_cast<uint32_t>(slot), e.sequence, e.error, e.atr);
			}
			break;
		case SmartcardReaders::Event::kResponse:
			if (slot >= 0) {
				if (e.reselect_failed) {
					card_slots_[slot].policy.reset();
				} else if (e.error == gdp::session::SMARTCARD_ERROR_NONE) {
					card_slots_[slot].policy.transmitted(reinterpret_cast<const uint8_t *>(e.apdu.data()),
						e.apdu.size(), reinterpret_cast<const uint8_t *>(e.response.data()), e.response.size());
				}
				client_.send_smartcard_response(static_cast<uint32_t>(slot), e.sequence, e.error, e.response);
			}
			break;
		}
	}
}

void StreamSession::on_smartcard_power(uint32_t i, uint32_t sequence, int action) {
	if (i >= gdp::kMaxSmartcardReaders || card_slots_[i].reader.empty() || !card_readers_) {
		client_.send_smartcard_powered(i, sequence, gdp::session::SMARTCARD_ERROR_NO_CARD, "");
		return;
	}
	// Whatever was selected, whatever chain was under way, ends here.
	card_slots_[i].policy.reset();
	card_readers_->power(card_slots_[i].reader, sequence, action);
}

void StreamSession::on_smartcard_transmit(uint32_t i, uint32_t sequence, const std::string &apdu) {
	// Long enough to read; the next refusal replaces it.
	constexpr uint64_t kToastUs = 10000000;
	if (i >= gdp::kMaxSmartcardReaders || card_slots_[i].reader.empty() || !card_readers_) {
		client_.send_smartcard_response(i, sequence, gdp::session::SMARTCARD_ERROR_NO_CARD, "");
		return;
	}
	CardSlot &slot = card_slots_[i];
	gdp::CardVerdict v = slot.policy.check(reinterpret_cast<const uint8_t *>(apdu.data()), apdu.size());
	switch (v.action) {
	case gdp::CardVerdict::kPass:
		card_readers_->transmit(slot.reader, sequence, apdu, slot.policy.selected_aid());
		return;
	case gdp::CardVerdict::kRefuse: {
		const char sw[2] = {static_cast<char>(v.status >> 8), static_cast<char>(v.status & 0xff)};
		client_.send_smartcard_response(i, sequence, gdp::session::SMARTCARD_ERROR_NONE, std::string(sw, 2));
		if (v.notify) {
			SLOG_ERROR("spectre: smartcard: \"%s\": the host tried to %s; refused", slot.reader.c_str(),
				v.refused_what.c_str());
			notify(NoticeLevel::kWarning, "The host tried to " + v.refused_what, kToastUs, "card-request", "Refused.");
			mark_ui_dirty();
		} else {
			SLOG_INFO("spectre: smartcard: \"%s\": refused: %s", slot.reader.c_str(), v.refused_what.c_str());
		}
		return;
	}
	case gdp::CardVerdict::kAsk: ask_card_consent(i, sequence, apdu, v.consent); return;
	}
}

void StreamSession::ask_card_consent(uint32_t i, uint32_t sequence, const std::string &apdu,
	const gdp::CardConsent &consent) {
	constexpr int kMaxDenials = 3;
	constexpr uint64_t kPromptUs = 30000000;
	const std::string reader = card_slots_[i].reader;
	gdp::CardConsentText text = gdp::describe_card_consent(consent, gdp::plain_reader_name(reader));
	auto refuse = [this, i, sequence] {
		const char sw[2] = {static_cast<char>(gdp::kSwSecurityNotSatisfied >> 8),
			static_cast<char>(gdp::kSwSecurityNotSatisfied & 0xff)};
		client_.send_smartcard_response(i, sequence, gdp::session::SMARTCARD_ERROR_NONE, std::string(sw, 2));
	};
	if (key_denials_ >= kMaxDenials) {
		SLOG_INFO("spectre: smartcard: \"%s\": %s -- refused unasked after %d denials", reader.c_str(),
			text.what.c_str(), key_denials_);
		refuse();
		notify(NoticeLevel::kWarning, "Refused: " + text.title, 10000000, "card-request",
			"You turned down three requests from this host. Reconnect to be asked again.");
		mark_ui_dirty();
		return;
	}
	PromptSpec spec;
	spec.level = NoticeLevel::kWarning;
	spec.title = text.title;
	spec.body = text.body;
	spec.allow_label = text.allow_label;
	spec.deny_label = text.deny_label;
	spec.timeout_us = kPromptUs;
	SLOG_INFO("spectre: smartcard: \"%s\": asking: %s", reader.c_str(), text.what.c_str());
	card_slots_[i].prompt = ask(std::move(spec), [this, i, sequence, apdu, reader, refuse](const PromptAnswer &answer) {
		if (card_slots_[i].reader != reader) {
			return; // the reader went while the prompt was up
		}
		card_slots_[i].prompt = 0;
		SLOG_INFO("spectre: smartcard: \"%s\": request %s", reader.c_str(), answer.allowed ? "allowed" : "refused");
		if (!answer.allowed) {
			key_denials_++;
			refuse();
			return;
		}
		card_readers_->transmit(reader, sequence, apdu, card_slots_[i].policy.selected_aid());
	});
}

void StreamSession::end_card_slot(uint32_t i) {
	CardSlot &slot = card_slots_[i];
	if (slot.prompt) {
		notices_.cancel(slot.prompt);
		prompt_handlers_.erase(slot.prompt);
		sync_prompt_mode();
		mark_ui_dirty();
	}
	if (card_readers_ && !slot.reader.empty()) {
		card_readers_->release(slot.reader);
	}
	slot = CardSlot{};
}

void StreamSession::on_smartcard_rejected(uint32_t i, const std::string &reason) {
	if (i >= gdp::kMaxSmartcardReaders || card_slots_[i].reader.empty()) {
		return;
	}
	const std::string reader = card_slots_[i].reader;
	SLOG_ERROR("spectre: smartcard: the host refused reader \"%s\" (%s)", reader.c_str(), reason.c_str());
	refused_readers_.insert(reader);
	end_card_slot(i);
	notify(NoticeLevel::kWarning, "The host refused " + gdp::plain_reader_name(reader), 5000000);
	mark_ui_dirty();
}

void StreamSession::handle_gamepad_removed(uint32_t instance_id) {
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		if (!gamepads_[i].used() || gamepads_[i].id != instance_id) {
			continue;
		}
		if (gamepads_[i].pad) {
			SDL_CloseGamepad(gamepads_[i].pad);
		}
		gamepads_[i] = GamepadSlot{}; // a raw controller's threads stop here
		SLOG_INFO("spectre: gamepad: pad %u removed", i);
		client_.send_gamepad_disconnect(i);
		return;
	}
}

void StreamSession::mark_gamepad_dirty(uint32_t instance_id) {
	for (GamepadSlot &slot : gamepads_) {
		if (slot.pad && slot.id == instance_id) {
			slot.dirty = true;
			return;
		}
	}
}

void StreamSession::flush_gamepads() {
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		GamepadSlot &slot = gamepads_[i];
		if (!slot.pad || !slot.dirty) {
			continue;
		}
		slot.dirty = false;
		gdp::GamepadSnapshot state;
		for (size_t a = 0; a < gdp::kGamepadAxisCount; a++) {
			// SDL gives int16: sticks -32768..32767, triggers 0..32767.
			// Dividing by 32767 puts both on the wire's -1..1 / 0..1 with
			// full deflection landing exactly on 1.
			state.axes[a] =
				std::clamp(SDL_GetGamepadAxis(slot.pad, (SDL_GamepadAxis)a) / 32767.0f, -1.0f, 1.0f);
		}
		for (size_t b = 0; b < gdp::kGamepadButtonCount; b++) {
			state.buttons[b] = SDL_GetGamepadButton(slot.pad, (SDL_GamepadButton)b);
		}
		client_.send_gamepad_state(i, state);
	}
}

void StreamSession::close_gamepads() {
	for (GamepadSlot &slot : gamepads_) {
		if (slot.pad) {
			SDL_CloseGamepad(slot.pad);
		}
		slot = GamepadSlot{};
	}
}

void StreamSession::handle_clipboard_update(const SDL_ClipboardEvent &event) {
	// Lengths and reasons only, never the text: it may be a password.
	if (event.owner) {
		// Our own SDL_SetClipboardText or files offer: not a local copy.
		SLOG_DEBUG("clipboard: update is our own write, ignored");
		return;
	}
#ifdef _WIN32
	if (HostFilesClipboard::on_clipboard()) {
		// The host's files, which OLE rather than SDL put there: no owner.
		return;
	}
#endif
	own_files_offer_ = false;
	if (!client_.clipboard_enabled()) {
		SLOG_DEBUG("clipboard: update ignored, the session has no clipboard");
		return;
	}
	if (announce_local_files()) {
		SLOG_DEBUG("clipboard: update holds files, announced as files");
		return;
	}
	if (!SDL_HasClipboardText()) {
		SLOG_DEBUG("clipboard: update holds no text (%d types)", event.num_mime_types);
		return;
	}
	// The text variants (rather than the generic SDL_SetClipboardData) do
	// the whole text mime set internally and hand back a plain buffer, so
	// there's no owned buffer or cleanup callback to manage -- and they
	// work on X11, Wayland, Windows and macOS alike.
	char *text = SDL_GetClipboardText();
	if (!text) {
		SLOG_DEBUG("clipboard: update's text couldn't be read");
		return;
	}
	// An unfocused window on Wayland reads "", which is "nothing readable
	// here", not a request to clear the remote clipboard -- send_clipboard
	// drops it either way.
	client_.send_clipboard(text);
	SDL_free(text);
}

#ifndef _WIN32
namespace {

// Where the host's copied files are mounted: this process's own folder
// under the user's runtime directory.
std::string host_files_directory() {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	std::string base = runtime && *runtime ? std::string(runtime) + "/spectre"
										   : "/tmp/spectre-" + std::to_string(getuid());
	return base + "/" + std::to_string(getpid());
}

} // namespace
#endif

void StreamSession::on_host_file_list(const gdp::session::FileList &list) {
#ifdef _WIN32
	// No FUSE: Explorer's virtual files, read from the host when pasted. A
	// list with no files leaves what we offered, as below.
	if (list.files_size() > 0) {
		host_files_clipboard_.offer(list, client_.file_clipboard_enabled());
	}
#else
	if (!host_files_.running()) {
		if (list.files_size() == 0 || host_files_failed_) {
			return;
		}
		std::string dir = host_files_directory();
		std::string error = "this build has no FUSE";
		if (!gdp::RemoteFiles::supported() || dir.empty() || !host_files_.start(dir, &error)) {
			// Once: a host copy then simply doesn't reach this client.
			SLOG_ERROR("files: the host's copied files can't be pasted here: %s", error.c_str());
			host_files_failed_ = true;
			return;
		}
		SLOG_INFO("files: the host's copied files are mounted at %s", dir.c_str());
	}
	std::vector<std::string> paths = host_files_.add_list(list, client_.file_clipboard_enabled());
	if (paths.empty()) {
		// The host's clipboard holds something else (no files). What we
		// offered stays until something replaces it -- the host's text,
		// which comes right behind this, or a local copy -- as text does
		// (gdp-spec.md §7.9: no "clear"). Clearing it here would empty the
		// clipboard for a moment, and a clipboard manager that never lets
		// it be empty (Klipper) would put its previous entry back over the
		// text that follows, and that entry would go to the host as a copy.
		pending_files_offer_.reset();
		return;
	}
	if (!sdl_initialized_ || !clipboard_ready_) {
		pending_files_offer_ = paths;
		return;
	}
	offer_files_on_clipboard(paths);
#endif
}

void StreamSession::offer_files_on_clipboard(const std::vector<std::string> &paths) {
	offered_files_ = paths;
	offered_payloads_.clear();
	// GTK file managers' own format first, then the standard one, then the
	// paths as text for anything else.
	std::vector<const char *> mimes;
	for (const std::string &m : gdp::copied_files_offer_mime_types()) {
		mimes.push_back(m.c_str());
	}
	auto data = [](void *self, const char *mime, size_t *size) -> const void * {
		return static_cast<StreamSession *>(self)->clipboard_files_data(mime, size);
	};
	auto cleanup = [](void *self) { static_cast<StreamSession *>(self)->own_files_offer_ = false; };
	if (SDL_SetClipboardData(data, cleanup, this, mimes.data(), mimes.size())) {
		own_files_offer_ = true;
		SLOG_INFO("files: %zu of the host's copied file(s) on the clipboard", paths.size());
	} else {
		SLOG_ERROR("files: SDL_SetClipboardData failed: %s", SDL_GetError());
	}
}

const void *StreamSession::clipboard_files_data(const char *mime, size_t *size) {
	*size = 0;
	if (!mime) {
		return nullptr;
	}
	auto it = offered_payloads_.find(mime);
	if (it == offered_payloads_.end()) {
		it = offered_payloads_.emplace(mime, gdp::copied_files_payload(mime, offered_files_)).first;
	}
	*size = it->second.size();
	return it->second.data();
}

bool StreamSession::announce_local_files() {
	if (!client_.file_clipboard_enabled()) {
		return false;
	}
	bool holds_files = false;
	std::vector<std::string> copied = local_copied_files(&holds_files);
	if (!holds_files) {
		if (local_files_.has_list()) {
			// The files are gone from it.
			local_files_.set_list({}, [this](const gdp::session::FileList &list) { client_.send_file_list(list); });
		}
		return false;
	}
	std::vector<std::string> paths;
	for (std::string &p : copied) {
		// Paths into our own mount are the host's files coming back (a
		// clipboard manager took our offer over), not a local copy -- and
		// reading them from this thread would wait on ourselves.
		if (!host_files_.contains(p)) {
			paths.push_back(std::move(p));
		}
	}
	if (paths.empty()) {
		return true; // nothing local in it: neither files nor their paths go
	}
	// Read off this thread (a big folder, a slow drive), then sent.
	local_files_.set_list(paths, [this](const gdp::session::FileList &list) {
		client_.send_file_list(list);
		SLOG_INFO("files: %d copied file(s) offered to the host",
			(int)std::count_if(list.files().begin(), list.files().end(),
				[](const gdp::session::FileListEntry &e) { return e.parent() == 0; }));
	});
	return true;
}

void StreamSession::share_folders() {
	if (options_.shared_folders.empty() && !options_.share_my_files) {
		return;
	}
	if (!client_.file_share_enabled()) {
		SLOG_INFO("files: the host doesn't take shared folders");
		return;
	}
	for (const std::string &folder : options_.shared_folders) {
		std::string name;
		uint64_t id = local_files_.add_share(folder, options_.share_hidden, &name);
		if (!id) {
			SLOG_ERROR("files: can't share %s: not a folder", folder.c_str());
			continue;
		}
		client_.send_file_share(id, name, false);
		SLOG_INFO("files: sharing %s with the host as \"%s\"", folder.c_str(), name.c_str());
	}
	if (options_.share_my_files) {
		rescan_my_folders();
	}
}

void StreamSession::rescan_my_folders() {
	constexpr uint64_t kRescanUs = 3000000;
	next_my_scan_us_ = gdp::monotonic_us() + kRescanUs;
	std::vector<MyFolder> now = my_folders();
	std::set<std::string> present;
	for (const MyFolder &folder : now) {
		present.insert(folder.path);
		if (my_shares_.count(folder.path)) {
			continue;
		}
		std::string name = folder.name;
		uint64_t id = local_files_.add_share(folder.path, options_.share_hidden, &name);
		if (!id) {
			continue;
		}
		my_shares_[folder.path] = id;
		client_.send_file_share(id, name, false);
		SLOG_INFO("files: sharing %s with the host as \"%s\"", folder.path.c_str(), name.c_str());
	}
	for (auto it = my_shares_.begin(); it != my_shares_.end();) {
		if (present.count(it->first)) {
			++it;
			continue;
		}
		local_files_.remove_share(it->second);
		client_.send_file_share(it->second, "", true);
		SLOG_INFO("files: %s is gone; no longer shared", it->first.c_str());
		it = my_shares_.erase(it);
	}
}

void StreamSession::request_host_file(uint64_t id, const gdp::RemoteFiles::Target &target, uint64_t offset,
	bool listing) {
	std::unique_ptr<gdp::Stream> stream = open_host_file_stream(target.list_id, target.index, offset, listing);
	if (!stream) {
		host_files_.fail(id, EIO);
		return;
	}
	host_files_.answer(id, std::move(stream));
}

std::unique_ptr<gdp::Stream> StreamSession::open_host_file_stream(uint64_t list_id, uint32_t index, uint64_t offset,
	bool listing) {
	std::unique_ptr<gdp::Stream> stream = client_.open_file_stream();
	if (!stream) {
		return nullptr;
	}
	gdp::session::FileOpen open;
	if (listing) {
		open.mutable_listing()->set_list_id(list_id);
		open.mutable_listing()->set_index(index);
	} else {
		open.mutable_request()->set_list_id(list_id);
		open.mutable_request()->set_index(index);
		open.mutable_request()->set_offset(offset);
	}
	gdp::session::StreamOpen stream_open;
	stream_open.set_kind(gdp::kStreamKindFile);
	stream_open.set_params(open.SerializeAsString());
	std::vector<uint8_t> frame;
	gdp::encode_frame(stream_open, &frame);
	stream->send(frame.data(), frame.size(), true); // the request is all this side sends
	return stream;
}

void StreamSession::handle_drop_file(const char *path) {
	if (!path) {
		return;
	}
	if (!client_.file_enabled()) {
		notify(NoticeLevel::kWarning, "This host doesn't take files", 3000000);
		mark_ui_dirty();
		return;
	}
	std::string error;
	bool new_batch = files_.pending() == 0;
	if (!files_.upload(path, &error)) {
		std::string name = path;
		size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos) {
			name.erase(0, slash + 1);
		}
		notify(NoticeLevel::kError, name + ": " + error, 4000000);
	} else if (new_batch) {
		upload_started_us_ = gdp::monotonic_us();
		upload_rate_.reset();
	}
	mark_ui_dirty();
}

void StreamSession::update_upload_card(uint64_t now_us) {
	// Shown only once a batch has gone on this long, so a small file
	// doesn't flash a card before its "Sent" toast.
	constexpr uint64_t kCardDelayUs = 500000;
	constexpr uint64_t kCardRefreshUs = 250000;
	for (uint64_t id : notices_.take_cancels()) {
		if (id == upload_card_) {
			files_.cancel(); // on_batch_done takes the card down
		}
	}
	if (files_.pending() == 0) {
		return;
	}
	UploadProgress p = files_.progress();
	upload_rate_.sample(now_us, p.done);
	if (upload_card_ == 0 && now_us - upload_started_us_ < kCardDelayUs) {
		return;
	}
	if (now_us < next_upload_card_us_) {
		return;
	}
	next_upload_card_us_ = now_us + kCardRefreshUs;
	ProgressSpec card = upload_card(p, upload_rate_.bytes_per_s());
	if (upload_card_ == 0) {
		upload_card_ = notices_.progress(std::move(card), now_us);
	} else {
		notices_.update_progress(upload_card_, std::move(card));
	}
}

void StreamSession::end_upload_card() {
	if (upload_card_ != 0) {
		notices_.end_progress(upload_card_);
		upload_card_ = 0;
	}
	next_upload_card_us_ = 0;
}

void StreamSession::handle_key(const SDL_KeyboardEvent &key) {
	// A prompt owns the keyboard ahead of everything, the menu chord
	// included: nothing is forwarded, and the menu can't open over it.
	if (prompt_mode_ || notices_.prompt_visible()) {
		if (key.down && !key.repeat) {
			notices_.handle_key(key.scancode, gdp::monotonic_us());
			mark_ui_dirty();
		}
		return;
	}
	// The menu chord first, so it works whether or not the menu is up
	// (it toggles). Checked on the key-down that completes it: SDL has
	// already recorded this key in its keyboard state by the time the
	// event is delivered, so all_held() sees the whole chord. The chord's
	// earlier keys were already forwarded as pressed -- open_menu()
	// releases them remotely -- and the completing key is never
	// forwarded at all.
	if (key.down && !key.repeat && options_.menu_hotkey.involves(key.scancode) &&
		options_.menu_hotkey.all_held(SDL_GetKeyboardState(nullptr))) {
		if (menu_.visible()) {
			close_menu();
		} else {
			open_menu();
		}
		return;
	}
	if (menu_.visible()) {
		// The menu owns the keyboard: nothing is forwarded, key-ups
		// included (everything held was released remotely at open).
		if (key.down) {
			apply_menu_action(menu_.handle_key(key.scancode));
		}
		return;
	}
	bool capture_mouse_key = key.scancode == SDL_SCANCODE_SCROLLLOCK;
	bool fullscreen_key =
		(key.mod & SDL_KMOD_CTRL) && (key.mod & SDL_KMOD_SHIFT) && key.scancode == SDL_SCANCODE_F11;
#ifdef __APPLE__
	capture_mouse_key = capture_mouse_key || is_ctrl_cmd_chord(key, SDL_SCANCODE_G);
	fullscreen_key = fullscreen_key || is_ctrl_cmd_chord(key, SDL_SCANCODE_F);
#endif
	if (capture_mouse_key) {
		// Local-only hotkey: never forwarded. Captures or releases the
		// mouse -- relative motion for mouselook (toggle_capture_mouse()).
		// Scroll Lock rather than a Ctrl+Alt+Fn combo: the Linux kernel's
		// VT switcher intercepts those below any application, whichever
		// Ctrl/Alt is held.
		if (key.down && !key.repeat) {
			toggle_capture_mouse();
		}
		return;
	}
	if (fullscreen_key) {
		// Local-only hotkey: never forwarded. Toggles the spectre window
		// between windowed and fullscreen.
		if (key.down && !key.repeat) {
			toggle_fullscreen();
		}
		return;
	}
	if (key.repeat) {
		// Real Wayland clients self-repeat from one key-down plus the
		// keymap's repeat_info; forwarding SDL's own repeat events would
		// double it up.
		return;
	}
	if (key.down) {
		forwarded_keys_.insert((uint32_t)key.scancode);
	} else if (forwarded_keys_.erase((uint32_t)key.scancode) == 0) {
		// Never forwarded as pressed (it went down while the menu had the
		// keyboard, or open_menu() already released it): a stray release
		// the remote would only see as a key it never knew was down.
		return;
	}
	// SDL_Scancode's values are literally USB HID usage IDs
	// (SDL_scancode.h's own doc comment) -- no translation table needed,
	// unlike wraith's evdev-facing side.
	client_.send_key((uint32_t)key.scancode, key.down);
}

void StreamSession::handle_mouse_motion(const SDL_MouseMotionEvent &motion) {
	if (prompt_mode_) {
		// As for the menu: the pointer is local and drives the prompt.
		move_local_cursor(motion.x, motion.y);
		notices_.handle_pointer_motion(last_cursor_x_, last_cursor_y_);
		return;
	}
	if (menu_.visible()) {
		// Pointer is local while the menu is up (relative mode was
		// suspended in open_menu()): move the overlay, hover the rows,
		// forward nothing.
		move_local_cursor(motion.x, motion.y);
		apply_menu_action(menu_.handle_pointer_motion(last_cursor_x_, last_cursor_y_));
		return;
	}
	if (relative_mouse_) {
		client_.send_pointer_motion(motion.xrel * prefs_.mouse_sensitivity,
			motion.yrel * prefs_.mouse_sensitivity, false);
		return;
	}
	pointer_moved_to(motion.x, motion.y);
}

void StreamSession::pointer_moved_to(float x, float y) {
	float density = pixel_density();
	// A drag that started on the remote stays the remote's, even across
	// the toolbar. SDL's live button state as well as our own record, so
	// a release lost to a focus change can't leave the toolbar dead.
	bool dragging = (forwarded_buttons_ & SDL_GetMouseState(nullptr, nullptr)) != 0;
	bool on_toolbar = !dragging && toolbar_.handle_pointer_motion(x * density, y * density);
	// A toast under the pointer is spectre's too, so whatever it covers on
	// the remote is one click (dismissing it) away rather than unreachable.
	bool on_notice = false;
	if (on_toolbar || dragging) {
		notices_.handle_pointer_left();
	} else {
		on_notice = notices_.handle_pointer_motion(x * density, y * density);
	}
	if (on_toolbar != pointer_on_toolbar_ || on_notice != pointer_on_notice_) {
		pointer_on_notice_ = on_notice;
		set_pointer_on_toolbar(on_toolbar || on_notice);
		pointer_on_toolbar_ = on_toolbar;
	}
	if (on_toolbar || on_notice) {
		stop_edge_push();
		mark_ui_dirty(); // hover highlight
		return;
	}
	move_local_cursor(x, y);
	send_absolute_pointer();
	update_edge_push(last_cursor_x_, last_cursor_y_);
}

void StreamSession::send_absolute_pointer() {
	VideoRect picture = picture_rect();
	if (picture.w <= 0 || picture.h <= 0) {
		return;
	}
	remote_pointer_x_ = std::clamp((last_cursor_x_ - picture.x) / picture.w, 0.0f, 1.0f);
	remote_pointer_y_ = std::clamp((last_cursor_y_ - picture.y) / picture.h, 0.0f, 1.0f);
	client_.send_pointer_motion(remote_pointer_x_, remote_pointer_y_, true);
}

void StreamSession::set_pointer_on_toolbar(bool on) {
	pointer_on_toolbar_ = on;
	if (on) {
		// The overlay is the *remote's* pointer, in whatever shape the
		// remote last set (an I-beam, or nothing at all for a game) --
		// spectre's own chrome gets the local one.
		SDL_ShowCursor();
		presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, false);
	} else {
		SDL_HideCursor();
		presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, true);
	}
	mark_ui_dirty();
}

void StreamSession::handle_fullscreen_changed(bool fullscreen) {
	if (!fullscreen && options_.kiosk) {
		// Something outside spectre took the window out of fullscreen
		// (a desktop keybinding, say): put it back.
		SLOG_INFO("spectre: kiosk: window left fullscreen, restoring it");
		SDL_SetWindowFullscreen(window_, true);
	}
	toolbar_.set_fullscreen(fullscreen);
	apply_keyboard_grab(fullscreen);
	apply_cursor_scale();
	mark_ui_dirty();
	// The toolbar just moved (docked <-> floating, and the floating one
	// starts hidden): re-decide who has the pointer where it now sits,
	// rather than waiting for it to move.
	if (!relative_mouse_ && !menu_.visible()) {
		float x = 0.0f, y = 0.0f;
		SDL_GetMouseState(&x, &y);
		pointer_moved_to(x, y);
	}
}

void StreamSession::handle_mouse_button(const SDL_MouseButtonEvent &button) {
	const uint32_t mask = SDL_BUTTON_MASK(button.button);
	uint32_t btn = sdl_button_to_btn_code(button.button);
	if (!button.down) {
		if (button.button == SDL_BUTTON_LEFT) {
			menu_.handle_pointer_release(); // ends a slider drag, if one
		}
		// Only releases of forwarded presses go out -- including one held
		// down from before the menu opened, so the remote never keeps it.
		if ((forwarded_buttons_ & mask) && btn != 0) {
			forwarded_buttons_ &= ~mask;
			client_.send_pointer_button(btn, false);
		}
		return;
	}
	float density = pixel_density();
	if (prompt_mode_) {
		if (button.button == SDL_BUTTON_LEFT) {
			notices_.handle_pointer_press(button.x * density, button.y * density, gdp::monotonic_us());
			mark_ui_dirty();
		}
		return;
	}
	if (menu_.visible()) {
		if (button.button == SDL_BUTTON_LEFT) {
			apply_menu_action(menu_.handle_pointer_press(button.x * density, button.y * density));
		}
		return;
	}
	if (pointer_on_notice_ && !relative_mouse_) {
		// A click on a toast dismisses it; it never reaches the remote. The
		// toast may have run out under a still pointer: then the click is
		// the remote's after all.
		if (notices_.handle_pointer_motion(button.x * density, button.y * density)) {
			if (button.button == SDL_BUTTON_LEFT) {
				notices_.handle_pointer_press(button.x * density, button.y * density, gdp::monotonic_us());
			}
			mark_ui_dirty();
			return;
		}
		pointer_on_notice_ = false;
		set_pointer_on_toolbar(pointer_on_toolbar_);
	}
	if (pointer_on_toolbar_ && !relative_mouse_) {
		if (button.button == SDL_BUTTON_LEFT) {
			apply_toolbar_action(toolbar_.handle_pointer_press(button.x * density, button.y * density));
		}
		return;
	}
	if (btn != 0) {
		forwarded_buttons_ |= mask;
		client_.send_pointer_button(btn, true);
	}
}

void StreamSession::toggle_capture_mouse() {
	relative_mouse_ = !relative_mouse_;
	SDL_SetWindowRelativeMouseMode(window_, relative_mouse_);
	SLOG_INFO("spectre: mouse %s", relative_mouse_ ? "captured" : "released");
	if (relative_mouse_) {
		// Captured: every motion is the remote's now, so the toolbar lets
		// go of the pointer (and the fullscreen one hides), and the view
		// follows the remote cursor rather than edge pushes.
		stop_edge_push();
		toolbar_.handle_pointer_left();
		if (pointer_on_toolbar_) {
			set_pointer_on_toolbar(false);
		}
	} else {
		// Releasing by any route retires the hint for releasing.
		notices_.hide("capture-mouse");
		mark_ui_dirty();
		// Park the local pointer where wraith's ended up, so the first
		// absolute motion continues from there instead of jumping back to
		// wherever SDL restored it (the position from before the toggle).
		SDL_WarpMouseInWindow(window_, last_cursor_x_ / pixel_density(), last_cursor_y_ / pixel_density());
	}
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, !menu_.visible() && !prompt_mode_);
}

float StreamSession::pixel_density() const {
	float density = window_ ? SDL_GetWindowPixelDensity(window_) : 0.0f;
	return density > 0.0f ? density : 1.0f;
}

void StreamSession::window_pixel_size(int *w, int *h) const {
	*w = 0;
	*h = 0;
	if (window_) {
		SDL_GetWindowSizeInPixels(window_, w, h);
	}
}

void StreamSession::move_local_cursor(float x, float y) {
	// SDL reports the pointer in logical window units; the presenter, the
	// menu and last_cursor_* all work in window pixels (see open_window()'s
	// HIGH_PIXEL_DENSITY).
	float density = pixel_density();
	last_cursor_x_ = x * density;
	last_cursor_y_ = y * density;
	// The menu has the local pointer, as the toolbar does (open_menu()).
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, !menu_.visible() && !prompt_mode_);
	mark_ui_dirty(); // the overlay cursor moved; see mark_ui_dirty()'s comment
}

void StreamSession::toggle_fullscreen() {
	if (options_.kiosk) {
		return; // fullscreen for good (StreamOptions::kiosk)
	}
	SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
	SDL_SetWindowFullscreen(window_, !(flags & SDL_WINDOW_FULLSCREEN));
}

void StreamSession::apply_keyboard_grab(bool fullscreen) {
	// Windows turns Alt+F4 into a close request unless told not to; with
	// the hint off it's an ordinary key, forwarded like any other.
	SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, fullscreen ? "0" : "1");
	if (!SDL_SetWindowKeyboardGrab(window_, fullscreen)) {
		SLOG_ERROR("spectre: keyboard grab %s failed: %s", fullscreen ? "on" : "off", SDL_GetError());
	}
}

void StreamSession::toggle_stats() {
	stats_enabled_ = !stats_enabled_;
	menu_.set_stats_enabled(stats_enabled_);
	if (stats_enabled_) {
		stats_accum_ = StatsAccum{};
		stats_accum_.window_start_us = gdp::monotonic_us();
	}
	mark_ui_dirty();
}

StreamSession::VideoRect StreamSession::video_rect() const {
	int win_w = 0, win_h = 0;
	window_pixel_size(&win_w, &win_h);
	float top = toolbar_.reserved_height(ui_font_);
	if (top >= (float)win_h) {
		top = 0.0f; // a window too short for the band: video wins
	}
	return VideoRect{0.0f, top, (float)win_w, (float)win_h - top};
}

StreamSession::VideoRect StreamSession::picture_rect() const {
	VideoRect video = video_rect();
	if (!actual_size_ || display_.width == 0 || display_.height == 0) {
		return video;
	}
	float w = (float)display_.width, h = (float)display_.height;
	// Whole pixels either way, so the picture lands 1:1 on the screen's.
	auto place = [](float origin, float span, float size, float pan) {
		if (size <= span) {
			return origin + std::floor((span - size) / 2.0f);
		}
		return origin - std::round(std::clamp(pan, 0.0f, size - span));
	};
	return VideoRect{place(video.x, video.w, w, pan_x_), place(video.y, video.h, h, pan_y_), w, h};
}

void StreamSession::set_lossless(bool on, bool save) {
	if (!gdp::has_capability(session_info_.capabilities, gdp::kCapabilityRefine)) {
		return; // a host without refinement: nothing to switch
	}
	if (on != lossless_) {
		lossless_ = on;
		client_.send_refine_pause(!on);
	}
	SLOG_INFO("spectre: lossless %s", on ? "on" : "off");
	if (save && prefs_.lossless != on) {
		prefs_.lossless = on;
		save_prefs(prefs_);
	}
}

void StreamSession::set_low_latency(bool on, bool save) {
	presenter_.set_low_latency(on); // vulkan_device logs the mode it builds
	if (save && prefs_.low_latency != on) {
		prefs_.low_latency = on;
		save_prefs(prefs_);
	}
}

void StreamSession::set_actual_size(bool on, bool save) {
	bool changed = on != actual_size_;
	if (changed) {
		SLOG_INFO("spectre: view: %s", on ? "actual size" : "fit to window");
	}
	actual_size_ = on;
	if (save && prefs_.actual_size != on) {
		prefs_.actual_size = on;
		save_prefs(prefs_);
	}
	stop_edge_push();
	if (on) {
		centre_pan_on_pointer();
	} else {
		take_follow_baseline(); // -A: the window as it is now is not a resize
	}
	apply_cursor_scale();
	// The overlay cursor stays on the remote pointer: where that is in the
	// window has just moved. In absolute mode it's the other way round --
	// the local pointer stays put and now points somewhere else remotely.
	if (changed && window_ready_ && !menu_.visible()) {
		if (relative_mouse_) {
			VideoRect picture = picture_rect();
			last_cursor_x_ = picture.x + remote_pointer_x_ * picture.w;
			last_cursor_y_ = picture.y + remote_pointer_y_ * picture.h;
			presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, true);
		} else if (!pointer_on_toolbar_) {
			send_absolute_pointer();
		}
	}
	show_scroll_indicators();
	mark_ui_dirty();
}

void StreamSession::clamp_pan() {
	VideoRect video = video_rect();
	pan_x_ = std::clamp(pan_x_, 0.0f, std::max(0.0f, (float)display_.width - video.w));
	pan_y_ = std::clamp(pan_y_, 0.0f, std::max(0.0f, (float)display_.height - video.h));
}

void StreamSession::centre_pan_on_pointer() {
	VideoRect video = video_rect();
	pan_x_ = remote_pointer_x_ * (float)display_.width - video.w / 2.0f;
	pan_y_ = remote_pointer_y_ * (float)display_.height - video.h / 2.0f;
	clamp_pan();
}

void StreamSession::update_edge_push(float x, float y) {
	// Window pixels at 100% scale; the band grows with the desktop's.
	constexpr float kBandPx = 24.0f;
	// Picture pixels per second with the pointer hard against the edge:
	// a 4K picture's overhang on a 1080p screen in about a second.
	constexpr float kMaxSpeed = 2400.0f;
	push_vx_ = push_vy_ = 0.0f;
	if (!actual_size_ || relative_mouse_ || menu_.visible()) {
		return;
	}
	VideoRect video = video_rect();
	float band = kBandPx * (float)ui_font_.pixel_height() / kUiFontPx;
	if (band <= 0.0f || video.w <= 2 * band || video.h <= 2 * band) {
		return;
	}
	// Depth into the band, 0 at its inner edge to 1 at the window's.
	auto depth = [band](float distance) { return std::clamp((band - distance) / band, 0.0f, 1.0f); };
	if ((float)display_.width > video.w) {
		push_vx_ -= kMaxSpeed * depth(x - video.x);
		push_vx_ += kMaxSpeed * depth(video.x + video.w - 1.0f - x);
	}
	// In fullscreen, pushing against the top edge under the toolbar's
	// hidden panel reveals the panel; it doesn't scroll as well.
	if ((float)display_.height > video.h && !toolbar_.in_reveal_span(x)) {
		push_vy_ -= kMaxSpeed * depth(y - video.y);
	}
	if ((float)display_.height > video.h) {
		push_vy_ += kMaxSpeed * depth(video.y + video.h - 1.0f - y);
	}
}

void StreamSession::stop_edge_push() {
	push_vx_ = push_vy_ = 0.0f;
}

void StreamSession::tick_pan(uint64_t now_us) {
	// A stall (a blocking present, a dragged window) mustn't turn into
	// one big jump.
	constexpr uint64_t kMaxStepUs = 50'000;
	uint64_t step_us = std::min(now_us - last_pan_tick_us_, kMaxStepUs);
	last_pan_tick_us_ = now_us;
	if (push_vx_ == 0.0f && push_vy_ == 0.0f) {
		return;
	}
	VideoRect before = picture_rect();
	float seconds = (float)step_us / 1e6f;
	pan_x_ += push_vx_ * seconds;
	pan_y_ += push_vy_ * seconds;
	clamp_pan();
	VideoRect after = picture_rect();
	if (after.x == before.x && after.y == before.y) {
		return; // against the picture's edge, or less than a pixel yet
	}
	// The picture moved under a pointer that didn't: it points at
	// something else on the remote now.
	send_absolute_pointer();
	show_scroll_indicators();
	mark_ui_dirty();
}

void StreamSession::pan_to_show(float x, float y) {
	VideoRect video = video_rect();
	float before_x = pan_x_, before_y = pan_y_;
	// Room kept between the cursor and the window's edge, so there is
	// something to see in the direction it's heading.
	float margin_x = video.w / 8.0f, margin_y = video.h / 8.0f;
	pan_x_ = std::clamp(pan_x_, x - (video.w - margin_x), x - margin_x);
	pan_y_ = std::clamp(pan_y_, y - (video.h - margin_y), y - margin_y);
	clamp_pan();
	if (std::round(pan_x_) != std::round(before_x) || std::round(pan_y_) != std::round(before_y)) {
		show_scroll_indicators();
	}
}

void StreamSession::show_scroll_indicators() {
	indicators_shown_us_ = gdp::monotonic_us();
	mark_ui_dirty();
}

bool StreamSession::build_scroll_indicators(UiDrawList &out, uint64_t now_us) const {
	if (!actual_size_ || indicators_shown_us_ == 0 || display_.width == 0 || display_.height == 0) {
		return false;
	}
	uint64_t age_us = now_us - indicators_shown_us_;
	if (age_us >= kIndicatorHoldUs + kIndicatorFadeUs) {
		return false;
	}
	float alpha = 1.0f;
	if (age_us > kIndicatorHoldUs) {
		alpha = 1.0f - (float)(age_us - kIndicatorHoldUs) / (float)kIndicatorFadeUs;
	}
	VideoRect video = video_rect();
	VideoRect picture = picture_rect();
	bool across = picture.w > video.w, down = picture.h > video.h;
	if (!across && !down) {
		return false;
	}
	float unit = (float)ui_font_.pixel_height() / kUiFontPx;
	float thick = std::max(3.0f, std::round(4.0f * unit));
	float inset = std::round(3.0f * unit);
	UiColor track{0.0f, 0.0f, 0.0f, 0.35f * alpha};
	UiColor thumb{1.0f, 1.0f, 1.0f, 0.65f * alpha};
	// Each runs along the edge it scrolls, short of the corner the other
	// one takes. The thumb is the window's share of the picture, where
	// the window is.
	if (across) {
		float x = video.x + inset;
		float len = video.w - 2 * inset - (down ? thick + inset : 0.0f);
		float y = video.y + video.h - inset - thick;
		out.rect(x, y, len, thick, track);
		out.rect(x + len * (video.x - picture.x) / picture.w, y, len * video.w / picture.w, thick, thumb);
	}
	if (down) {
		float y = video.y + inset;
		float len = video.h - 2 * inset - (across ? thick + inset : 0.0f);
		float x = video.x + video.w - inset - thick;
		out.rect(x, y, thick, len, track);
		out.rect(x, y + len * (video.y - picture.y) / picture.h, thick, len * video.h / picture.h, thumb);
	}
	return true;
}

void StreamSession::apply_cursor_scale() {
	// The cursor bitmaps wraith sends are drawn for the remote output, so
	// they belong to the video: the pointer is scaled by however much that
	// video is being stretched into the window, exactly like everything
	// inside the frame. A window opened at the stream's own size on a 100%
	// desktop gives 1.0; halving the window halves the pointer; a HiDPI
	// desktop falls out of this on its own, since the swapchain is sized in
	// physical pixels while the stream is not. -c multiplies the result for
	// setups where the pointer still wants nudging.
	if (display_.width == 0 || display_.height == 0) {
		return;
	}
	VideoRect video = video_rect();
	int win_w = (int)video.w, win_h = (int)video.h;
	if (win_w <= 0 || win_h <= 0) {
		return;
	}
	// The video quad fills its rect (the window, less the docked toolbar)
	// without preserving aspect, but a cursor quad can only scale
	// uniformly -- take the smaller axis so the pointer never overshoots
	// the picture it belongs to.
	float scale = std::min((float)win_w / (float)display_.width, (float)win_h / (float)display_.height);
	if (actual_size_) {
		scale = 1.0f; // the picture isn't stretched at all
	}
	if (options_.cursor_scale > 0.0f) {
		scale *= options_.cursor_scale;
	}
	if (scale <= 0.0f) {
		return;
	}
	presenter_.set_cursor_scale(scale);
	// Resizes are continuous; only say something when it actually moves.
	if (std::abs(scale - logged_cursor_scale_) > 0.01f) {
		logged_cursor_scale_ = scale;
		SLOG_DEBUG("spectre: cursor scale %.2f (%dx%d video area, %ux%u stream%s)", scale, win_w, win_h,
			display_.width, display_.height, options_.cursor_scale > 0.0f ? ", -c" : "");
	}
}

void StreamSession::apply_ui_scale() {
	// The UI font -- unlike the cursor -- is spectre's own chrome, not part
	// of the remote picture, so it follows the local desktop's scaling and
	// not the video zoom. SDL_GetWindowDisplayScale() is "the expected
	// scale for displaying content in this window", which is exactly the
	// question being asked; it's re-read when the window moves to a display
	// with a different scale (SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED).
	float scale = SDL_GetWindowDisplayScale(window_);
	if (scale <= 0.0f) {
		scale = 1.0f;
	}
	int font_px = std::clamp((int)std::lround(kUiFontPx * scale), 10, 96);
	if (font_px != ui_font_.pixel_height()) {
		if (ui_font_.build(font_px) && presenter_.set_font(ui_font_)) {
			SLOG_DEBUG("spectre: ui font %dpx (%dx%d atlas)", font_px, ui_font_.atlas_width(),
				ui_font_.atlas_height());
		} else {
			SLOG_ERROR("spectre: ui font build failed at %dpx; UI text will not draw", font_px);
		}
	}
}

// --- client-side UI ---

void StreamSession::open_menu(bool confirm_end) {
	menu_.set_stats_enabled(stats_enabled_);
	menu_.set_capture_mouse(relative_mouse_);
	menu_.set_lossless(gdp::has_capability(session_info_.capabilities, gdp::kCapabilityRefine)
			? std::optional<bool>(lossless_)
			: std::nullopt);
	menu_.set_presentation(presenter_.low_latency(), presenter_.supports_low_latency());
	menu_.set_mouse_sensitivity(prefs_.mouse_sensitivity);
	// This machine's own volume; no slider when wpctl can't say.
	std::optional<SystemVolumeState> volume = get_system_volume();
	system_volume_muted_ = volume && volume->muted;
	menu_.set_volume(volume ? std::optional<double>(volume->volume) : std::nullopt);
	update_menu_mute_state();
	menu_.set_fullscreen(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN);
	menu_.set_actual_size(actual_size_);
	menu_.set_controller_mode(
		client_.hid_enabled() ? std::optional<bool>(options_.raw_gamepads) : std::nullopt);
	menu_.set_file_transfer(files_.pending() > 0);
	stop_edge_push();
	menu_.set_resolution(display_.width, display_.height);
	uint32_t screen_w = 0, screen_h = 0;
	screen_pixel_size(&screen_w, &screen_h);
	menu_.set_screen_resolution(screen_w, screen_h);
	if (confirm_end) {
		menu_.show_end_confirmation();
	} else {
		menu_.show();
	}
	release_forwarded_keys();
	if (pointer_on_toolbar_) {
		toolbar_.handle_pointer_left();
		set_pointer_on_toolbar(false);
		float x = 0.0f, y = 0.0f;
		SDL_GetMouseState(&x, &y);
		move_local_cursor(x, y);
	}
	if (relative_mouse_) {
		// The menu needs a pointer to click on; suspend capture for the
		// duration. close_menu() restores it by re-checking
		// relative_mouse_ (rather than a separate flag), so toggling
		// mouse mode *from* the menu -- which flips relative_mouse_ and
		// closes the menu in the same call -- leaves the new mode in
		// effect instead of stomping it back.
		SDL_SetWindowRelativeMouseMode(window_, false);
		SDL_WarpMouseInWindow(window_, last_cursor_x_ / pixel_density(), last_cursor_y_ / pixel_density());
	}
	// Like the toolbar (set_pointer_on_toolbar()), the menu gets the local
	// pointer: the overlay is the remote's, in whatever shape the remote
	// last set -- nothing at all, for a game.
	SDL_ShowCursor();
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, false);
	mark_ui_dirty();
}

void StreamSession::close_menu() {
	menu_.hide();
	if (prefs_dirty_) {
		prefs_dirty_ = false;
		save_prefs(prefs_);
		SLOG_INFO("spectre: mouse sensitivity %.2fx saved", prefs_.mouse_sensitivity);
	}
	if (relative_mouse_) {
		SDL_SetWindowRelativeMouseMode(window_, true);
	}
	SDL_HideCursor();
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, true);
	mark_ui_dirty();
}

void StreamSession::apply_menu_action(MenuAction action) {
	switch (action) {
	case MenuAction::kNone: mark_ui_dirty(); break;
	case MenuAction::kToggleStats:
		toggle_stats();
		// The toggle is the whole job: close so the overlay is visible
		// (or gone) immediately rather than behind the panel.
		close_menu();
		break;
	case MenuAction::kToggleCaptureMouse:
		toggle_capture_mouse();
		show_capture_mouse_hint();
		close_menu();
		break;
	case MenuAction::kToggleLossless:
		close_menu();
		set_lossless(!lossless_, true);
		notify(NoticeLevel::kInfo,
			lossless_ ? "Lossless refinement on: still text and images become pixel-exact"
					  : "Lossless refinement off",
			3000000, "lossless");
		break;
	case MenuAction::kTogglePresentation:
		close_menu();
		if (!presenter_.supports_low_latency()) {
			notify(NoticeLevel::kInfo, "This display only presents smooth: every frame waits for vsync",
				3000000, "presentation");
			break;
		}
		set_low_latency(!presenter_.low_latency(), true);
		notify(NoticeLevel::kInfo,
			presenter_.low_latency() ? "Presentation: low latency, always the newest frame"
									 : "Presentation: smooth, every frame waits its turn for vsync",
			3000000, "presentation");
		break;
	case MenuAction::kToggleFullscreen:
		toggle_fullscreen();
		close_menu();
		break;
	case MenuAction::kCancelFileTransfer:
		close_menu();
		files_.cancel();
		break;
	case MenuAction::kToggleControllerMode:
		close_menu();
		set_raw_gamepads(!options_.raw_gamepads);
		notify(NoticeLevel::kInfo,
			options_.raw_gamepads ? "Controllers: raw, each as itself" : "Controllers: Xbox emulation",
			3000000, "controllers");
		break;
	case MenuAction::kToggleActualSize:
		close_menu();
		set_actual_size(!actual_size_, true);
		notify(NoticeLevel::kInfo,
			actual_size_ ? "Actual size: push the pointer against an edge to scroll" : "Fit to window",
			3000000, "view");
		break;
	case MenuAction::kSensitivityChanged:
		prefs_.mouse_sensitivity = menu_.mouse_sensitivity();
		prefs_dirty_ = true;
		mark_ui_dirty();
		break;
	case MenuAction::kVolumeChanged:
		// Moving the slider unmutes, as a desktop's volume control does.
		if (!set_system_volume(menu_.volume(), system_volume_muted_)) {
			SLOG_ERROR("spectre: wpctl couldn't set the volume");
		}
		system_volume_muted_ = false;
		mark_ui_dirty();
		break;
	case MenuAction::kToggleSpeakerMute:
		// The glyph shows the new state: no toast, which the menu would
		// cover anyway, and the menu stays open for the other glyph.
		toggle_speaker_mute(false);
		update_menu_mute_state();
		mark_ui_dirty();
		break;
	case MenuAction::kToggleMicMute:
		toggle_mic_mute(false);
		update_menu_mute_state();
		mark_ui_dirty();
		break;
	case MenuAction::kChangeResolution:
		request_resolution(menu_.chosen_width(), menu_.chosen_height(), true);
		close_menu();
		notify(NoticeLevel::kInfo,
			"Changing resolution to " + std::to_string(requested_width_) + "x" +
				std::to_string(requested_height_) + "...",
			5000000, "resolution");
		mark_ui_dirty();
		break;
	case MenuAction::kDisconnect:
		// As the toolbar's close: spectre exits and the remote session
		// carries on, to be reconnected to later.
		running_ = false;
		break;
	case MenuAction::kEndSession: request_logout(); break;
	case MenuAction::kClose: close_menu(); break;
	}
}

void StreamSession::toggle_speaker_mute(bool announce) {
	audio_player_.set_muted(!audio_player_.muted());
	if (announce) {
		notify(NoticeLevel::kInfo, audio_player_.muted() ? "Session audio muted" : "Session audio on",
			3000000, "speaker");
	}
	SLOG_INFO("spectre: session audio %s", audio_player_.muted() ? "muted" : "unmuted");
	mark_ui_dirty();
}

void StreamSession::toggle_mic_mute(bool announce) {
	microphone_.set_muted(!microphone_.muted());
	if (announce) {
		notify(NoticeLevel::kInfo, microphone_.muted() ? "Session microphone muted" : "Session microphone on",
			3000000, "mic");
	}
	SLOG_INFO("spectre: microphone %s", microphone_.muted() ? "muted" : "unmuted");
	mark_ui_dirty();
}

void StreamSession::update_menu_mute_state() {
	menu_.set_speaker_muted(
		audio_player_.is_open() ? std::optional<bool>(audio_player_.muted()) : std::nullopt);
	menu_.set_mic_muted(microphone_.is_open() ? std::optional<bool>(microphone_.muted()) : std::nullopt);
}

void StreamSession::request_logout() {
	if (!logout_requested_) {
		logout_requested_ = true;
		SLOG_INFO("spectre: requesting logout");
		client_.send_logout_request();
	}
	// The menu's job is done: a popup says what's happening instead,
	// and stays up until the host ends the session.
	if (menu_.visible()) {
		close_menu();
	}
	notices_.clear_toasts();
	logout_notice_.show("Logging out... waiting for the host to end the session");
	mark_ui_dirty();
}

void StreamSession::apply_toolbar_action(ToolbarAction action) {
	switch (action) {
	case ToolbarAction::kNone: break;
	case ToolbarAction::kOpenMenu: open_menu(false); break;
	case ToolbarAction::kToggleCaptureMouse:
		toggle_capture_mouse();
		show_capture_mouse_hint();
		mark_ui_dirty();
		break;
	case ToolbarAction::kToggleFullscreen:
		toggle_fullscreen(); // handle_fullscreen_changed() follows
		break;
	case ToolbarAction::kToggleSpeakerMute: toggle_speaker_mute(true); break;
	case ToolbarAction::kToggleMicMute: toggle_mic_mute(true); break;
	case ToolbarAction::kEndSession:
		// No undo for a logout: the menu's confirmation step asks first.
		// Once one is on its way there's nothing left to confirm.
		if (!logout_requested_) {
			open_menu(true);
		}
		break;
	case ToolbarAction::kMinimize:
		// The pointer is about to leave with the window, and SDL may not
		// say so: let go of it now, so the panel isn't still up (and the
		// system cursor still shown) when the window comes back.
		toolbar_.handle_pointer_left();
		set_pointer_on_toolbar(false);
		SDL_MinimizeWindow(window_);
		break;
	case ToolbarAction::kNativeSize:
		snap_window(); // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED follows
		break;
	case ToolbarAction::kClose:
		// Same as closing the window: spectre exits and the remote
		// session carries on, to be reconnected to later.
		running_ = false;
		break;
	}
}

void StreamSession::snap_window() {
	// Long enough to cover every size event the compositor sends for one
	// resize (leaving fullscreen sends a restore and then this).
	constexpr uint64_t kOwnResizeUs = 1000000;
	bool fullscreen = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN;
	// A maximized window ignores a new size until it's restored.
	if (!fullscreen && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED)) {
		SDL_RestoreWindow(window_);
		SDL_SyncWindow(window_);
	}
	float density = pixel_density();
	float toolbar = (float)Toolbar::docked_height(ui_font_);
	float w = (float)display_.width;
	float h = (float)display_.height;
	SDL_DisplayID id = SDL_GetDisplayForWindow(window_);
	SDL_Rect usable{};
	bool capped = false;
	if (id && SDL_GetDisplayUsableBounds(id, &usable) && w > 0 && h > 0) {
		// The frame around the window, where the platform reports one
		// (0 where it doesn't, e.g. Wayland's client-side decorations).
		int top = 0, left = 0, bottom = 0, right = 0;
		SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);
		float max_w = (float)(usable.w - left - right) * density;
		float max_h = (float)(usable.h - top - bottom) * density - toolbar;
		float scale = std::min({1.0f, max_w / w, max_h / h});
		if (scale > 0.0f && scale < 1.0f) {
			w = std::floor(w * scale);
			h = std::floor(h * scale);
			capped = true;
		}
	}
	own_resize_until_us_ = gdp::monotonic_us() + kOwnResizeUs;
	SDL_SetWindowSize(window_, (int)std::lround(w / density), (int)std::lround((h + toolbar) / density));
	if (capped && !fullscreen) {
		// As big as the display allows: centred, it is all on screen
		// wherever it was. (Wayland places windows itself and ignores this.)
		SDL_SetWindowPosition(window_, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(id),
			(int)SDL_WINDOWPOS_CENTERED_DISPLAY(id));
	}
}

void StreamSession::show_capture_mouse_hint() {
	constexpr uint64_t kHintUs = 4000000;
	if (relative_mouse_ && !logout_requested_) {
		notify(NoticeLevel::kInfo, std::string("Press ") + kCaptureMouseKeyName + " to release the mouse",
			kHintUs, "capture-mouse");
		mark_ui_dirty();
	}
}

void StreamSession::notify(NoticeLevel level, std::string title, uint64_t duration_us, std::string tag,
	std::string body) {
	ToastSpec spec;
	spec.level = level;
	spec.title = std::move(title);
	spec.body = std::move(body);
	spec.duration_us = duration_us;
	spec.tag = std::move(tag);
	notices_.toast(std::move(spec), gdp::monotonic_us());
	mark_ui_dirty();
}

uint64_t StreamSession::ask(PromptSpec spec, std::function<void(const PromptAnswer &)> done) {
	uint64_t id = notices_.prompt(std::move(spec), gdp::monotonic_us());
	prompt_handlers_[id] = std::move(done);
	sync_prompt_mode();
	mark_ui_dirty();
	return id;
}

void StreamSession::sync_prompt_mode() {
	bool want = notices_.prompt_visible();
	if (want == prompt_mode_) {
		return;
	}
	prompt_mode_ = want;
	if (want) {
		// What open_menu() does to take the input, and the menu itself
		// goes: the prompt is what needs answering.
		if (menu_.visible()) {
			close_menu();
		}
		stop_edge_push();
		release_forwarded_keys();
		toolbar_.handle_pointer_left();
		pointer_on_toolbar_ = false;
		pointer_on_notice_ = false;
		if (relative_mouse_) {
			SDL_SetWindowRelativeMouseMode(window_, false);
			SDL_WarpMouseInWindow(window_, last_cursor_x_ / pixel_density(),
				last_cursor_y_ / pixel_density());
		}
		float x = 0.0f, y = 0.0f;
		SDL_GetMouseState(&x, &y);
		move_local_cursor(x, y);
		notices_.handle_pointer_motion(last_cursor_x_, last_cursor_y_);
		SDL_ShowCursor();
		presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, false);
	} else {
		// As close_menu(): relative mode comes back if it was on.
		if (relative_mouse_) {
			SDL_SetWindowRelativeMouseMode(window_, true);
		}
		SDL_HideCursor();
		presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, true);
		if (!relative_mouse_) {
			float x = 0.0f, y = 0.0f;
			SDL_GetMouseState(&x, &y);
			pointer_moved_to(x, y);
		}
	}
	mark_ui_dirty();
}

// spectre -Z: what the notices look like in a real session. Each step runs
// once, at its time after the first call; the prompts' answers come back
// as toasts and change nothing else.
void StreamSession::run_demo_notices(uint64_t now_us) {
	constexpr uint64_t kSec = 1000000;
	if (demo_start_us_ == 0) {
		demo_start_us_ = now_us;
	}
	uint64_t t = now_us - demo_start_us_;
	auto report = [this](const std::string &what) {
		return [this, what](const PromptAnswer &a) {
			SLOG_INFO("spectre: demo prompt \"%s\": %s", what.c_str(),
				a.timed_out ? "timed out" : (a.allowed ? "allowed" : "denied"));
			if (a.timed_out) {
				notify(NoticeLevel::kWarning, what + ": no answer, denied", 10 * kSec);
			} else if (a.allowed) {
				notify(NoticeLevel::kSuccess, what + ": allowed", 10 * kSec);
			} else {
				notify(NoticeLevel::kInfo, what + ": denied", 10 * kSec);
			}
		};
	};
	switch (demo_step_) {
	case 0:
		if (t < 2 * kSec) {
			return;
		}
		notify(NoticeLevel::kInfo, "Session audio muted", 6 * kSec, "speaker");
		break;
	case 1:
		if (t < 3 * kSec) {
			return;
		}
		notify(NoticeLevel::kSuccess, "Sent quarterly-report-2025.pdf", 8 * kSec, {}, "to Downloads");
		break;
	case 2:
		if (t < 4 * kSec) {
			return;
		}
		notify(NoticeLevel::kWarning, "The host tried to reset YubiKey 5C NFC", 12 * kSec, {},
			"Refused. Manage security keys on this computer.");
		break;
	case 3:
		if (t < 5 * kSec) {
			return;
		}
		notify(NoticeLevel::kError, "report.pdf: permission denied", 12 * kSec);
		break;
	case 4:
		if (t < 9 * kSec) {
			return;
		}
		ask({NoticeLevel::kWarning, "Erase this security key?",
				"The host wants to reset YubiKey 5C NFC. This erases every passkey on it and its PIN, and "
				"can't be undone.",
				"Erase", "Keep", 30 * kSec},
			report("Erase YubiKey 5C NFC"));
		ask({NoticeLevel::kWarning, "Generate a new key in PIV slot 9a?",
				"The host wants to replace the authentication key on YubiKey 5C NFC. Anything that signs in "
				"with the old key stops working.",
				"Replace", "Keep", 20 * kSec},
			report("Replace the PIV key"));
		break;
	default: return;
	}
	demo_step_++;
}

void StreamSession::release_forwarded_keys() {
	for (uint32_t scancode : forwarded_keys_) {
		client_.send_key(scancode, false);
	}
	forwarded_keys_.clear();
}

void StreamSession::update_stats(uint64_t now_us) {
	double elapsed_s = (double)(now_us - stats_accum_.window_start_us) / 1e6;
	if (elapsed_s <= 0.0) {
		return;
	}
	// Re-read every window: a hardware decoder can fall back to software
	// mid-stream (Decoder::backend()).
	stats_sample_.decoder = decoder_.backend_name();
	stats_sample_.fps = stats_accum_.frames / elapsed_s;
	stats_sample_.decode_ms =
		stats_accum_.frames ? (double)stats_accum_.decode_us / stats_accum_.frames / 1000.0 : 0.0;
	stats_sample_.present_ms =
		stats_accum_.frames ? (double)stats_accum_.present_us / stats_accum_.frames / 1000.0 : 0.0;
	stats_sample_.video_mbps = (double)stats_accum_.video_bytes * 8.0 / elapsed_s / 1e6;
	SessionClient::HostLatency host = client_.take_host_latency();
	if (host.samples) { // else keep the last window's: a still desktop sends no frames
		stats_sample_.host_min_ms = host.min_us / 1000.0;
		stats_sample_.host_avg_ms = (double)host.sum_us / host.samples / 1000.0;
		stats_sample_.host_max_ms = host.max_us / 1000.0;
	}
	SessionClient::LinkStats link = client_.link_stats();
	stats_sample_.rtt_ms = link.rtt_us / 1000.0;
	stats_sample_.lost_recent = link.lost_recent;
	stats_sample_.window_span = link.window_span;
	stats_sample_.fec = link.fec;
	stats_sample_.fec_recovered = link.fec_recovered;
	// Re-read every window too: an AUTO session's host can reclassify.
	stats_sample_.host_network_profile = client_.host_network_profile();
	stats_sample_.via_gateway = client_.via_gateway();
	stats_accum_ = StatsAccum{};
	stats_accum_.window_start_us = now_us;
}

void StreamSession::rebuild_ui() {
	ui_.clear();
	int win_w = 0, win_h = 0;
	window_pixel_size(&win_w, &win_h);
	VideoRect video = video_rect();
	VideoRect picture = picture_rect();
	presenter_.set_video_placement(picture.x, picture.y, picture.w, picture.h, (uint32_t)video.y);
	if (stats_enabled_) {
		build_stats_overlay(ui_, ui_font_, stats_sample_, last_latency_ms_, video.y);
	}
	tile_debug_.build(ui_, display_.width, display_.height, {picture.x, picture.y, picture.w, picture.h},
		{video.x, video.y, video.w, video.h});
	indicators_drawn_ = build_scroll_indicators(ui_, gdp::monotonic_us());
	toolbar_.set_capture_mouse(relative_mouse_);
	toolbar_.set_speaker_muted(
		audio_player_.is_open() ? std::optional<bool>(audio_player_.muted()) : std::nullopt);
	toolbar_.set_mic_muted(microphone_.is_open() ? std::optional<bool>(microphone_.muted()) : std::nullopt);
	toolbar_.build(ui_, ui_font_, win_w, win_h);
	notices_.build(ui_, ui_font_, win_w, win_h, video.y, gdp::monotonic_us());
	logout_notice_.build(ui_, ui_font_, win_w, win_h, video.y);
	menu_.build(ui_, ui_font_, win_w, win_h);
	presenter_.set_ui(ui_);
}

void StreamSession::flush_ui() {
	if (!ui_dirty_ || !window_ready_) {
		return;
	}
	// At most once per display refresh (see mark_ui_dirty()); held back,
	// it stays dirty for a later iteration, at most the loop's 4 ms away.
	SDL_DisplayID id = SDL_GetDisplayForWindow(window_);
	const SDL_DisplayMode *mode = id ? SDL_GetCurrentDisplayMode(id) : nullptr;
	float hz = mode && mode->refresh_rate > 0.0f ? mode->refresh_rate : 60.0f;
	uint64_t now_us = gdp::monotonic_us();
	if (now_us - ui_redraw_us_ < (uint64_t)(1e6f / hz)) {
		return;
	}
	ui_redraw_us_ = now_us;
	ui_dirty_ = false;
	rebuild_ui();
	presenter_.redraw();
}

} // namespace spectre
