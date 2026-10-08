// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// spectre's logger: the SLOG_* macros are the call-site interface, the
// client-side counterpart of wraith's WLOG_*. Each message is one line on
// stderr, written in a single call so lines from the audio thread never
// interleave with the main loop's. The text is the message alone -- by
// convention it starts with the subsystem ("decoder: ...") -- with no level
// tag or timestamp, since spectre-qt shows the captured output to the user
// as-is.
//
// The threshold comes from $SPECTRE_LOG ("error", "info" or "debug"),
// read once by Log::init(); the default is info. spectre-qt's --debug
// mode runs spectre with SPECTRE_LOG=debug.
#pragma once

#include <string>

#if defined(__GNUC__) || defined(__clang__)
#define SPECTRE_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define SPECTRE_PRINTF_FORMAT(fmt_index, first_arg)
#endif

namespace spectre {

enum class LogLevel {
	Error = 1,
	Info = 2,
	Debug = 3,
};

class Log {
public:
	// Reads $SPECTRE_LOG. Messages logged before this use the default
	// (info) threshold.
	static void init();

	static bool enabled(LogLevel level);

	// Writes `fmt` plus a newline.
	static void write(const char *fmt, ...) SPECTRE_PRINTF_FORMAT(1, 2);

	// The most recent lines written, oldest first, for the support report
	// (wraith --report, gdp-spec.md §7.11). Only what passed the threshold:
	// a client run at the default level has no debug lines to give.
	static std::string recent();
};

} // namespace spectre

#define SLOG(level, ...)                                                                                     \
	do {                                                                                                     \
		if (::spectre::Log::enabled(level)) ::spectre::Log::write(__VA_ARGS__);                              \
	} while (0)

#define SLOG_ERROR(...) SLOG(::spectre::LogLevel::Error, __VA_ARGS__)
#define SLOG_INFO(...) SLOG(::spectre::LogLevel::Info, __VA_ARGS__)
#define SLOG_DEBUG(...) SLOG(::spectre::LogLevel::Debug, __VA_ARGS__)
