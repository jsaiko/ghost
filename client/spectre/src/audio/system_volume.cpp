// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/system_volume.hpp"

#include <SDL3/SDL_process.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace spectre {

namespace {

constexpr const char *kSink = "@DEFAULT_AUDIO_SINK@";

// Runs `args` and waits; true on exit status 0. With `output`, stdout is
// captured into it.
bool run(const char *const *args, std::string *output) {
	SDL_Process *process = SDL_CreateProcess(args, output != nullptr);
	if (!process) {
		return false;
	}
	int exit_code = -1;
	if (output) {
		size_t size = 0;
		void *data = SDL_ReadProcess(process, &size, &exit_code);
		if (data) {
			output->assign(static_cast<const char *>(data), size);
			SDL_free(data);
		}
	} else {
		SDL_WaitProcess(process, true, &exit_code);
	}
	SDL_DestroyProcess(process);
	return exit_code == 0;
}

} // namespace

std::optional<SystemVolumeState> get_system_volume() {
	const char *args[] = {"wpctl", "get-volume", kSink, nullptr};
	std::string out;
	if (!run(args, &out)) {
		return std::nullopt;
	}
	// "Volume: 0.40", with " [MUTED]" after it when muted.
	const char *p = std::strstr(out.c_str(), "Volume:");
	if (!p) {
		return std::nullopt;
	}
	char *end = nullptr;
	double volume = std::strtod(p + 7, &end);
	if (end == p + 7) {
		return std::nullopt;
	}
	SystemVolumeState state;
	state.volume = std::clamp(volume, 0.0, 1.0);
	state.muted = out.find("[MUTED]") != std::string::npos;
	return state;
}

bool set_system_volume(double volume, bool unmute) {
	char value[16];
	snprintf(value, sizeof(value), "%.2f", std::clamp(volume, 0.0, 1.0));
	const char *args[] = {"wpctl", "set-volume", kSink, value, nullptr};
	if (!run(args, nullptr)) {
		return false;
	}
	if (unmute) {
		const char *mute_args[] = {"wpctl", "set-mute", kSink, "0", nullptr};
		return run(mute_args, nullptr);
	}
	return true;
}

} // namespace spectre
