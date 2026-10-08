// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wraith's own logger. Every wraith message goes through Log::write; the
// WLOG_* macros below are the call-site interface and stamp the source
// location.
//
// Output is one line on stderr per message, written with a single write(2)
// so lines from the encoder and PipeWire threads never interleave. Under
// systemd (stderr connected to the journal, per $JOURNAL_STREAM) each line
// carries a sd-daemon(3) "<N>" priority prefix and no timestamp, since the
// journal records both; otherwise it gets a local HH:MM:SS.mmm timestamp,
// coloured by level when stderr is a terminal.
#pragma once

namespace wraith {

enum class LogLevel {
	Error = 1,
	Info = 2,
	Debug = 3,
};

class Log {
public:
	// Sets the threshold (messages above it are dropped), picks the output
	// style from the environment. Call once, before anything logs; messages before init are written at the
	// default Error threshold in the plain style.
	static void init(LogLevel level);

	static bool enabled(LogLevel level);

	static void write(LogLevel level, const char *file, int line, const char *fmt, ...)
		__attribute__((format(printf, 4, 5)));
};

} // namespace wraith

#if defined(__FILE_NAME__)
#define WRAITH_LOG_FILE __FILE_NAME__
#else
#define WRAITH_LOG_FILE __FILE__
#endif

#define WLOG(level, ...)                                                                                     \
	do {                                                                                                     \
		if (::wraith::Log::enabled(level))                                                                   \
			::wraith::Log::write(level, WRAITH_LOG_FILE, __LINE__, __VA_ARGS__);                             \
	} while (0)

#define WLOG_ERROR(...) WLOG(::wraith::LogLevel::Error, __VA_ARGS__)
#define WLOG_INFO(...) WLOG(::wraith::LogLevel::Info, __VA_ARGS__)
#define WLOG_DEBUG(...) WLOG(::wraith::LogLevel::Debug, __VA_ARGS__)
