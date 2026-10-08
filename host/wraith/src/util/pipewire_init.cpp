// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "util/pipewire_init.hpp"

#include <pipewire/pipewire.h>

#include <mutex>

namespace wraith {

void ensure_pw_init() {
	static std::once_flag once;
	std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

} // namespace wraith
