// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "input/raw_controller.hpp"

#include "gdp/gamepad.hpp"
#include "log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace spectre {

namespace {

// Linux's BUS_* values, which the wire uses whatever the client's OS.
constexpr uint32_t kBusUsb = 0x03;
constexpr uint32_t kBusBluetooth = 0x05;
// HidReportType on the wire.
constexpr int kTypeInput = 1;
constexpr int kTypeOutput = 2;
constexpr int kTypeFeature = 3;
// How often the reader looks up from a quiet controller to see whether
// it should stop.
constexpr int kReadTimeoutMs = 100;

std::string utf8(const wchar_t *w) {
	if (!w || !*w) {
		return {};
	}
	char *s = SDL_iconv_wchar_utf8(w);
	if (!s) {
		return {};
	}
	std::string out(s);
	SDL_free(s);
	return out;
}

// The HID interface for a joystick path, or "" if there is none to try.
std::string hid_path_for(const char *joystick_path) {
	if (!joystick_path || !*joystick_path) {
		return {};
	}
	std::string path = joystick_path;
#ifdef __linux__
	// SDL's evdev driver: find the hidraw node the same HID device has,
	// /sys/class/input/eventN/device (inputM) /device (the HID device)
	// /hidraw/hidrawK. Not every evdev controller has one (an Xbox pad on
	// xpad is USB but not HID).
	const char *name = strrchr(joystick_path, '/');
	if (path.rfind("/dev/input/event", 0) == 0 && name) {
		std::string dir = std::string("/sys/class/input/") + (name + 1) + "/device/device/hidraw";
		char **entries = SDL_GlobDirectory(dir.c_str(), "hidraw*", 0, nullptr);
		std::string found;
		if (entries) {
			if (entries[0]) {
				found = std::string("/dev/") + entries[0];
			}
			SDL_free(entries);
		}
		return found;
	}
#endif
	return path;
}

std::string bluetooth_address(std::string serial) {
	// Linux reports a Bluetooth controller's address as aa:bb:cc:dd:ee:ff
	// already; elsewhere it may come as twelve bare hex digits. Drivers
	// on the host (hid-playstation) parse the colon form.
	bool bare = serial.size() == 12 &&
		std::all_of(serial.begin(), serial.end(), [](char c) { return isxdigit((unsigned char)c) != 0; });
	if (bare) {
		std::string out;
		for (size_t i = 0; i < 12; i += 2) {
			if (i) {
				out += ':';
			}
			out += serial.substr(i, 2);
		}
		serial = out;
	}
	for (char &c : serial) {
		c = (char)tolower((unsigned char)c);
	}
	return serial;
}

// What HidConnect.uniq may hold: printable ASCII, short.
std::string clean_uniq(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (c > 0x20 && c < 0x7f && out.size() < 63) {
			out += c;
		}
	}
	return out;
}

} // namespace

std::unique_ptr<RawController> RawController::open(const char *joystick_path, std::string *why) {
	std::string path = hid_path_for(joystick_path);
	if (path.empty()) {
		*why = "no HID interface";
		return nullptr;
	}
	SDL_hid_device *dev = SDL_hid_open_path(path.c_str());
	if (!dev) {
		*why = std::string("can't open ") + path + ": " + SDL_GetError();
		return nullptr;
	}
	SDL_hid_device_info *info = SDL_hid_get_device_info(dev);
	if (!info) {
		*why = std::string("no device info: ") + SDL_GetError();
		SDL_hid_close(dev);
		return nullptr;
	}
	Identity id;
	switch (info->bus_type) {
	case SDL_HID_API_BUS_USB: id.bus = kBusUsb; break;
	case SDL_HID_API_BUS_BLUETOOTH: id.bus = kBusBluetooth; break;
	default:
		*why = "neither USB nor Bluetooth";
		SDL_hid_close(dev);
		return nullptr;
	}
	id.vendor_id = info->vendor_id;
	id.product_id = info->product_id;
	id.version = info->release_number;
	std::string product = utf8(info->product_string);
	std::string manufacturer = utf8(info->manufacturer_string);
	// Named the way Linux's usbhid names a USB device ("manufacturer
	// product" unless the product already starts with it), so the host's
	// device carries the name it would have had plugged in there; a
	// Bluetooth device's name is the one it announces.
	id.name = product;
	if (id.bus == kBusUsb && !manufacturer.empty() && product.rfind(manufacturer, 0) != 0) {
		id.name = manufacturer + (product.empty() ? "" : " " + product);
	}
	if (id.name.size() > 127) {
		id.name.resize(127);
	}
	std::string serial = utf8(info->serial_number);
	id.uniq = clean_uniq(id.bus == kBusBluetooth ? bluetooth_address(serial) : serial);

	id.report_descriptor.resize(gdp::kMaxHidDescriptor);
	int len = SDL_hid_get_report_descriptor(dev, id.report_descriptor.data(), id.report_descriptor.size());
	if (len <= 0) {
		*why = std::string("can't read its report descriptor: ") + SDL_GetError();
		SDL_hid_close(dev);
		return nullptr;
	}
	id.report_descriptor.resize((size_t)len);
	return std::unique_ptr<RawController>(new RawController(dev, std::move(id)));
}

RawController::RawController(SDL_hid_device *dev, Identity identity)
	: dev_(dev), identity_(std::move(identity)) {}

RawController::~RawController() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	cv_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	if (reader_.joinable()) {
		reader_.join(); // at most kReadTimeoutMs
	}
	SDL_hid_close(dev_);
}

void RawController::start(InputFn on_input, ReplyFn on_reply) {
	on_input_ = std::move(on_input);
	on_reply_ = std::move(on_reply);
	reader_ = std::thread([this] { read_loop(); });
	worker_ = std::thread([this] { work_loop(); });
}

void RawController::read_loop() {
	uint8_t buf[gdp::kMaxHidReport];
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (stopping_) {
				return;
			}
		}
		int n = SDL_hid_read_timeout(dev_, buf, sizeof(buf), kReadTimeoutMs);
		if (n < 0) {
			// Unplugged, most likely: SDL's gamepad-removed event follows
			// and StreamSession tears this down.
			SLOG_INFO("spectre: raw controller \"%s\": read failed (%s), reader stopping",
				identity_.name.c_str(), SDL_GetError());
			return;
		}
		if (n > 0) {
			on_input_(buf, (size_t)n);
		}
	}
}

void RawController::enqueue(Request req) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back(std::move(req));
	}
	cv_.notify_one();
}

void RawController::output(const std::string &data) {
	enqueue(Request{Request::kOutput, 0, 0, kTypeOutput, data});
}

void RawController::get_report(uint32_t request_id, uint32_t report_id, int type) {
	enqueue(Request{Request::kGet, request_id, report_id, type, {}});
}

void RawController::set_report(uint32_t request_id, uint32_t report_id, int type, const std::string &data) {
	enqueue(Request{Request::kSet, request_id, report_id, type, data});
}

void RawController::work_loop() {
	for (;;) {
		Request req;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_) {
				return;
			}
			req = std::move(queue_.front());
			queue_.pop_front();
		}
		perform(req);
	}
}

void RawController::perform(const Request &req) {
	const auto *data = reinterpret_cast<const unsigned char *>(req.data.data());
	switch (req.kind) {
	case Request::kOutput:
		// Report ID first (0 when unnumbered): hidapi's convention too.
		if (!req.data.empty() && SDL_hid_write(dev_, data, req.data.size()) < 0) {
			SLOG_ERROR("spectre: raw controller \"%s\": output report failed: %s", identity_.name.c_str(),
				SDL_GetError());
		}
		return;
	case Request::kGet: {
		std::vector<unsigned char> buf(gdp::kMaxHidReport);
		buf[0] = (unsigned char)req.report_id;
		int n = -1;
		if (req.type == kTypeFeature) {
			n = SDL_hid_get_feature_report(dev_, buf.data(), buf.size());
		} else if (req.type == kTypeInput) {
			n = SDL_hid_get_input_report(dev_, buf.data(), buf.size());
		}
		on_reply_(req.request_id, true, n < 0, buf.data(), n < 0 ? 0 : (size_t)n);
		return;
	}
	case Request::kSet: {
		int n = -1;
		if (!req.data.empty()) {
			if (req.type == kTypeFeature) {
				n = SDL_hid_send_feature_report(dev_, data, req.data.size());
			} else if (req.type == kTypeOutput) {
				n = SDL_hid_write(dev_, data, req.data.size());
			}
		}
		on_reply_(req.request_id, false, n < 0, nullptr, 0);
		return;
	}
	}
}

} // namespace spectre
