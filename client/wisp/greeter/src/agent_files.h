// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The files the greeter shares with wisp-agent in /run/wisp
// (docs/design/wisp.md): the agent writes profile.json, the settings Veil
// sends; the greeter writes session.json while spectre runs, and the agent
// tells Veil (and signals spectre's pid in it when an administrator logs
// the session out).
#pragma once

#include "spectre_settings.h"

#include <QString>

// profile.json as SpectreSettings. With no file (Veil not reached yet) or
// a field missing, SpectreSettings' own defaults.
SpectreSettings load_profile(const QString &run_dir);

// Veil's own default, for a client that hasn't heard from it yet.
constexpr unsigned kDefaultDisplaySleepMinutes = 20;

// profile.json's debug_logging: whether to run spectre with
// SPECTRE_LOG=debug. Not a SpectreSettings field (spectre-qt has -D for it).
bool load_debug_logging(const QString &run_dir);

// profile.json's display_sleep_minutes (0: never).
unsigned load_display_sleep_minutes(const QString &run_dir);

void write_session(const QString &run_dir, const QString &user, const QString &host_name, qint64 spectre_pid);
void clear_session(const QString &run_dir);
