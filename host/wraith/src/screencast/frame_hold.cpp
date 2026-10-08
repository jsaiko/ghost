// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/frame_hold.hpp"

namespace wraith {

void *FrameHold::arrived(void *new_token) {
	void *to_release = current_;
	current_ = new_token;
	return to_release;
}

bool FrameHold::forget(void *token) {
	if (!token || token != current_) {
		return false;
	}
	current_ = nullptr;
	return true;
}

std::vector<void *> FrameHold::drain() {
	std::vector<void *> tokens;
	if (current_) {
		tokens.push_back(current_);
	}
	current_ = nullptr;
	return tokens;
}

} // namespace wraith
