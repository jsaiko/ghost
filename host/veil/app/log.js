// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser client's log: what goes to the console, also kept as the
// last few hundred lines so a DiagnosticsReport (gdp-spec.md §7.11) can
// carry it into the user's `wraith --report`, as spectre's Log::recent()
// does.

const MAX_LINES = 300;
const MAX_LINE_CHARS = 500;
const lines = [];
const start = performance.now();

function record(level, args) {
  const text = args
    .map((a) => (a instanceof Error ? `${a.name}: ${a.message}` : typeof a === "string" ? a : safeString(a)))
    .join(" ")
    .slice(0, MAX_LINE_CHARS);
  const seconds = ((performance.now() - start) / 1000).toFixed(3);
  lines.push(`[${seconds}] ${level}: ${text}`);
  if (lines.length > MAX_LINES) lines.shift();
}

function safeString(value) {
  try {
    return typeof value === "object" && value !== null ? JSON.stringify(value) : String(value);
  } catch (_) {
    return String(value);
  }
}

export const log = {
  info(...args) {
    record("info", args);
    console.info(...args);
  },
  warn(...args) {
    record("warn", args);
    console.warn(...args);
  },
};

/// The kept lines, oldest first, one per line.
export function recentLog() {
  return lines.join("\n");
}
