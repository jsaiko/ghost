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
#include "gdp/negotiation.hpp"
#include "gdp/refine.hpp"
#include "log.hpp"
#include "net/lan_link.hpp"
#include "session.pb.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
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
#ifdef _WIN32
void wait_for_notify(int /*fd*/, int timeout_ms) {
	Sleep((DWORD)timeout_ms);
}
#else
void wait_for_notify(int fd, int timeout_ms) {
	struct pollfd pfd{fd, POLLIN, 0};
	poll(&pfd, 1, timeout_ms);
}
#endif

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
	decoder_.on_frame = [this](AVFrame *frame) { on_decoded_frame(frame); };
}

StreamSession::~StreamSession() {
	// Explicit, in this order, before SDL_Quit(): the members would
	// otherwise be destroyed *after* this destructor body, i.e. after
	// SDL_Quit() already tore the video/audio subsystems down. Every
	// close() here is a no-op on something never opened.
	av_frame_free(&pending_frame_);
	decoder_.close();
	presenter_.shutdown();
	microphone_.close();
	audio_player_.close();
	audio_decoder_.close();
	close_gamepads();
	if (window_) {
		SDL_DestroyWindow(window_);
	}
	if (sdl_initialized_) {
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

std::vector<std::string> StreamSession::offerable_codecs() const {
	std::vector<std::string> codecs = probe_decodable_codecs(options_.decoder, kDrmRenderNode);
	// pyrowave only on a wired LAN of 1 Gbit/s or more (gdp-spec.md §6.6),
	// unless asked for by name. It leads gdp's token order, so offering it
	// at all offers it first.
	auto pyrowave = std::find(codecs.begin(), codecs.end(), gdp::kVideoCodecPyrowave);
	if (pyrowave == codecs.end()) {
		SLOG_INFO("spectre: not offering pyrowave: this client can't decode it (see vulkan_device above)");
		return codecs;
	}
	if (options_.preferred_codec == gdp::kVideoCodecPyrowave) {
		SLOG_INFO("spectre: offering pyrowave (-C pyrowave)");
		return codecs;
	}
	if (!options_.allow_pyrowave) {
		codecs.erase(pyrowave);
		return codecs;
	}
	LanLink link = probe_lan_link(options_.host, options_.port);
	if (!link.qualifies) {
		SLOG_INFO("spectre: not offering pyrowave: %s", link.description.c_str());
		codecs.erase(pyrowave);
		return codecs;
	}
	SLOG_INFO("spectre: offering pyrowave first: %s", link.description.c_str());
	return codecs;
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
	text += "offered codecs: " + join(offered_codecs_) + "\n";
	text += "session codec: " + session_info_.codec + " (host encoder: " + session_info_.encoder + ")\n";
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
	client_.set_requested_display(options_.requested_width, options_.requested_height);
	offered_codecs_ = offerable_codecs();
	client_.set_decodable_codecs(offered_codecs_);
	if (!options_.preferred_codec.empty()) {
		client_.set_preferred_codec(options_.preferred_codec);
	}
	client_.set_network_profile(options_.network_profile);
	client_.set_take_over(options_.take_over);
	client_.set_gamepad_forwarding(options_.forward_gamepads);
	// Offered whichever mode the session starts in: the menu can switch
	// to raw later, but a capability can't be added after SessionAccept.
	client_.set_raw_controllers(true);
	client_.set_microphone(options_.microphone);
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
	// Matches the "spectre-qt" desktop entry (packaging/desktop/spectre-qt.desktop.in)
	// so Wayland compositors/taskbars -- which resolve a window's icon by
	// app-id rather than accepting one at runtime -- show the same icon for
	// this streaming window as for the launcher. SDL_SetWindowIcon() below
	// covers X11/Windows, where SDL can set one directly.
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
	// screensaver and display sleep go ahead -- except after gamepad input
	// (note_gamepad_activity()), which the local desktop never sees.
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

	// SDL_INIT_GAMEPAD whether or not the session negotiated "gamepad"
	// (no spectre -G, or a host that can't create devices): the subsystem is
	// cheap, and handle_gamepad_added() is where the capability check
	// lives, so such a session just sees no controller ever opened.
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
		SLOG_ERROR("spectre: SDL_Init failed: %s", SDL_GetError());
		return false;
	}
	sdl_initialized_ = true;
	if (pending_clipboard_) {
		apply_clipboard_text(*pending_clipboard_);
		pending_clipboard_.reset();
	}

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
	SDL_IOStream *icon_io = SDL_IOFromConstMem(kAppIconPng, kAppIconPng_len);
	if (icon_io) {
		SDL_Surface *icon = SDL_LoadPNG_IO(icon_io, true);
		if (icon) {
			SDL_SetWindowIcon(window_, icon);
			SDL_DestroySurface(icon);
		}
	}

	if (!presenter_.init(window_, kDrmRenderNode)) {
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
		stats_sample_.audio = true;
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

		wait_for_notify(client_.notify_fd(), 4);
		client_.dispatch();
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
		if (toast_.expire(now_us)) {
			mark_ui_dirty();
		}
		if (follow_due_us_ != 0 && now_us >= follow_due_us_) {
			follow_due_us_ = 0;
			follow_window();
		}
		tick_pan(now_us);
		uint64_t raw_report_us = raw_report_us_.load(std::memory_order_relaxed);
		if (raw_report_us > gamepad_active_us_ && now_us - raw_report_us < kGamepadIdleHoldUs) {
			note_gamepad_activity();
		}
		if (idle_inhibited_ && now_us - gamepad_active_us_ >= kGamepadIdleHoldUs) {
			idle_inhibited_ = false;
			SDL_EnableScreenSaver();
		}
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
}

void StreamSession::on_displays_changed(const DisplayInfo &display) {
	constexpr uint64_t kToastUs = 3000000;
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
			toast_.show(got_it ? "Resolution " + size : "The host kept " + size, gdp::monotonic_us(),
				kToastUs);
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
	client_.send_resolution_change(width, height);
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
	SDL_DisplayID id = window_ ? SDL_GetDisplayForWindow(window_) : 0;
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

void StreamSession::on_disconnected(const std::string &reason) {
	disconnected_ = true;
	disconnect_reason_ = reason;
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
	if (!relative_mouse_ || !window_ready_ || menu_.visible()) {
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
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, !menu_.visible());
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
	if (!sdl_initialized_) {
		pending_clipboard_ = utf8;
		return;
	}
	apply_clipboard_text(utf8);
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
	if (!idle_inhibited_) {
		idle_inhibited_ = true;
		SDL_DisableScreenSaver();
	}
}

void StreamSession::handle_event(const SDL_Event &event) {
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
	case SDL_EVENT_CLIPBOARD_UPDATE: handle_clipboard_update(); break;
	case SDL_EVENT_MOUSE_WHEEL:
		if (menu_.visible() || pointer_on_toolbar_) {
			break; // the menu or toolbar has the pointer
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

void StreamSession::handle_clipboard_update() {
	if (!client_.clipboard_enabled() || !SDL_HasClipboardText()) {
		return;
	}
	// The text variants (rather than the generic SDL_SetClipboardData) do
	// the whole text mime set internally and hand back a plain buffer, so
	// there's no owned buffer or cleanup callback to manage -- and they
	// work on X11, Wayland, Windows and macOS alike.
	char *text = SDL_GetClipboardText();
	if (!text) {
		return;
	}
	// An unfocused window on Wayland reads "", which is "nothing readable
	// here", not a request to clear the remote clipboard -- send_clipboard
	// drops it either way.
	client_.send_clipboard(text);
	SDL_free(text);
}

void StreamSession::handle_key(const SDL_KeyboardEvent &key) {
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
	if (on_toolbar != pointer_on_toolbar_) {
		set_pointer_on_toolbar(on_toolbar);
	}
	if (on_toolbar) {
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
	if (menu_.visible()) {
		if (button.button == SDL_BUTTON_LEFT) {
			apply_menu_action(menu_.handle_pointer_press(button.x * density, button.y * density));
		}
		return;
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
		toast_.hide();
		mark_ui_dirty();
		// Park the local pointer where wraith's ended up, so the first
		// absolute motion continues from there instead of jumping back to
		// wherever SDL restored it (the position from before the toggle).
		SDL_WarpMouseInWindow(window_, last_cursor_x_ / pixel_density(), last_cursor_y_ / pixel_density());
	}
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, !menu_.visible());
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
	presenter_.set_cursor_position(last_cursor_x_, last_cursor_y_, !menu_.visible());
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
		toast_.show(lossless_ ? "Lossless refinement on: still text and images become pixel-exact"
							  : "Lossless refinement off",
			gdp::monotonic_us(), 3000000);
		break;
	case MenuAction::kToggleFullscreen:
		toggle_fullscreen();
		close_menu();
		break;
	case MenuAction::kToggleControllerMode:
		close_menu();
		set_raw_gamepads(!options_.raw_gamepads);
		toast_.show(options_.raw_gamepads ? "Controllers: raw, each as itself"
										  : "Controllers: Xbox emulation",
			gdp::monotonic_us(), 3000000);
		break;
	case MenuAction::kToggleActualSize:
		close_menu();
		set_actual_size(!actual_size_, true);
		toast_.show(actual_size_ ? "Actual size: push the pointer against an edge to scroll"
								 : "Fit to window",
			gdp::monotonic_us(), 3000000);
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
		toast_.show("Changing resolution to " + std::to_string(requested_width_) + "x" +
				std::to_string(requested_height_) + "...",
			gdp::monotonic_us(), 5000000);
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
		toast_.show(audio_player_.muted() ? "Session audio muted" : "Session audio on", gdp::monotonic_us(),
			3000000);
	}
	SLOG_INFO("spectre: session audio %s", audio_player_.muted() ? "muted" : "unmuted");
	mark_ui_dirty();
}

void StreamSession::toggle_mic_mute(bool announce) {
	microphone_.set_muted(!microphone_.muted());
	if (announce) {
		toast_.show(microphone_.muted() ? "Session microphone muted" : "Session microphone on",
			gdp::monotonic_us(), 3000000);
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
	toast_.hide();
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
		toast_.show(std::string("Press ") + kCaptureMouseKeyName + " to release the mouse",
			gdp::monotonic_us(), kHintUs);
		mark_ui_dirty();
	}
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
	toast_.build(ui_, ui_font_, win_w, win_h, video.y);
	logout_notice_.build(ui_, ui_font_, win_w, win_h, video.y);
	menu_.build(ui_, ui_font_, win_w, win_h);
	presenter_.set_ui(ui_);
}

void StreamSession::flush_ui() {
	if (!ui_dirty_ || !window_ready_) {
		return;
	}
	ui_dirty_ = false;
	rebuild_ui();
	presenter_.redraw();
}

} // namespace spectre
