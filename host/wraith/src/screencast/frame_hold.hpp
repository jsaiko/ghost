// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Which capture-side dmabuf release token ScreencastHost is allowed to
// give back to PipeWire, and which one to re-push on redeliver_frame()
// (docs/design/capture-backends.md).
//
// A compositor's screencast stream is damage-driven: it sends nothing for
// a static screen, and wraith doesn't own the compositor's rendering, so it
// has no lever to force a fresh one. A keyframe request on an idle desktop
// instead means "re-encode the last frame we
// already have." That requires holding onto it instead of releasing it
// back to the producer's buffer pool the instant a new one arrives.
//
// Exactly one frame is held: the newest. Encoder::push() consumes its
// dmabuf synchronously (encoder.hpp: the fds only have to stay valid for
// the duration of the call), so the moment a newer frame has arrived the
// older one has nothing left to do and goes straight back. Holding more
// starves the producer: the negotiated pool is at most 4 buffers
// (pipewire_capture.cpp's SPA_PARAM_Buffers) and a compositor silently
// drops a frame -- video content included -- whenever it finds no free
// buffer to render into, which with two held stretches 16 ms frame
// intervals to 21-25 ms. No PipeWire/libei types here, so this is a pure,
// independently unit-testable translation unit
// (host/wraith/tests/frame_hold_test.cpp) -- exactly where a leaked or
// double-released token would otherwise hide.
#pragma once

#include <vector>

namespace wraith {

class FrameHold {
public:
	// A new frame arrived, identified by its capture-side release token
	// (opaque to this class -- PipeWireCapture's pw_buffer* in
	// practice). Returns the token that is now safe to release (the
	// previously held one), or nullptr on the first arrival.
	void *arrived(void *new_token);

	// The token for the most recently arrived frame; nullptr before the
	// first frame arrives.
	void *current() const { return current_; }

	// The producer withdrew this buffer (PipeWire's remove_buffer stream
	// event: mutter went away, or the stream is being torn down). If it
	// is the held token, forget it *without* releasing it -- the
	// pw_buffer behind it is already freed, and queueing it back would
	// write into freed memory.
	// Returns true if it was the held token, so the caller can also drop
	// whatever it derived from that frame (dmabuf fds, which PipeWire
	// closed along with the buffer).
	bool forget(void *token);

	// Teardown: every token still held (at most one), so the caller can
	// release it rather than leaking the producer's buffer. Clears this instance.
	std::vector<void *> drain();

private:
	void *current_ = nullptr;
};

} // namespace wraith
