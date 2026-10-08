// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// DataControlClipboard: clipboard sync for the screencast backends whose
// compositor wraith reaches as a plain Wayland client -- screencast-ext
// and screencast-kwin -- over ext-data-control-v1.
//
// One class serves both. The ext_ variant is deliberate, not the older
// zwlr_data_control_unstable_v1: kwin 6.6 (the version this was written
// against) exports *only* ext_data_control_manager_v1, while labwc 0.9 /
// wlroots 0.19 export both. Unlike the core wl_data_device protocol,
// data-control needs no keyboard focus in either direction, which is what
// a headless session needs -- wraith has no surface to focus.
//
// Primary selection is deliberately out of scope (gdp-spec.md §7.9):
// the protocol surfaces it, and this ignores it.
#pragma once

#include "session/clipboard_sync.hpp"

#include <memory>
#include <string>

struct wl_event_loop;

namespace wraith {

class WaylandClient;

class DataControlClipboard : public ClipboardSink {
public:
	// Null if the compositor doesn't advertise ext_data_control_manager_v1
	// -- the backend then simply has no clipboard sink and the session
	// never negotiates the capability (session/gdp_session.cpp).
	// `client` must outlive the returned object.
	static std::unique_ptr<DataControlClipboard> create(WaylandClient &client, struct wl_event_loop *loop);

	~DataControlClipboard() override;

	void set_text(const std::string &utf8) override;
	void close() override;

private:
	struct Impl;
	DataControlClipboard(struct wl_event_loop *loop, std::unique_ptr<Impl> impl);

	// Impl's protocol callbacks reach the ClipboardPipes pool through
	// these (it's protected on the base class, and Impl is not a member
	// function of this one).
	void read_pipe(int fd, const std::string &mime);
	void write_pipe(int fd, std::string payload);

	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
