// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test (same convention as the other host/wraith/tests/):
// FrameHold's hold/release bookkeeping (it holds exactly the newest
// frame) is exactly where a leaked or double-released PipeWire
// buffer token would hide, so it gets its own pure translation
// unit -- no PipeWire/libei types involved, opaque `void *` tokens stand
// in for pw_buffer* here.
#include "screencast/frame_hold.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>

using namespace wraith;

namespace {

int tokens[8];

void test_empty_hold_releases_nothing() {
	FrameHold hold;
	assert(hold.current() == nullptr);
	assert(hold.drain().empty());
}

void test_first_arrival_releases_nothing() {
	FrameHold hold;
	assert(hold.arrived(&tokens[0]) == nullptr);
	assert(hold.current() == &tokens[0]);
}

// The second arrival is the first point anything becomes safe to release,
// and it must be the previously held token, never the new one.
void test_second_arrival_releases_the_previous() {
	FrameHold hold;
	hold.arrived(&tokens[0]);
	void *released = hold.arrived(&tokens[1]);
	assert(released == &tokens[0]);
	assert(hold.current() == &tokens[1]);
}

// A long run: each arrival after the first must release exactly the
// token from the arrival before, exactly once, and current() must always
// be the most recent one -- what redeliver_frame() re-pushes. Only
// ever one token outstanding: the producer's pool is small (see frame_hold.hpp).
void test_steady_state_always_releases_exactly_one_arrival_back() {
	FrameHold hold;
	for (int i = 0; i < 8; i++) {
		void *released = hold.arrived(&tokens[i]);
		if (i == 0) {
			assert(released == nullptr);
		} else {
			assert(released == &tokens[i - 1]);
		}
		assert(hold.current() == &tokens[i]);
	}
}

// Teardown must hand back the token still held so nothing leaks when the
// session ends mid-stream.
void test_drain_returns_the_held_token_and_clears() {
	FrameHold hold;
	hold.arrived(&tokens[0]);
	hold.arrived(&tokens[1]);
	std::vector<void *> drained = hold.drain();
	assert(drained.size() == 1);
	assert(drained[0] == &tokens[1]);

	// A second drain (e.g. a repeated teardown call) must not re-yield
	// tokens already handed back.
	assert(hold.drain().empty());
	assert(hold.current() == nullptr);
}

// The producer withdrawing a buffer (PipeWire remove_buffer) must make the
// hold drop it silently: a later drain() must not hand back a token whose
// pw_buffer is already freed.
void test_forget_drops_the_held_token_without_releasing() {
	FrameHold hold;
	hold.arrived(&tokens[0]);
	hold.arrived(&tokens[1]);

	// Forgetting a token that isn't held (already released, or never
	// seen) changes nothing.
	assert(!hold.forget(&tokens[0]));
	assert(!hold.forget(nullptr));
	assert(hold.current() == &tokens[1]);

	assert(hold.forget(&tokens[1]));
	assert(hold.current() == nullptr);
	assert(hold.drain().empty());

	// Forgetting is not a release: the next arrival has nothing to hand
	// back, exactly like a first arrival.
	assert(hold.arrived(&tokens[2]) == nullptr);
	assert(hold.current() == &tokens[2]);
}

} // namespace

int main() {
	test_empty_hold_releases_nothing();
	test_first_arrival_releases_nothing();
	test_second_arrival_releases_the_previous();
	test_steady_state_always_releases_exactly_one_arrival_back();
	test_drain_returns_the_held_token_and_clears();
	test_forget_drops_the_held_token_without_releasing();
	printf("frame_hold_test: ok\n");
	return 0;
}
