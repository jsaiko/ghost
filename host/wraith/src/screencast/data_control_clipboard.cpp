// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/data_control_clipboard.hpp"

#include "screencast/wayland_client.hpp"

#include "gdp/clipboard.hpp"

#include <ext-data-control-v1-client-protocol.h>
#include <wayland-client-protocol.h>

#include "util/log.hpp"

#include <map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace wraith {

struct DataControlClipboard::Impl {
	DataControlClipboard *owner = nullptr;
	WaylandClient *client = nullptr;
	struct wl_seat *seat = nullptr;
	struct ext_data_control_manager_v1 *manager = nullptr;
	struct ext_data_control_device_v1 *device = nullptr;
	struct ext_data_control_source_v1 *source = nullptr;
	// What `source` serves. Also what an incoming selection is compared
	// against: the compositor hands our own source's offer straight back
	// to us, and that read must not look like a fresh local copy.
	std::string own_text;
	// Mime types each live offer advertised, keyed by the offer proxy.
	// An offer's `offer` events all arrive before the selection event
	// that hands it over.
	std::map<struct ext_data_control_offer_v1 *, std::vector<std::string>> offers;

	// --- ext_data_control_offer_v1 ---

	static void on_offer_mime(void *data, struct ext_data_control_offer_v1 *offer, const char *mime_type) {
		auto *impl = static_cast<Impl *>(data);
		if (mime_type) {
			impl->offers[offer].emplace_back(mime_type);
		}
	}

	// --- ext_data_control_device_v1 ---

	static void on_data_offer(void *data, struct ext_data_control_device_v1 *,
		struct ext_data_control_offer_v1 *offer) {
		auto *impl = static_cast<Impl *>(data);
		static const struct ext_data_control_offer_v1_listener offer_listener = {
			.offer = Impl::on_offer_mime,
		};
		impl->offers[offer]; // a still-empty mime list, so the map owns it
		ext_data_control_offer_v1_add_listener(offer, &offer_listener, impl);
	}

	static void on_selection(void *data, struct ext_data_control_device_v1 *,
		struct ext_data_control_offer_v1 *offer) {
		auto *impl = static_cast<Impl *>(data);
		impl->take_selection(offer);
	}

	static void on_primary_selection(void *data, struct ext_data_control_device_v1 *,
		struct ext_data_control_offer_v1 *offer) {
		// Out of scope for v1 (gdp-spec.md §7.9): only
		// CLIPBOARD is synced. The offer is still ours to destroy.
		auto *impl = static_cast<Impl *>(data);
		impl->drop_offer(offer);
	}

	static void on_finished(void *data, struct ext_data_control_device_v1 *) {
		// The compositor invalidated the device (its manager went away).
		// Nothing to recover to: the whole RemoteSession is torn down and
		// reopened when that happens.
		auto *impl = static_cast<Impl *>(data);
		WLOG_INFO("clipboard: the compositor finished our data-control device");
		impl->device = nullptr;
	}

	// --- ext_data_control_source_v1 ---

	static void on_source_send(void *data, struct ext_data_control_source_v1 *source, const char *mime_type,
		int32_t fd) {
		auto *impl = static_cast<Impl *>(data);
		if (source != impl->source) {
			::close(fd);
			return;
		}
		impl->owner->write_pipe(fd, clipboard_payload_for_mime(impl->own_text, mime_type));
	}

	static void on_source_cancelled(void *data, struct ext_data_control_source_v1 *source) {
		auto *impl = static_cast<Impl *>(data);
		if (source != impl->source) {
			ext_data_control_source_v1_destroy(source);
			return;
		}
		// Another client took the selection from us.
		ext_data_control_source_v1_destroy(impl->source);
		impl->source = nullptr;
		impl->own_text.clear();
	}

	void drop_offer(struct ext_data_control_offer_v1 *offer) {
		if (!offer) {
			return;
		}
		offers.erase(offer);
		ext_data_control_offer_v1_destroy(offer);
	}

	void take_selection(struct ext_data_control_offer_v1 *offer) {
		if (!offer) {
			// The selection was cleared. Not forwarded: the wire has no
			// "clear" (gdp-spec.md §7.9), and the peer's clipboard still
			// holds whatever its own user last copied.
			return;
		}
		auto it = offers.find(offer);
		std::string mime = it == offers.end() ? std::string() : gdp::select_clipboard_mime(it->second);
		if (mime.empty()) {
			drop_offer(offer); // an image or a file list: nothing v1 carries
			return;
		}

		int fds[2];
		if (pipe2(fds, O_CLOEXEC) != 0) {
			WLOG_ERROR("clipboard: pipe2 failed while reading the compositor selection");
			drop_offer(offer);
			return;
		}
		ext_data_control_offer_v1_receive(offer, mime.c_str(), fds[1]);
		// The compositor dups the fd out of the message; ours goes now so
		// the read end sees EOF when the sender is done. The request has
		// to reach the compositor before anything will: the client's fd
		// source only ever reads.
		::close(fds[1]);
		client->flush();
		drop_offer(offer);
		owner->read_pipe(fds[0], mime);
	}
};

DataControlClipboard::DataControlClipboard(struct wl_event_loop *loop, std::unique_ptr<Impl> impl)
	: ClipboardSink(loop), impl_(std::move(impl)) {
	impl_->owner = this;
}

std::unique_ptr<DataControlClipboard> DataControlClipboard::create(WaylandClient &client,
	struct wl_event_loop *loop) {
	if (!client.has_global(ext_data_control_manager_v1_interface.name)) {
		WLOG_INFO("clipboard: the compositor exports no ext_data_control_manager_v1, "
				  "this session runs without clipboard sync");
		return nullptr;
	}
	auto impl = std::make_unique<Impl>();
	impl->client = &client;
	impl->manager = static_cast<struct ext_data_control_manager_v1 *>(
		client.bind(&ext_data_control_manager_v1_interface, 1));
	// A seat proxy of this sink's own, like the frame source's: sharing
	// the session's would mean sharing its listener.
	impl->seat = static_cast<struct wl_seat *>(client.bind(&wl_seat_interface, 5));
	if (!impl->manager || !impl->seat) {
		WLOG_ERROR("clipboard: binding ext_data_control_manager_v1/wl_seat failed");
		return nullptr;
	}
	impl->device = ext_data_control_manager_v1_get_data_device(impl->manager, impl->seat);

	static const struct ext_data_control_device_v1_listener device_listener = {
		.data_offer = Impl::on_data_offer,
		.selection = Impl::on_selection,
		.finished = Impl::on_finished,
		.primary_selection = Impl::on_primary_selection,
	};
	ext_data_control_device_v1_add_listener(impl->device, &device_listener, impl.get());

	// Constructed *before* the round trip below: that trip dispatches, and
	// the selection event it is there to collect reaches back through
	// Impl::owner, which only exists once this is built.
	auto sink = std::unique_ptr<DataControlClipboard>(new DataControlClipboard(loop, std::move(impl)));
	// The compositor sends the current selection right after the device is
	// created, so this leaves current_text() already populated for the push
	// the first authenticated session makes.
	client.roundtrip();
	WLOG_INFO("clipboard: ext-data-control-v1 clipboard sync up");
	return sink;
}

DataControlClipboard::~DataControlClipboard() {
	close();
}

void DataControlClipboard::close() {
	Impl &i = *impl_;
	for (auto &entry : i.offers) {
		ext_data_control_offer_v1_destroy(entry.first);
	}
	i.offers.clear();
	if (i.source) {
		// Destroying the source drops the selection with it, rather than
		// leaving the compositor offering data nothing will answer.
		ext_data_control_source_v1_destroy(i.source);
		i.source = nullptr;
	}
	if (i.device) {
		ext_data_control_device_v1_destroy(i.device);
		i.device = nullptr;
	}
	if (i.manager) {
		ext_data_control_manager_v1_destroy(i.manager);
		i.manager = nullptr;
	}
	if (i.seat) {
		wl_seat_destroy(i.seat);
		i.seat = nullptr;
	}
	if (i.client) {
		i.client->flush();
	}
	i.own_text.clear();
}

void DataControlClipboard::read_pipe(int fd, const std::string &mime) {
	pipes_.read(fd, [this, mime](std::string raw) {
		std::string text = gdp::decode_clipboard_text(mime, raw);
		if (text == impl_->own_text) {
			// The compositor hands our own source's offer back to us like
			// any other client's; that is our set_text(), not a copy.
			return;
		}
		note_local_text(std::move(text));
	});
}

void DataControlClipboard::write_pipe(int fd, std::string payload) {
	pipes_.write(fd, std::move(payload));
}

void DataControlClipboard::set_text(const std::string &utf8) {
	Impl &i = *impl_;
	if (!i.manager || !i.device) {
		return;
	}
	i.own_text = utf8;

	struct ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source(i.manager);
	if (!source) {
		return;
	}
	static const struct ext_data_control_source_v1_listener source_listener = {
		.send = Impl::on_source_send,
		.cancelled = Impl::on_source_cancelled,
	};
	ext_data_control_source_v1_add_listener(source, &source_listener, &i);
	// The whole offer set, not just text/plain: pastes into older
	// GTK/X11/Xwayland apps silently fail otherwise (gdp/clipboard.hpp).
	for (const std::string &mime : gdp::clipboard_offer_mime_types()) {
		ext_data_control_source_v1_offer(source, mime.c_str());
	}

	struct ext_data_control_source_v1 *previous = i.source;
	i.source = source;
	// set_selection takes effect immediately, with no focus requirement --
	// which is the whole reason this uses data-control rather than the
	// core wl_data_device protocol: a headless wraith has no surface to
	// focus.
	ext_data_control_device_v1_set_selection(i.device, source);
	if (previous) {
		// Destroyed after the replacement is in place: doing it first
		// would clear the selection in between.
		ext_data_control_source_v1_destroy(previous);
	}
	i.client->flush();
	// Record it as the last thing seen too, so a later session's initial
	// push doesn't hand a peer back text that peer just sent.
	note_local_text(utf8);
}

} // namespace wraith
