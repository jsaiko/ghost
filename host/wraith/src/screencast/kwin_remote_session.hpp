// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// KwinRemoteSession: the RemoteSession (remote_session.hpp) for
// `Backend=screencast-kwin` (docs/design/capture-backends.md):
// kwin_wayland runs top-level on its `--virtual` backend (packaging/ghost/
// sessions/plasma-screencast) and wraith is its client twice over. Size:
// open() sets kwin's virtual output to wraith's own through
// kde_output_management_v2 (kwin_output_size.hpp), so kwin starts with no
// size of its own and a new one needs no kwin restart. Video:
// a Wayland connection binding kwin's privileged
// `zkde_screencast_unstable_v1` global (advertised only to a client whose
// desktop file lists it -- packaging/desktop/wraith.desktop.in) and asking it to
// `stream_output` the first wl_output with pointer metadata, which
// answers with a PipeWire node id for a PipeWireCapture. Input: kwin's
// `org.kde.KWin.EIS.RemoteDesktop.connectToEIS` on the session bus, an
// EIS fd for an EiInput. Unlike mutter, the two halves
// are independent: nothing links the stream to the EIS context.
#pragma once

#include "screencast/remote_session.hpp"

#include <cstdint>
#include <memory>

namespace wraith {

class KwinRemoteSession : public RemoteSession {
public:
	KwinRemoteSession();
	~KwinRemoteSession() override;
	KwinRemoteSession(const KwinRemoteSession &) = delete;
	KwinRemoteSession &operator=(const KwinRemoteSession &) = delete;

	// Connects to kwin's socket (the user manager's WAYLAND_DISPLAY, else
	// wayland-0, which kwin_wayland_wrapper always creates), requires the
	// screencast global, sets the output to config.width x height, starts
	// the output stream and waits briefly for its `created(node)`. False,
	// cleanly, while kwin isn't up yet; false with a pointed log line if
	// kwin is up but hides a global (the desktop-file trust rule) or
	// refuses the size.
	bool open(const Config &config) override;
	void close() override;

	// --- RemoteSession ---
	std::unique_ptr<FrameSource> make_frame_source() override;
	std::unique_ptr<ScreencastInput> make_input_sink() override;
	// ext-data-control-v1, the same class the ext backend uses: kwin 6.6
	// exports ext_data_control_manager_v1 (and only that variant, not the
	// older zwlr_ one). Null on a kwin that doesn't.
	std::unique_ptr<ClipboardSink> make_clipboard_sink() override;
	const char *name() const override { return "kwin"; }
	bool sizes_output_on_open() const override { return true; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
