// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace spectre {

namespace {

std::atomic<int> g_threshold{static_cast<int>(LogLevel::Info)};

// The last kRecentBytes of output, a line at a time. Written from the audio
// and network threads as well as the main loop.
constexpr size_t kRecentBytes = 256 * 1024;
std::mutex g_recent_mutex;
std::deque<std::string> g_recent;
size_t g_recent_size = 0;

void remember(const char *line, size_t len) {
	std::lock_guard<std::mutex> lock(g_recent_mutex);
	g_recent.emplace_back(line, len);
	g_recent_size += len;
	while (g_recent_size > kRecentBytes && g_recent.size() > 1) {
		g_recent_size -= g_recent.front().size();
		g_recent.pop_front();
	}
}

} // namespace

void Log::init() {
	const char *value = getenv("SPECTRE_LOG");
	if (!value || !*value) {
		return;
	}
	if (strcmp(value, "error") == 0) {
		g_threshold = static_cast<int>(LogLevel::Error);
	} else if (strcmp(value, "info") == 0) {
		g_threshold = static_cast<int>(LogLevel::Info);
	} else if (strcmp(value, "debug") == 0) {
		g_threshold = static_cast<int>(LogLevel::Debug);
	} else {
		write("spectre: ignoring SPECTRE_LOG=\"%s\" (expected error, info or debug)", value);
	}
}

bool Log::enabled(LogLevel level) {
	return static_cast<int>(level) <= g_threshold.load(std::memory_order_relaxed);
}

void Log::write(const char *fmt, ...) {
	char line[1024];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 2;
	line[len] = '\n';
	line[len + 1] = '\0';
	fputs(line, stderr);
	remember(line, len + 1);
}

std::string Log::recent() {
	std::lock_guard<std::mutex> lock(g_recent_mutex);
	std::string text;
	text.reserve(g_recent_size);
	for (const std::string &line : g_recent) {
		text += line;
	}
	return text;
}

} // namespace spectre
