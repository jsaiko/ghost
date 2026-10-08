// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// One controller forwarded raw (gdp-spec.md §8.6,
// docs/design/audio-cursor-gamepad.md#raw-controllers): its HID
// interface, opened through SDL's HID API, whose identity and report
// descriptor become the host's uhid device and whose reports are relayed
// unchanged.
//
// Two threads per controller, so neither the main loop's pacing nor a
// slow request adds latency to input:
// - the reader blocks in SDL_hid_read_timeout() and hands each input
//   report to `on_input` as it arrives;
// - the worker performs the host's output, get-report and set-report
//   requests, each a blocking USB control transfer or Bluetooth round
//   trip, and hands the replies to `on_reply`.
// Both callbacks run on those threads; StreamSession points them at
// SessionClient's thread-safe senders. The destructor stops and joins
// both before closing the device.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_hid_device;

namespace spectre {

class RawController {
public:
	// HidConnect's fields (gdp-spec.md §8.6), as the platform reports
	// them.
	struct Identity {
		std::string name;
		uint32_t bus = 0; // Linux BUS_*: 3 USB, 5 Bluetooth
		uint32_t vendor_id = 0;
		uint32_t product_id = 0;
		uint32_t version = 0;
		std::string uniq;
		std::vector<uint8_t> report_descriptor;
	};

	using InputFn = std::function<void(const uint8_t *data, size_t len)>;
	// `get` tells a get-report reply (with `data`) from a set-report one.
	using ReplyFn =
		std::function<void(uint32_t request_id, bool get, bool failed, const uint8_t *data, size_t len)>;

	// The HID interface behind the SDL joystick whose path is
	// `joystick_path` (SDL_GetJoystickPathForID): the path itself when
	// SDL's HIDAPI driver handles the controller, or on Linux the hidraw
	// node beside an evdev one. nullptr, with `*why` set, when there is
	// none, it can't be opened (a hidraw node this user can't open), it is
	// on neither USB nor Bluetooth, or its descriptor can't be read.
	static std::unique_ptr<RawController> open(const char *joystick_path, std::string *why);
	~RawController();
	RawController(const RawController &) = delete;
	RawController &operator=(const RawController &) = delete;

	const Identity &identity() const { return identity_; }

	// Starts both threads. Call once.
	void start(InputFn on_input, ReplyFn on_reply);

	// The host's requests, queued for the worker. `type` is the wire's
	// HidReportType (1 input, 2 output, 3 feature).
	void output(const std::string &data);
	void get_report(uint32_t request_id, uint32_t report_id, int type);
	void set_report(uint32_t request_id, uint32_t report_id, int type, const std::string &data);

private:
	struct Request {
		enum Kind { kOutput, kGet, kSet } kind;
		uint32_t request_id = 0;
		uint32_t report_id = 0;
		int type = 0;
		std::string data;
	};

	RawController(SDL_hid_device *dev, Identity identity);
	void read_loop();
	void work_loop();
	void perform(const Request &req);
	void enqueue(Request req);

	SDL_hid_device *dev_;
	Identity identity_;
	InputFn on_input_;
	ReplyFn on_reply_;
	std::thread reader_;
	std::thread worker_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Request> queue_;
	bool stopping_ = false;
};

} // namespace spectre
