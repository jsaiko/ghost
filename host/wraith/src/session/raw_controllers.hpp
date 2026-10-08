// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Raw HID controllers (gdp-spec.md §8.6,
// docs/design/audio-cursor-gamepad.md#raw-controllers): a client's
// controller recreated on the host through uhid, with its own identity
// and report descriptor, so the host's own driver for it binds.
// ghostseat creates the device (seat_client.hpp's create_hid_device) and
// hands over an fd it relays to the device; this class relays between
// that fd and the client:
//
// - HidInput -> UHID_INPUT2, held back until ghostseat's DeviceVerified;
// - UHID_OUTPUT, UHID_GET_REPORT, UHID_SET_REPORT -> HidOutput,
//   HidGetReport, HidSetReport, sent through `send`;
// - HidGetReportReply, HidSetReportReply -> UHID_GET_REPORT_REPLY,
//   UHID_SET_REPORT_REPLY.
//
// Requests get wire IDs of this class's own, never reused for the
// session's life, mapped back to the slot, the device generation and the
// kernel's ID, so a late reply can't land on a replacement device.
// ghostseat's verdict arrives on the CreateDevice connection, which
// stays open and is watched from the event loop; a ControlError there
// (ghostseat has destroyed the device) or a failed create becomes
// HidRejected.
//
// Per-client state, like GamepadDevices: the session disconnects every
// slot when the client goes away.
#pragma once

#include "gdp/framing.hpp"
#include "gdp/gamepad.hpp"
#include "util/event_source.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

struct wl_event_loop;

namespace gdp::session {
class HidConnect;
class HidInput;
class HidGetReportReply;
class HidSetReportReply;
class HostInputEnvelope;
} // namespace gdp::session

namespace wraith {

class RawControllers {
public:
	using Send = std::function<void(const gdp::session::HostInputEnvelope &)>;

	// `control_socket` is the -G control socket path. `send` puts a
	// message on the attached client's input stream.
	RawControllers(std::string control_socket, struct wl_event_loop *loop, Send send);
	~RawControllers();
	RawControllers(const RawControllers &) = delete;
	RawControllers &operator=(const RawControllers &) = delete;

	// Creates the device for the connect's slot, replacing what was
	// there. On failure the slot stays empty and the client gets
	// HidRejected.
	void connect(const gdp::session::HidConnect &connect);
	// Destroys the slot's device; no-op for an empty slot.
	void disconnect(uint32_t index);
	void disconnect_all();
	bool connected(uint32_t index) const;

	void input(const gdp::session::HidInput &input);
	void get_report_reply(const gdp::session::HidGetReportReply &reply);
	void set_report_reply(const gdp::session::HidSetReportReply &reply);

private:
	struct Slot {
		RawControllers *owner = nullptr;
		uint32_t index = 0;
		int fd = -1; // the uhid device; close() destroys it
		EventSource fd_source;
		int verdict_fd = -1; // the CreateDevice connection, until ghostseat's verdict
		EventSource verdict_source;
		gdp::FrameReader verdict_reader;
		bool verified = false;
		uint64_t generation = 0;
		bool write_failed = false; // logged once per device, not per report
		std::string name;
	};
	struct Request {
		uint32_t index;
		uint64_t generation;
		uint32_t kernel_id;
		bool get; // GET_REPORT, else SET_REPORT
		int64_t at_us;
	};

	static int on_device_event(int fd, uint32_t mask, void *data);
	static int on_verdict(int fd, uint32_t mask, void *data);
	void read_device(Slot &slot);
	void handle_device_event(Slot &slot, const uint8_t *buf, size_t len);
	// Drains ghostseat's verdict from the slot's connection. False once the
	// slot has been torn down (rejected).
	bool read_verdict(Slot &slot);
	bool drain_verdict(Slot &slot);
	void close_verdict(Slot &slot);
	void reject(uint32_t index, const std::string &reason);
	void write_event(Slot &slot, const std::vector<uint8_t> &ev);
	uint32_t add_request(const Slot &slot, uint32_t kernel_id, bool get);
	// The slot a reply is for, or nullptr when its request is unknown or
	// its device has been replaced since; removes the request either way.
	Slot *take_request(uint32_t request_id, uint32_t index, bool get, uint32_t *kernel_id);

	std::string control_socket_;
	struct wl_event_loop *loop_;
	Send send_;
	Slot slots_[gdp::kMaxGamepads];
	uint64_t next_generation_ = 1;
	uint32_t next_request_id_ = 1;
	std::unordered_map<uint32_t, Request> requests_;
};

} // namespace wraith
