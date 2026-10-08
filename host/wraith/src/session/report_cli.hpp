// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// `wraith --report [directory]`: run inside the remote desktop, collects what
// a person helping with a problem would ask for into
// <directory>/ghost-report-<date>-<time>.tar.gz (default: the home directory):
//
//   wraith-status.txt   what the running session negotiated (via the report
//                       socket, report_server.hpp)
//   client.txt          the attached client's own report (gdp-spec.md §7.11)
//   wraith-journal.txt  this user's wraith.service log
//   desktop-warnings.txt  warnings from the compositor, PipeWire and the portals
//   wraith.toml         the settings file
//   system.txt          kernel, distribution, the session's environment
//   gpu.txt             render nodes, vainfo, vulkaninfo
//
// Nothing is sent anywhere. ghostd's and Veil's logs belong to root and are
// not included; the report says so. A missing piece is noted in the report,
// not a reason to fail.
#pragma once

namespace wraith {

// The exit status of the command: 0 once the report is written.
int run_report_mode(const char *directory);

} // namespace wraith
