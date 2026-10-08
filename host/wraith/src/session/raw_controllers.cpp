// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/raw_controllers.hpp"

#include "control.pb.h"
#include "session.pb.h"
#include "session/seat_client.hpp"
#include "session/uhid_codec.hpp"

#include <unistd.h>
#include <wayland-server-core.h>

#include <cerrno>
#include <cstring>

#include "util/clock.hpp"
#include "util/log.hpp"

namespace wraith {

namespace {

// A request the client never answered: the kernel gave up on it after
// 5 s, so its entry is only kept a while longer, in case the reply is
// merely late.
constexpr int64_t kRequestForgetUs = 30'000'000;
constexpr size_t kRequestSweepAt = 64;

gdp::session::HidReportType wire_type(uint8_t rtype) {
	switch (rtype) {
	case uhid::kFeatureReport: return gdp::session::HID_REPORT_TYPE_FEATURE;
	case uhid::kOutputReport: return gdp::session::HID_REPORT_TYPE_OUTPUT;
	case uhid::kInputReport: return gdp::session::HID_REPORT_TYPE_INPUT;
	default: return gdp::session::HID_REPORT_TYPE_UNSPECIFIED;
	}
}

} // namespace

RawControllers::RawControllers(std::string control_socket, struct wl_event_loop *loop, Send send)
	: control_socket_(std::move(control_socket)), loop_(loop), send_(std::move(send)) {
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		slots_[i].owner = this;
		slots_[i].index = i;
	}
}

RawControllers::~RawControllers() {
	disconnect_all();
}

void RawControllers::connect(const gdp::session::HidConnect &c) {
	uint32_t index = c.pad_index();
	if (index >= gdp::kMaxGamepads) {
		WLOG_ERROR("hid: client used slot %u, max is %u", index, gdp::kMaxGamepads - 1);
		return;
	}
	disconnect(index);
	ghost::control::HidIdentity identity;
	identity.set_bus(c.bus());
	identity.set_vendor_id(c.vendor_id());
	identity.set_product_id(c.product_id());
	identity.set_version(c.version());
	identity.set_uniq(c.uniq());
	identity.set_report_descriptor(c.report_descriptor());

	Slot &slot = slots_[index];
	std::string node, error;
	int conn = -1;
	slot.verdict_reader = gdp::FrameReader{};
	int fd = SeatClient::create_hid_device(control_socket_, index, c.name(), identity, &node, &conn,
		&slot.verdict_reader, &error);
	if (fd < 0) {
		WLOG_ERROR("hid: slot %u (\"%s\" %04x:%04x): ghostseat could not create a device: %s", index,
			c.name().c_str(), c.vendor_id(), c.product_id(), error.c_str());
		reject(index, error);
		return;
	}
	slot.fd = fd;
	slot.verdict_fd = conn;
	slot.verified = false;
	slot.write_failed = false;
	slot.generation = next_generation_++;
	slot.name = c.name();
	slot.fd_source.reset(wl_event_loop_add_fd(loop_, fd, WL_EVENT_READABLE, on_device_event, &slot));
	slot.verdict_source.reset(wl_event_loop_add_fd(loop_, conn, WL_EVENT_READABLE, on_verdict, &slot));
	WLOG_INFO("hid: slot %u (\"%s\" %04x:%04x, bus %u) is %s, waiting for its driver", index,
		c.name().c_str(), c.vendor_id(), c.product_id(), c.bus(), node.c_str());
	// The verdict may have come with DeviceCreated, already read.
	drain_verdict(slot);
}

void RawControllers::disconnect(uint32_t index) {
	if (index >= gdp::kMaxGamepads) {
		return;
	}
	Slot &slot = slots_[index];
	close_verdict(slot);
	if (slot.fd < 0) {
		return;
	}
	slot.fd_source.reset();
	// close() on a uhid fd is UHID_DESTROY: every node goes, and whatever
	// had them open sees an unplug. The kernel fails any request still
	// waiting for a reply.
	close(slot.fd);
	slot.fd = -1;
	slot.verified = false;
	for (auto it = requests_.begin(); it != requests_.end();) {
		it = it->second.index == index ? requests_.erase(it) : std::next(it);
	}
	WLOG_INFO("hid: slot %u removed", index);
}

void RawControllers::disconnect_all() {
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		disconnect(i);
	}
}

bool RawControllers::connected(uint32_t index) const {
	return index < gdp::kMaxGamepads && slots_[index].fd >= 0;
}

void RawControllers::input(const gdp::session::HidInput &in) {
	if (!connected(in.pad_index())) {
		return;
	}
	Slot &slot = slots_[in.pad_index()];
	if (!slot.verified) {
		// gdp-spec.md §8.6: dropped until ghostseat has checked what the
		// driver made of the device.
		return;
	}
	const std::string &data = in.data();
	write_event(slot, uhid::input2(reinterpret_cast<const uint8_t *>(data.data()), data.size()));
}

void RawControllers::get_report_reply(const gdp::session::HidGetReportReply &r) {
	uint32_t kernel_id = 0;
	Slot *slot = take_request(r.request_id(), r.pad_index(), /*get=*/true, &kernel_id);
	if (!slot) {
		return;
	}
	const std::string &data = r.data();
	write_event(*slot,
		uhid::get_report_reply(kernel_id, r.failed() ? EIO : 0,
			reinterpret_cast<const uint8_t *>(data.data()), data.size()));
}

void RawControllers::set_report_reply(const gdp::session::HidSetReportReply &r) {
	uint32_t kernel_id = 0;
	Slot *slot = take_request(r.request_id(), r.pad_index(), /*get=*/false, &kernel_id);
	if (!slot) {
		return;
	}
	write_event(*slot, uhid::set_report_reply(kernel_id, r.failed() ? EIO : 0));
}

int RawControllers::on_device_event(int, uint32_t, void *data) {
	auto *slot = static_cast<Slot *>(data);
	slot->owner->read_device(*slot);
	return 0;
}

void RawControllers::read_device(Slot &slot) {
	// One event per read() (one packet from ghostseat's relay); the fd is
	// non-blocking, so the loop ends at EAGAIN.
	uint8_t buf[uhid::kEventSize];
	while (slot.fd >= 0) {
		ssize_t n = read(slot.fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno != EAGAIN && errno != EINTR) {
				WLOG_ERROR("hid: slot %u: read failed: %s", slot.index, strerror(errno));
				slot.fd_source.reset();
			}
			if (errno != EINTR) {
				return;
			}
			continue;
		}
		if (n == 0) {
			return;
		}
		handle_device_event(slot, buf, (size_t)n);
	}
}

void RawControllers::handle_device_event(Slot &slot, const uint8_t *buf, size_t len) {
	uhid::Event ev;
	if (!uhid::parse(buf, len, &ev)) {
		WLOG_ERROR("hid: slot %u: short or malformed uhid event (%zu bytes)", slot.index, len);
		return;
	}
	gdp::session::HostInputEnvelope env;
	switch (ev.type) {
	case uhid::kOutput: {
		auto *out = env.mutable_hid_output();
		out->set_pad_index(slot.index);
		out->set_data(ev.data, ev.size);
		break;
	}
	case uhid::kGetReport: {
		auto *get = env.mutable_hid_get_report();
		get->set_pad_index(slot.index);
		get->set_request_id(add_request(slot, ev.id, /*get=*/true));
		get->set_report_id(ev.rnum);
		get->set_type(wire_type(ev.rtype));
		break;
	}
	case uhid::kSetReport: {
		auto *set = env.mutable_hid_set_report();
		set->set_pad_index(slot.index);
		set->set_request_id(add_request(slot, ev.id, /*get=*/false));
		set->set_report_id(ev.rnum);
		set->set_type(wire_type(ev.rtype));
		set->set_data(ev.data, ev.size);
		break;
	}
	case uhid::kStart: WLOG_DEBUG("hid: slot %u: driver started", slot.index); return;
	case uhid::kStop: WLOG_DEBUG("hid: slot %u: driver stopped", slot.index); return;
	case uhid::kOpen: WLOG_DEBUG("hid: slot %u: opened", slot.index); return;
	case uhid::kClose: WLOG_DEBUG("hid: slot %u: closed", slot.index); return;
	default: return;
	}
	send_(env);
}

uint32_t RawControllers::add_request(const Slot &slot, uint32_t kernel_id, bool get) {
	int64_t now = monotonic_now_us();
	if (requests_.size() >= kRequestSweepAt) {
		for (auto it = requests_.begin(); it != requests_.end();) {
			it = now - it->second.at_us > kRequestForgetUs ? requests_.erase(it) : std::next(it);
		}
	}
	uint32_t id = next_request_id_++;
	if (next_request_id_ == 0) {
		next_request_id_ = 1;
	}
	requests_[id] = Request{slot.index, slot.generation, kernel_id, get, now};
	return id;
}

RawControllers::Slot *RawControllers::take_request(uint32_t request_id, uint32_t index, bool get,
	uint32_t *kernel_id) {
	auto it = requests_.find(request_id);
	if (it == requests_.end()) {
		return nullptr;
	}
	Request req = it->second;
	requests_.erase(it);
	if (req.index != index || req.get != get || !connected(index) ||
		slots_[index].generation != req.generation) {
		return nullptr;
	}
	*kernel_id = req.kernel_id;
	return &slots_[index];
}

void RawControllers::write_event(Slot &slot, const std::vector<uint8_t> &ev) {
	if (slot.fd < 0) {
		return;
	}
	if (write(slot.fd, ev.data(), ev.size()) != (ssize_t)ev.size() && !slot.write_failed) {
		// Expected for a moment around a driver's (re)start: uhid takes
		// input only while a driver is bound.
		slot.write_failed = true;
		WLOG_ERROR("hid: slot %u: write failed: %s", slot.index, strerror(errno));
	}
}

int RawControllers::on_verdict(int, uint32_t, void *data) {
	auto *slot = static_cast<Slot *>(data);
	slot->owner->read_verdict(*slot);
	return 0;
}

bool RawControllers::read_verdict(Slot &slot) {
	uint8_t buf[4096];
	ssize_t n = read(slot.verdict_fd, buf, sizeof(buf));
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
		return true;
	}
	if (n <= 0) {
		uint32_t index = slot.index;
		reject(index, "ghostseat closed the connection without a verdict");
		return false;
	}
	slot.verdict_reader.feed(buf, (size_t)n);
	return drain_verdict(slot);
}

bool RawControllers::drain_verdict(Slot &slot) {
	ghost::control::ControlEnvelope env;
	for (;;) {
		gdp::FrameReader::Result r = slot.verdict_reader.drain(&env);
		if (r == gdp::FrameReader::Result::kIncomplete) {
			return true;
		}
		uint32_t index = slot.index;
		if (r != gdp::FrameReader::Result::kOk) {
			reject(index, "malformed verdict from ghostseat");
			return false;
		}
		if (env.has_device_verified()) {
			slot.verified = true;
			close_verdict(slot);
			WLOG_INFO("hid: slot %u (\"%s\"): driver %s bound, reports flowing", index, slot.name.c_str(),
				env.device_verified().driver().c_str());
			return true;
		}
		if (env.has_error()) {
			WLOG_ERROR("hid: slot %u (\"%s\"): %s", index, slot.name.c_str(), env.error().message().c_str());
			reject(index, env.error().message());
			return false;
		}
		// Anything else is ghostseat's mistake; keep waiting for the verdict.
	}
}

void RawControllers::close_verdict(Slot &slot) {
	slot.verdict_source.reset();
	if (slot.verdict_fd >= 0) {
		close(slot.verdict_fd);
		slot.verdict_fd = -1;
	}
}

void RawControllers::reject(uint32_t index, const std::string &reason) {
	disconnect(index);
	gdp::session::HostInputEnvelope env;
	auto *rejected = env.mutable_hid_rejected();
	rejected->set_pad_index(index);
	rejected->set_reason(reason);
	send_(env);
}

} // namespace wraith
