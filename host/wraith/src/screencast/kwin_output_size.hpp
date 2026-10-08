// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// set_kwin_output_size: makes kwin's `--virtual` output the size wraith
// streams at (docs/design/capture-backends.md#kde-plasma), over
// kwin's own output-configuration protocol -- kde_output_management_v2's
// custom mode list plus a mode switch, the same two steps
// `kscreen-doctor output.Virtual-0.addCustomMode.W.H.R.full` and
// `.mode.WxH@R` take. The ext backend does the same over
// zwlr_output_manager_v1 (ext_remote_session.cpp), which kwin doesn't
// export. This is what lets kwin start with no size on its command line
// (packaging/ghost/sessions/plasma-screencast) and lets a client's requested startup size
// be applied without restarting Plasma.
//
// kwin saves the mode to ~/.config/kwinoutputconfig.json under the virtual
// output's own entry, which no physical monitor ever matches; the next
// ghost session's kwin starts at that size and is set again here anyway.
#pragma once

#include <cstdint>
#include <string>

namespace wraith {

class WaylandClient;

// Blocking (a few round trips on `client`). True once the first
// kde_output_device_v2 is at width x height -- immediately if it already
// was. False with `*error` set if kwin hides kde_output_management_v2 from
// wraith (the desktop-file trust rule, packaging/desktop/wraith.desktop.in),
// advertises a version without custom modes (< 18), or refuses the change.
bool set_kwin_output_size(WaylandClient &client, uint32_t width, uint32_t height, std::string *error);

} // namespace wraith
