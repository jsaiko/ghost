// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Ties SessionClient (net), Decoder (decode), VulkanPresenter (present),
// and the audio chain (audio/) together into the actual SDL3 stream window.
// run() connects, opens the window once SessionAccept arrives, and pumps
// SDL events and the network until the window closes or the session
// disconnects. Also owns the client-side UI (ui/): the toolbar along the
// top of the window, the session menu the -k hotkey toggles, and the
// statistics overlay the menu can switch on.
#pragma once

#include "audio/audio_player.hpp"
#include "audio/jitter_buffer.hpp"
#include "audio/microphone_capture.hpp"
#include "audio/opus_decode.hpp"
#include "audio/system_volume.hpp"
#include "decode/decoder.hpp"
#include "gdp/gamepad.hpp"
#include "input/raw_controller.hpp"
#include "net/session_client.hpp"
#include "present/vulkan_presenter.hpp"
#include "ui/draw_list.hpp"
#include "ui/font.hpp"
#include "ui/hotkey.hpp"
#include "ui/session_menu.hpp"
#include "ui/stats_overlay.hpp"
#include "ui/toolbar.hpp"
#include "ui/tile_debug_overlay.hpp"
#include "ui/toast.hpp"
#include "prefs.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Gamepad;
union SDL_Event;
struct SDL_KeyboardEvent;
struct SDL_MouseMotionEvent;
struct SDL_MouseButtonEvent;
struct AVFrame;

namespace spectre {

// spectre's exit status when wraith closed the session with
// ENDED_BY_LOCAL_LOGIN (gdp-spec.md §12): the user logged in at the host's
// console. spectre-qt (connect_window.cpp) matches it to say so.
constexpr int kExitEndedByLocalLogin = 3;
// spectre's exit status when wraith refused the connection with
// ALREADY_CONNECTED: the user's session is open on another client. The
// launchers (spectre-qt, wisp-greeter) offer to run again with -T.
constexpr int kExitAlreadyConnected = 4;
// ... and when the session was open here but another client took it over
// (TAKEN_OVER): an ordinary end, and the launchers say so.
constexpr int kExitTakenOver = 5;

// The GPU spectre decodes and presents on (Linux). The first render node
// is right for a single-GPU client; a client with no GPU has no node and
// falls back to lavapipe + software decode
// (VulkanDevice::pick_physical_device()). Choosing among several GPUs would need node
// enumeration -- see docs/design/spectre-client.md#limitations.
constexpr const char *kDrmRenderNode = "/dev/dri/renderD128";

struct StreamOptions {
	std::string host;
	uint16_t port = 0;
	std::string token;
	// The session host's certificate fingerprint (spectre -P, gdp::
	// sha256_hex() form): the connection is refused unless wraith presents
	// exactly this certificate. spectre-qt passes what ghostd vouched for
	// in Redirect.cert_sha256 (gdp-spec.md §2.3).
	std::string cert_sha256;
	// Which decoder to try first (spectre -X): Vulkan Video by default, or
	// VA-API or V4L2 (Linux) / D3D11VA (Windows) / VideoToolbox (macOS), or
	// software. A hardware decoder that can't open falls back to the other
	// one, then to software; NVIDIA under Linux never tries VA-API, having
	// none. See decoder.hpp.
	DecodeBackend decoder = DecodeBackend::Vulkan;
	// Open the session window fullscreen (spectre -f) rather than at the
	// stream's own size. The session menu's toggle still applies either way.
	bool start_fullscreen = false;
	// Kiosk mode (spectre -K), for a machine that is only a terminal onto
	// the session: fullscreen from the start and for good -- the
	// fullscreen toggle (hotkey, menu, toolbar) does nothing and is
	// hidden, the toolbar has no minimize, and a window the desktop takes
	// out of fullscreen goes straight back. The toolbar's close stays, for
	// when the remote desktop or the link freezes: spectre closes and the
	// session keeps running. Ending the session works as ever.
	bool kiosk = false;
	// Requested output resolution (spectre -r WIDTHxHEIGHT), sent in
	// SessionHello.displays (gdp-spec.md §7.2). 0x0 (the default) omits
	// the field entirely -- "whatever the host is already running at" --
	// rather than sending a bogus 0x0 request. A request, not a guarantee:
	// a compositor that can't resize keeps its size, and the size
	// SessionAccept reports is what the window and decoder use either way.
	uint32_t requested_width = 0;
	uint32_t requested_height = 0;
	// Follow the window (spectre -A, off by default): when the window's
	// video area settles at a new pixel size, ask the host for that size
	// (ResolutionChange, gdp-spec.md §7.2). Off, the remote resolution only
	// changes from the menu and the picture scales to the window.
	bool follow_window = false;
	// The view to start in (spectre -V): true shows the remote picture at
	// 1:1 -- centred when smaller than the window, panned by pushing the
	// pointer against an edge when bigger -- false scales it to fill the
	// window. Unset (the default) uses whichever the session menu last
	// picked (prefs.hpp). -A sends nothing while the view is actual size.
	std::optional<bool> actual_size;
	// Extra cursor overlay size multiplier (spectre -c). 0 (the default)
	// means 1.0: the pointer is drawn at the size the host sends, scaled by
	// however much the window stretches the video (see
	// StreamSession::apply_cursor_scale). Set it for setups where that
	// still leaves the pointer too small or too large for taste.
	float cursor_scale = 0.0f;
	// Video codec to offer ahead of everything else this build can decode
	// (spectre -C), as a gdp/video_codec.hpp wire token. Empty -- the
	// default -- offers gdp::all_video_codec_tokens()' own order (pyrowave
	// on a wired LAN, then av1, h265, h264). An unrecognized token is
	// reported and ignored rather than failing the session.
	std::string preferred_codec;
	// SessionHello.take_over (spectre -T): displace the viewer already
	// attached to this user's session. The launcher sets it only when the
	// user agreed, after a first attempt exited with kExitAlreadyConnected.
	bool take_over = false;
	// SessionHello.network_profile (spectre -N): "auto" -- the default,
	// letting the host classify the link itself -- or "lan", "internet",
	// "mobile" to pin it.
	std::string network_profile = "auto";
	// Lossless refinement to start with (spectre -R, gdp/refine.hpp):
	// settled text and UI become bit-exact via a lossless tile layer over
	// the video, at the cost of hashing every frame on the host. Works with
	// any codec.
	// Unset (the default) uses whichever the session menu last picked
	// (prefs.hpp), on until it picks otherwise.
	std::optional<bool> lossless;
	// Offer the "pyrowave" codec when the host is on a wired LAN
	// (net/lan_link.hpp) -- on unless spectre -W. -C pyrowave offers it
	// regardless of the link.
	bool allow_pyrowave = true;
	// Outline every lossless refinement tile in red as it lands (spectre -D,
	// ui/tile_debug_overlay.hpp). A debugging aid for the refinement layer; it
	// has nothing to draw on a session that didn't negotiate it.
	bool debug_tile_outlines = false;
	// Offer the "gamepad" capability (gdp/gamepad.hpp), so local
	// controllers are mirrored onto the host as virtual pads. Off unless
	// spectre -G (which spectre-qt's "Forward gamepads" checkbox, on by
	// default there, passes); off, no controller is ever opened.
	bool forward_gamepads = false;
	// With forward_gamepads: forward each controller raw where it can be
	// (gdp-spec.md §8.6), as a standard pad where it can't. Off with
	// spectre -g: every controller standard. Only the starting mode --
	// "hid" is offered either way, and the session menu switches.
	bool raw_gamepads = true;
	// Offer the "microphone" capability and, if the host takes it, send
	// the default recording device to it (spectre -M). Off by default: a
	// microphone is never opened unless the user asked for one.
	bool microphone = false;
	// The chord that opens the session menu (spectre -k); main.cpp fills
	// in kDefaultMenuHotkey when none is given.
	HotkeyChord menu_hotkey;
};

class StreamSession {
public:
	explicit StreamSession(StreamOptions options);
	~StreamSession();
	StreamSession(const StreamSession &) = delete;
	StreamSession &operator=(const StreamSession &) = delete;

	// Blocks for the duration of the stream. Returns a process exit code.
	int run();

private:
	// What SessionHello offers: what this client decodes, less pyrowave
	// off a wired LAN (net/lan_link.hpp).
	std::vector<std::string> offerable_codecs() const;
	// The DiagnosticsRequest answer (gdp-spec.md §7.11): what this client
	// ended up with, then its recent log.
	std::string diagnostics_text() const;
	// run()'s steps, in order.
	bool connect_and_wait_for_accept();
	bool open_window();
	void open_audio();
	void open_microphone();
	void main_loop();
	int exit_status_for_close(int otherwise) const;
	// Flip session audio's or the microphone's mute (toolbar or session
	// menu), with a toast when `announce`.
	void toggle_speaker_mute(bool announce);
	void toggle_mic_mute(bool announce);
	// The session menu's glyph row, from the player and capture.
	void update_menu_mute_state();

	// SessionClient callbacks (fired from client_.dispatch()).
	void on_accepted(const SessionInfo &session, const DisplayInfo &display, const AudioInfo &audio);
	void on_disconnected(const std::string &reason);
	bool on_video_frame(const uint8_t *data, size_t len);
	void on_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
		const uint8_t *argb8888);
	void on_cursor_position(double x, double y);
	void on_clipboard_text(const std::string &utf8);
	// DisplaysChanged: the remote output's size now (gdp-spec.md §7.2). The
	// window keeps its size; the picture scales.
	void on_displays_changed(const DisplayInfo &display);
	// The local screen under the window, in pixels (0x0 if unknown): the
	// menu's "Match this display" choice.
	void screen_pixel_size(uint32_t *width, uint32_t *height) const;
	// -A. schedule_follow_window(), on a window size event: arms the settle
	// timer, or -- in the moment after snap_window() or the window first
	// appearing -- takes the new video area as the baseline instead (those
	// sizes are spectre's, not the user's). follow_window(), once the timer
	// fires: asks the host for the video area's size.
	void schedule_follow_window();
	void follow_window();
	// Sends ResolutionChange and remembers it until DisplaysChanged; from
	// the menu (which reports the answer in a toast) or from -A (quiet).
	void request_resolution(uint32_t width, uint32_t height, bool from_menu);
	// Hands `utf8` to SDL's clipboard. Split out of on_clipboard_text() so
	// open_window() can run the same steps for text that arrived early.
	void apply_clipboard_text(const std::string &utf8);
	// Decoder callback (fired synchronously from decoder_.decode()).
	void on_decoded_frame(AVFrame *frame);
	// Presents the newest picture on_decoded_frame() held back, if any.
	void present_pending_frame();
	void apply_lossless_layer(const gdp::RefineLayer &layer, uint64_t now_us);

	// SDL event handling.
	void handle_event(const SDL_Event &event);
	void handle_key(const SDL_KeyboardEvent &key);
	void handle_mouse_motion(const SDL_MouseMotionEvent &motion);
	void handle_mouse_button(const SDL_MouseButtonEvent &button);
	// The absolute-mode pointer moved to SDL position `x`,`y` (logical
	// units): the toolbar takes it if it's on the toolbar, otherwise it
	// moves the overlay cursor and is forwarded, mapped into the video.
	void pointer_moved_to(float x, float y);
	// Hands the pointer to the toolbar (the local system cursor, the
	// overlay cursor hidden) or back to the remote (the reverse).
	void set_pointer_on_toolbar(bool on);
	// SDL_EVENT_WINDOW_ENTER/LEAVE_FULLSCREEN: docks or floats the toolbar,
	// which moves the video (see video_rect()).
	void handle_fullscreen_changed(bool fullscreen);
	// SDL_EVENT_CLIPBOARD_UPDATE: read the local clipboard and forward it
	// to wraith. Core Wayland only delivers the selection to the client
	// with keyboard focus, so on a Wayland desktop this fires only while
	// the spectre window is focused -- a copy made in another local app is
	// picked up on the next focus gain, which is exactly the moment before
	// the user pastes into the remote session anyway.
	void handle_clipboard_update();
	// Gamepads (gdp-spec.md §8.5): SDL's gamepad API already
	// normalizes every controller on every platform to one layout, so
	// this is the same code on Linux, Windows and macOS. A controller
	// that appears takes the lowest free slot in gamepads_ and is
	// announced with GamepadConnect; one that goes away frees its slot
	// with GamepadDisconnect. Nothing is opened at all on a session that
	// didn't negotiate the capability.
	void handle_gamepad_added(uint32_t instance_id);
	void handle_gamepad_removed(uint32_t instance_id);
	// Forwards the controller in slot `i` raw, if it can be (true), or
	// leaves the slot empty for the standard path.
	bool open_raw_controller(uint32_t i, uint32_t instance_id);
	// Forwards the controller as a standard pad in slot `i`.
	void open_standard_gamepad(uint32_t i, uint32_t instance_id);
	// The host refused or failed slot `i`'s raw controller
	// (HidRejected): the same controller goes standard.
	void on_hid_rejected(uint32_t i, const std::string &reason);
	// The session menu's controller row: switches StreamOptions::
	// raw_gamepads and re-plugs every forwarded controller that way.
	void set_raw_gamepads(bool raw);
	// Any axis/button event: marks the slot so flush_gamepads() sends one
	// snapshot for it, however many SDL events the poll batch held.
	void mark_gamepad_dirty(uint32_t instance_id);
	void note_gamepad_activity();
	// Once per main-loop iteration, after the SDL queue is drained: a full
	// GamepadState for every dirty slot. Coalescing per batch rather than
	// per event keeps a stick's stream of motion events to at most one
	// message per loop turn, which is already faster than the video.
	void flush_gamepads();
	void close_gamepads();
	// Captures or releases the mouse (relative_mouse_). Scroll Lock, the
	// menu and the toolbar all toggle it.
	void toggle_capture_mouse();
	// After a toggle from the toolbar button or the menu, if it captured
	// the mouse: shows how to release it, since the captured pointer can't
	// reach the button again. Scroll Lock itself needs no hint, and
	// toggle_capture_mouse() takes it down on release.
	void show_capture_mouse_hint();
	// The lossless layer on or off (RefinePause, gdp-spec.md §7.8): off,
	// the host saves the hashing, the lossless bytes and, where it can,
	// the per-frame readback. `save` records it as the menu's choice
	// (prefs). No-op on a session that didn't negotiate "refine".
	void set_lossless(bool on, bool save);
	void toggle_fullscreen();
	void toggle_stats();
	// Fullscreen is the remote desktop: the keyboard is grabbed, so the
	// local desktop's own shortcuts (Alt+F4, Alt+Tab, Super) reach spectre
	// and are forwarded instead of acting locally. Windowed, they're the
	// local desktop's again. SDL does it per platform -- Wayland's
	// keyboard-shortcuts-inhibit protocol (GNOME asks the user once),
	// an X11 keyboard grab, a Windows keyboard hook -- and a few combos
	// no application can have: Ctrl+Alt+Del on Windows, Ctrl+Alt+Fn VT
	// switching on Linux.
	void apply_keyboard_grab(bool fullscreen);
	// Where the video is drawn, in window pixels: the whole window, less
	// the docked toolbar's band at the top when windowed. Pointer
	// positions, the cursor overlay and the tile outlines all map through
	// this, not the window.
	struct VideoRect {
		float x = 0, y = 0, w = 0, h = 0;
	};
	VideoRect video_rect() const;
	// Where the remote picture is drawn, in window pixels: video_rect()
	// itself when fitting the window; at actual size, the display's own
	// size, centred in video_rect() along an axis where it is smaller and
	// offset by the pan where it is bigger (so it hangs off the window's
	// edges). Everything that maps between window and remote -- the
	// pointer, the cursor overlay, the tile outlines -- goes through this.
	VideoRect picture_rect() const;
	// Window pixels per logical unit (SDL_GetWindowPixelDensity), 1 when
	// unknown. SDL's pointer coordinates are logical; everything spectre
	// draws is in pixels.
	float pixel_density() const;
	// The window's size in pixels (the swapchain's), 0x0 before it exists.
	void window_pixel_size(int *w, int *h) const;
	// Moves the overlay cursor to SDL pointer position `x`,`y` (logical
	// units), recording it in last_cursor_* in window pixels.
	void move_local_cursor(float x, float y);
	void apply_cursor_scale();
	void apply_ui_scale();
	// Snaps the window to the remote size: one stream pixel per screen
	// pixel below the docked toolbar, un-maximizing it first if need be --
	// or, when that won't fit the display's usable area, the largest window
	// that does at the same aspect. While fullscreen it sets the size the
	// window comes back to. -A ignores the resizes this causes.
	void snap_window();

	// --- actual-size view (StreamOptions::actual_size) ---
	// Switches the view; `save` records it as the menu's choice (prefs).
	void set_actual_size(bool on, bool save);
	// Keeps the pan inside the picture: 0 along an axis that fits.
	void clamp_pan();
	// Pans so the remote pointer's last known position is in the middle of
	// the window (entering the view, or a new remote size).
	void centre_pan_on_pointer();
	// Absolute mode: the pointer at window pixel `x`,`y` sets the edge-push
	// velocity -- inside a band along an edge the picture overhangs, the
	// deeper the faster. Elsewhere, or off the video, it stops.
	void update_edge_push(float x, float y);
	void stop_edge_push();
	// Main loop: moves the pan by the edge-push velocity and, since the
	// picture moved under a still pointer, sends where it now points.
	void tick_pan(uint64_t now_us);
	// Mouse captured: pans just enough to keep the remote cursor (at picture
	// pixel `x`,`y`) clear of the window's edges.
	void pan_to_show(float x, float y);
	// The scroll indicators: shown now, fading out a second after the
	// last call. A no-op when nothing overhangs.
	void show_scroll_indicators();
	// Appends them to `out`; false when nothing was drawn (hidden, faded
	// out, or nothing overhangs).
	bool build_scroll_indicators(UiDrawList &out, uint64_t now_us) const;
	// Sends the absolute pointer position for the overlay cursor's current
	// spot (last_cursor_*), mapped into the picture.
	void send_absolute_pointer();

	// Client-side UI (ui/). The menu captures all keyboard and pointer
	// input while it's up; nothing reaches the remote session.
	// `confirm_end` opens straight onto the end-session confirmation
	// (the toolbar's End Session button).
	void open_menu(bool confirm_end = false);
	void close_menu();
	void apply_menu_action(MenuAction action);
	// Sends LogoutRequest (once) and says so until the host ends the
	// session: the menu's End Session, or SIGUSR1.
	void request_logout();
	void apply_toolbar_action(ToolbarAction action);
	// Sends a release for every key currently forwarded as pressed, so
	// the remote never sees the menu chord's modifiers stuck down.
	void release_forwarded_keys();
	// Composes the stats overlay, toolbar, toasts and menu into the
	// presenter's UI list, and tells the presenter where the video goes.
	void rebuild_ui();
	// Marks the UI as needing a re-present of the last frame -- for
	// changes that happen between video frames (menu navigation, the
	// pointer moving over the menu on an idle desktop). Coalesced: the
	// main loop redraws at most once per iteration, after draining every
	// pending SDL event, because a present blocks on vsync and doing one
	// per mouse-motion event (hundreds a second) backs the event queue up
	// into visible pointer lag.
	void mark_ui_dirty() { ui_dirty_ = true; }
	// rebuild_ui() and re-present the last frame if anything is dirty.
	void flush_ui();
	// Folds the last window's accumulators into stats_sample_ (twice a
	// second, so the numbers are readable rather than flickering).
	void update_stats(uint64_t now_us);

	StreamOptions options_;

	SessionClient client_;
	Decoder decoder_;
	VulkanPresenter presenter_;
	// wraith omits SessionAccept.audio entirely when its own capture failed
	// to open (wraith's gdp_session.cpp), in which case the session just
	// stays video-only and jitter_buffer_ stays null -- no audio device is
	// opened at all.
	OpusDecodeWrapper audio_decoder_;
	std::unique_ptr<JitterBuffer> jitter_buffer_;
	AudioPlayer audio_player_;
	MicrophoneCapture microphone_;

	bool sdl_initialized_ = false;
	SDL_Window *window_ = nullptr;
	// Set once open_window() has the presenter and decoder up. Until then
	// the network callbacks have nowhere to deliver: video frames are
	// dropped (a keyframe is requested right after anyway) and the cursor
	// shape -- which wraith sends in the same control chunk as
	// SessionAccept -- is held in pending_cursor_ and applied once the
	// presenter exists.
	bool window_ready_ = false;
	struct PendingCursor {
		uint32_t width = 0;
		uint32_t height = 0;
		int32_t hotspot_x = 0;
		int32_t hotspot_y = 0;
		std::vector<uint8_t> argb8888;
	};
	std::optional<PendingCursor> pending_cursor_;
	// Same idea for the clipboard wraith pushes on connect: SDL's clipboard
	// belongs to the video subsystem, so text that arrives before
	// open_window()'s SDL_Init waits here rather than being dropped.
	std::optional<std::string> pending_clipboard_;

	// Session state from the SessionClient callbacks.
	bool accepted_ = false;
	bool disconnected_ = false;
	std::string disconnect_reason_;
	SessionInfo session_info_;
	std::vector<std::string> offered_codecs_; // what SessionHello offered
	DisplayInfo display_;
	AudioInfo audio_info_;

	// Set when a coded frame is handed to the decoder; the decode-time
	// half of the per-frame timing reported in StatsReport.
	uint64_t frame_decode_start_us_ = 0;
	// Whether the refinement layer of the frame being decoded has been
	// applied yet -- by on_decoded_frame() if the decoder produced a
	// picture, else by on_video_frame() itself afterwards (a layer-only
	// frame, or a decoder holding the picture back). Either way the layer
	// reaches the plane; it is never tied to a picture that may not come.
	bool lossless_applied_ = false;

	// The newest decoded picture not yet on screen. Frames that arrive
	// together -- the burst a stalled link releases all at once -- all
	// decode, so the next one has its references, but only the last is
	// presented, after dispatch() has drained them: showing each in turn
	// would replay the stall at fast-forward instead of catching up.
	AVFrame *pending_frame_ = nullptr;
	uint32_t pending_decode_us_ = 0;
	uint64_t last_present_us_ = 0;
	// Pictures decoded but replaced before being shown, since the last
	// present; logged when a burst was big enough to be a stall.
	uint32_t frames_skipped_ = 0;

	// Absolute mode (1:1 with the window) is the default: it's what normal
	// desktop interaction needs -- click a window, see where the pointer
	// is. Relative/captured mode (toggle_capture_mouse()) is for games and
	// 3D viewports that want mouselook. The
	// cursor overlay stays visible in both; what differs is where its
	// position comes from: the local pointer in absolute mode, wraith's
	// CursorPosition reports in relative mode (only the server knows where
	// relative deltas landed). Whether a cursor is drawn at all is the
	// focused application's call either way, via CursorShape (a game hiding
	// its pointer for mouselook sends an empty one).
	bool relative_mouse_ = false;
	// prefs.hpp's settings, loaded at start; the menu's slider changes
	// mouse_sensitivity (which scales relative motion) and close_menu()
	// saves them if it did.
	Prefs prefs_;
	bool prefs_dirty_ = false;
	// The local output was muted when the menu opened (its volume row);
	// the first slider move unmutes it.
	bool system_volume_muted_ = false;
	float last_cursor_x_ = 0.0f;
	float last_cursor_y_ = 0.0f;

	// --- client-side UI ---
	Toolbar toolbar_;
	// The absolute-mode pointer is on the toolbar: it's shown as the local
	// system cursor and nothing is forwarded (see set_pointer_on_toolbar).
	bool pointer_on_toolbar_ = false;
	// SDL_BUTTON_MASK()s of the buttons forwarded to the remote as pressed
	// and not yet released. A press on the toolbar or menu is never
	// forwarded, so its release isn't either; and while one of these is
	// held (a drag) the pointer stays the remote's even over the toolbar.
	uint32_t forwarded_buttons_ = 0;
	SessionMenu menu_;
	bool stats_enabled_ = false;
	// Refinement tile outlines (StreamOptions::debug_tile_outlines). Inert
	// unless enabled, and only ever fed on a refined session.
	TileDebugOverlay tile_debug_;
	// Short notices: how to release the mouse when it was captured with a
	// click (show_capture_mouse_hint()), the view, resolution and lossless
	// changing, the toolbar's mute buttons.
	Toast toast_;
	// "Logging out..." from the LogoutRequest until the session ends. Its
	// own Toast so that releasing the mouse, which takes toast_ down,
	// can't take this one with it.
	Toast logout_notice_;
	// Last cursor scale logged, so a drag-resize doesn't log a line per
	// pixel (see apply_cursor_scale).
	float logged_cursor_scale_ = 0.0f;
	// The UI font, rasterized at kUiFontPx times the OS display scale
	// (14px on a 100% desktop, 28px at 200%) -- rebuilt and re-uploaded
	// when the window lands on a display with a different scale.
	UiFont ui_font_;
	// Scancodes forwarded to the remote as pressed and not yet released.
	// Releases for anything not in here are dropped (the key went down
	// while the menu had the keyboard, or was released remotely when the
	// menu opened).
	std::set<uint32_t> forwarded_keys_;
	bool logout_requested_ = false;
	// The size the menu last asked the host for, until DisplaysChanged
	// answers; 0x0 when nothing is pending.
	uint32_t requested_width_ = 0, requested_height_ = 0;
	bool request_from_menu_ = false;
	// -A: when the settle timer fires (0 = not armed), and the last size
	// the host refused, which is not asked for again.
	uint64_t follow_due_us_ = 0;
	uint32_t refused_width_ = 0, refused_height_ = 0;
	// The screen is held awake after gamepad input (note_gamepad_activity())
	// until kGamepadIdleHoldUs have passed without any.
	static constexpr uint64_t kGamepadIdleHoldUs = 60 * 1000000ull;
	static constexpr int16_t kGamepadActiveAxis = 8000;
	bool idle_inhibited_ = false;
	uint64_t gamepad_active_us_ = 0;
	// The last input report from any raw controller, written by their
	// reader threads: a raw controller never raises SDL gamepad events,
	// and it reports continuously while it is on, so main_loop() counts
	// it as activity for as long as it does.
	std::atomic<uint64_t> raw_report_us_{0};
	// Window size changes before this come from snap_window() or the
	// window first appearing, not the user.
	uint64_t own_resize_until_us_ = 0;
	// -A asks the host for the video area only when it differs from this:
	// the area it last accepted (the window as it first settled, after a
	// snap, or what it last asked for). A size event that changes nothing
	// -- a compositor restating fullscreen, say -- asks for nothing.
	uint32_t follow_baseline_w_ = 0, follow_baseline_h_ = 0;
	void take_follow_baseline();
	// Actual-size view: on, and how far into the picture (picture pixels)
	// the window's top-left is, along an axis the picture overhangs.
	bool actual_size_ = false;
	// The lossless layer is being built (set_lossless()); the host starts
	// every session with it on.
	bool lossless_ = true;
	float pan_x_ = 0.0f, pan_y_ = 0.0f;
	// Edge-push velocity, picture pixels per second, and the last
	// tick_pan() time it was applied from.
	float push_vx_ = 0.0f, push_vy_ = 0.0f;
	uint64_t last_pan_tick_us_ = 0;
	// The remote pointer's last position, normalized 0..1 (what was last
	// sent in absolute mode, or reported in relative mode).
	float remote_pointer_x_ = 0.5f, remote_pointer_y_ = 0.5f;
	// When the scroll indicators were last shown; 0 = never.
	uint64_t indicators_shown_us_ = 0;
	// The last rebuild_ui() drew them, so the main loop keeps redrawing
	// while they fade.
	bool indicators_drawn_ = false;
	UiDrawList ui_;
	bool ui_dirty_ = false;

	// Statistics: per-window accumulators and the last computed sample.
	struct StatsAccum {
		uint64_t window_start_us = 0;
		uint32_t frames = 0;
		uint64_t decode_us = 0;
		uint64_t present_us = 0;
		uint64_t video_bytes = 0;
	};
	StatsAccum stats_accum_;
	StatsSample stats_sample_;
	double last_latency_ms_ = 0.0;

	// Forwarded controllers, one per wire slot (GamepadConnect.pad_index).
	// `id` is SDL's instance id, what its events are keyed by. A slot
	// holds a standard pad (`pad`, opened through SDL's gamepad API) or a
	// raw one (`raw`, its HID interface), never both.
	struct GamepadSlot {
		SDL_Gamepad *pad = nullptr;
		std::unique_ptr<RawController> raw;
		uint32_t id = 0;
		bool dirty = false;
		bool used() const { return pad || raw; }
	};
	GamepadSlot gamepads_[gdp::kMaxGamepads];

	bool running_ = true;
	// The presenter reported an unrecoverable Vulkan error (see
	// VulkanPresenter::failed()); run() exits non-zero.
	bool presenter_failed_ = false;
};

} // namespace spectre
