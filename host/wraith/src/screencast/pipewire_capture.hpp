// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// PipeWireCapture: FrameSource (frame_source.hpp) that pulls frames off a
// PipeWire node by id. The node id comes from the RemoteSession that
// makes this object (mutter's linked RemoteDesktop/ScreenCast session,
// kwin's zkde_screencast stream); there is no Wayland connection of this
// class's own -- `pw_stream_connect()` takes the node id directly.
//
// Runs on the caller's wl_event_loop: PipeWire's pw_loop is added as an
// fd source, no dedicated thread.
#pragma once

#include "screencast/frame_source.hpp"

#include <cstdint>
#include <memory>

namespace wraith {

class PipeWireCapture : public FrameSource {
public:
	explicit PipeWireCapture(uint32_t node_id);
	~PipeWireCapture() override;
	PipeWireCapture(const PipeWireCapture &) = delete;
	PipeWireCapture &operator=(const PipeWireCapture &) = delete;

	// Connects a pw_stream directly to the node id (PW_DIRECTION_INPUT,
	// AUTOCONNECT|MAP_BUFFERS) and offers a dmabuf-with-modifiers/MemFd-
	// fallback format pair. Capture callbacks only start firing once
	// PipeWire's stream reaches PW_STREAM_STATE_STREAMING; this call
	// itself only reports the pw_stream_connect setup.
	bool open(const Params &params) override;
	void close() override;
	// Queues the pw_buffer identified by `release_token` back to PipeWire.
	void release(void *release_token) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
