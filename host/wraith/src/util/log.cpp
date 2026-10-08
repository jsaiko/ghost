// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "util/log.hpp"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace wraith {

namespace {

enum class Style { Plain, Colour, Journal };

std::atomic<int> g_threshold{static_cast<int>(LogLevel::Error)};
Style g_style = Style::Plain;

// $JOURNAL_STREAM is "<dev>:<ino>" of the journal socket systemd connected
// stdout/stderr to (systemd.exec(5)); it only counts if stderr is still
// that socket, not something the variable leaked through to.
bool stderr_is_journal() {
	const char *stream = getenv("JOURNAL_STREAM");
	if (!stream) {
		return false;
	}
	unsigned long long dev, ino;
	if (sscanf(stream, "%llu:%llu", &dev, &ino) != 2) {
		return false;
	}
	struct stat st;
	if (fstat(STDERR_FILENO, &st) != 0) {
		return false;
	}
	return st.st_dev == dev && st.st_ino == ino;
}

const char *level_tag(LogLevel level) {
	switch (level) {
	case LogLevel::Error: return "ERROR";
	case LogLevel::Info: return "INFO";
	case LogLevel::Debug: return "DEBUG";
	}
	return "?";
}

// sd-daemon(3) priorities: LOG_ERR, LOG_INFO, LOG_DEBUG.
int journal_priority(LogLevel level) {
	switch (level) {
	case LogLevel::Error: return 3;
	case LogLevel::Info: return 6;
	case LogLevel::Debug: return 7;
	}
	return 6;
}

const char *level_colour(LogLevel level) {
	switch (level) {
	case LogLevel::Error: return "\x1b[1;31m";
	case LogLevel::Info: return "\x1b[1;34m";
	case LogLevel::Debug: return "\x1b[1;90m";
	}
	return "";
}

void vwrite(LogLevel level, const char *file, int line, const char *fmt, va_list args) {
	int saved_errno = errno;

	std::string out;
	char prefix[96];
	int n = 0;
	if (g_style == Style::Journal) {
		n = snprintf(prefix, sizeof(prefix), "<%d>", journal_priority(level));
	} else {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm tm;
		localtime_r(&ts.tv_sec, &tm);
		n = snprintf(prefix, sizeof(prefix), "%s%02d:%02d:%02d.%03ld %-5s%s ",
			g_style == Style::Colour ? level_colour(level) : "", tm.tm_hour, tm.tm_min, tm.tm_sec,
			ts.tv_nsec / 1'000'000, level_tag(level), g_style == Style::Colour ? "\x1b[0m" : "");
	}
	out.append(prefix, n > 0 ? n : 0);
	if (file) {
		n = snprintf(prefix, sizeof(prefix), "[%s:%d] ", file, line);
		out.append(prefix, n > 0 ? n : 0);
	}

	char buf[1024];
	va_list copy;
	va_copy(copy, args);
	n = vsnprintf(buf, sizeof(buf), fmt, copy);
	va_end(copy);
	if (n >= static_cast<int>(sizeof(buf))) {
		size_t start = out.size();
		out.resize(start + n + 1);
		vsnprintf(out.data() + start, n + 1, fmt, args);
		out.resize(start + n);
	} else if (n > 0) {
		out.append(buf, n);
	}
	out.push_back('\n');

	// One write per line: stderr is shared with the encoder and PipeWire
	// threads, and stdio's per-call locking wouldn't keep a line whole.
	const char *p = out.data();
	size_t left = out.size();
	while (left > 0) {
		ssize_t w = ::write(STDERR_FILENO, p, left);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		p += w;
		left -= w;
	}

	errno = saved_errno;
}

} // namespace

void Log::init(LogLevel level) {
	g_threshold.store(static_cast<int>(level), std::memory_order_relaxed);
	if (stderr_is_journal()) {
		g_style = Style::Journal;
	} else if (isatty(STDERR_FILENO)) {
		g_style = Style::Colour;
	} else {
		g_style = Style::Plain;
	}
}

bool Log::enabled(LogLevel level) {
	return static_cast<int>(level) <= g_threshold.load(std::memory_order_relaxed);
}

void Log::write(LogLevel level, const char *file, int line, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vwrite(level, file, line, fmt, args);
	va_end(args);
}

} // namespace wraith
