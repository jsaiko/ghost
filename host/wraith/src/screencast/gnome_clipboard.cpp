// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/gnome_clipboard.hpp"

#include "gdp/clipboard.hpp"
#include "screencast/sdbus_util.hpp"

#include <systemd/sd-bus.h>

#include "util/log.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace wraith {

namespace {

constexpr const char *kRdService = "org.gnome.Mutter.RemoteDesktop";
constexpr const char *kRdSessionIface = "org.gnome.Mutter.RemoteDesktop.Session";

// Appends an a{sv} with a single 'mime-types' entry holding the whole
// text offer set -- the argument EnableClipboard and SetSelection both
// take to say "we own the selection, and this is what we can serve".
int append_mime_types_options(sd_bus_message *m) {
	int r = sd_bus_message_open_container(m, 'a', "{sv}");
	r = r >= 0 ? sd_bus_message_open_container(m, 'e', "sv") : r;
	r = r >= 0 ? sd_bus_message_append(m, "s", "mime-types") : r;
	r = r >= 0 ? sd_bus_message_open_container(m, 'v', "as") : r;
	r = r >= 0 ? sd_bus_message_open_container(m, 'a', "s") : r;
	for (const std::string &mime : gdp::clipboard_offer_mime_types()) {
		r = r >= 0 ? sd_bus_message_append(m, "s", mime.c_str()) : r;
	}
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // a s
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // v
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // e
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // a {sv}
	return r;
}

// mutter does not put the plain value in each option variant: live
// against mutter 50, `SelectionOwnerChanged` carries
//
//   {'mime-types': <(['text/plain', ...],)>, 'session-is-owner': <false>}
//
// -- the mime list is a variant holding a *1-tuple* of the array, so its
// signature is "(as)" and not "as", while the boolean is plain. So the
// variant is entered with whatever signature it actually declares, and a
// struct inside it is stepped through rather than assumed away.
//
// Returns >0 having entered the variant (and any struct in it, flagged in
// `entered_struct`), 0 without consuming anything if the value is not a
// variant at all.
int enter_option_value(sd_bus_message *m, bool *entered_struct) {
	const char *contents = nullptr;
	char type = 0;
	*entered_struct = false;
	int r = sd_bus_message_peek_type(m, &type, &contents);
	if (r <= 0 || type != SD_BUS_TYPE_VARIANT || !contents) {
		return 0;
	}
	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
	if (r <= 0) {
		return r;
	}
	if (sd_bus_message_peek_type(m, &type, &contents) > 0 && type == SD_BUS_TYPE_STRUCT && contents) {
		*entered_struct = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, contents) > 0;
	}
	return 1;
}

void exit_option_value(sd_bus_message *m, bool entered_struct) {
	if (entered_struct) {
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
}

// Reads SelectionOwnerChanged's a{sv}: the mime types the new owner
// offers, and whether that owner is us. An option whose shape isn't what
// this expects is skipped, never fatal -- mutter is free to add keys, and
// one of them being odd must not cost us the two that matter.
bool read_owner_changed(sd_bus_message *m, std::vector<std::string> *mimes, bool *session_is_owner) {
	if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
		return false;
	}
	while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
		const char *key = nullptr;
		bool handled = false;
		if (sd_bus_message_read(m, "s", &key) > 0 && key) {
			bool in_struct = false;
			if (strcmp(key, "mime-types") == 0 && enter_option_value(m, &in_struct) > 0) {
				if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s") > 0) {
					const char *mime = nullptr;
					while (sd_bus_message_read(m, "s", &mime) > 0) {
						if (mime) {
							mimes->emplace_back(mime);
						}
					}
					sd_bus_message_exit_container(m);
				}
				exit_option_value(m, in_struct);
				handled = true;
			} else if (strcmp(key, "session-is-owner") == 0 && enter_option_value(m, &in_struct) > 0) {
				int owner = 0;
				if (sd_bus_message_read(m, "b", &owner) > 0) {
					*session_is_owner = owner != 0;
				}
				exit_option_value(m, in_struct);
				handled = true;
			}
		}
		if (!handled) {
			// A key we don't act on, or one whose value wasn't the shape
			// above: the value still has to be consumed before the dict
			// entry can be left.
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m); // dict entry
	}
	sd_bus_message_exit_container(m); // array
	return true;
}

} // namespace

struct GnomeClipboard::Impl {
	GnomeClipboard *owner = nullptr;
	sd_bus *bus = nullptr;
	std::string session_path;
	sd_bus_slot *owner_changed_slot = nullptr;
	sd_bus_slot *transfer_slot = nullptr;
	bool clipboard_enabled = false;
	// What SetSelection last offered, i.e. what a SelectionTransfer is
	// answered with.
	std::string own_text;

	// Someone else took the selection: read the best text mime they offer.
	static int on_owner_changed(sd_bus_message *m, void *data, sd_bus_error *) {
		auto *impl = static_cast<Impl *>(data);
		std::vector<std::string> mimes;
		bool session_is_owner = false;
		if (!read_owner_changed(m, &mimes, &session_is_owner)) {
			return 0;
		}
		if (session_is_owner) {
			return 0; // the echo of our own SetSelection
		}
		std::string mime = gdp::select_clipboard_mime(mimes);
		if (mime.empty()) {
			return 0; // an image or a file list: nothing v1 carries
		}
		impl->read_selection(mime);
		return 0;
	}

	// An app in the session is pasting from the selection we own.
	static int on_selection_transfer(sd_bus_message *m, void *data, sd_bus_error *) {
		auto *impl = static_cast<Impl *>(data);
		const char *mime = nullptr;
		uint32_t serial = 0;
		if (sd_bus_message_read(m, "su", &mime, &serial) < 0) {
			return 0;
		}
		impl->serve_transfer(mime ? mime : "", serial);
		return 0;
	}

	void read_selection(const std::string &mime) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		int r = sd_bus_call_method(bus, kRdService, session_path.c_str(), kRdSessionIface, "SelectionRead",
			&error, &reply, "s", mime.c_str());
		if (r < 0) {
			WLOG_ERROR("clipboard: SelectionRead(%s) failed: %s", mime.c_str(), sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return;
		}
		sd_bus_error_free(&error);

		int fd = -1;
		r = sd_bus_message_read(reply, "h", &fd);
		// The fd in `reply` closes with the message, so it is dup'd out
		// before the unref -- the pipe pool owns its copy.
		int dup_fd = r >= 0 ? sdbus_dup_fd(fd) : -1;
		sd_bus_message_unref(reply);
		if (dup_fd < 0) {
			WLOG_ERROR("clipboard: SelectionRead returned no usable fd");
			return;
		}
		owner->read_pipe(dup_fd, mime);
	}

	void serve_transfer(const std::string &mime, uint32_t serial) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		int r = sd_bus_call_method(bus, kRdService, session_path.c_str(), kRdSessionIface, "SelectionWrite",
			&error, &reply, "u", serial);
		if (r < 0) {
			WLOG_ERROR("clipboard: SelectionWrite(%u) failed: %s", serial, sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			write_done(serial, false);
			return;
		}
		sd_bus_error_free(&error);

		int fd = -1;
		r = sd_bus_message_read(reply, "h", &fd);
		int dup_fd = r >= 0 ? sdbus_dup_fd(fd) : -1;
		sd_bus_message_unref(reply);
		if (dup_fd < 0) {
			WLOG_ERROR("clipboard: SelectionWrite(%u) returned no usable fd", serial);
			write_done(serial, false);
			return;
		}

		// Written off the event loop, never inline: mutter cancels a
		// transfer it waits too long for, and a paste target that stops
		// reading must not take the session's event loop down with it.
		owner->write_pipe(dup_fd, clipboard_payload_for_mime(own_text, mime.c_str()), serial);
	}

	void write_done(uint32_t serial, bool ok) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		if (sd_bus_call_method(bus, kRdService, session_path.c_str(), kRdSessionIface, "SelectionWriteDone",
				&error, nullptr, "ub", serial, ok ? 1 : 0) < 0) {
			WLOG_INFO("clipboard: SelectionWriteDone(%u) failed: %s", serial,
				error.message ? error.message : "");
		}
		sd_bus_error_free(&error);
	}
};

GnomeClipboard::GnomeClipboard(struct wl_event_loop *loop, std::unique_ptr<Impl> impl)
	: ClipboardSink(loop), impl_(std::move(impl)) {
	impl_->owner = this;
}

std::unique_ptr<GnomeClipboard> GnomeClipboard::create(struct sd_bus *bus, const std::string &session_path,
	struct wl_event_loop *loop) {
	if (!bus || session_path.empty()) {
		return nullptr;
	}
	auto impl = std::make_unique<Impl>();
	impl->bus = bus;
	impl->session_path = session_path;

	// Subscribed before EnableClipboard: mutter reports the session's
	// current selection owner as soon as the clipboard is enabled, and a
	// missed signal is not redelivered.
	sd_bus_match_signal(bus, &impl->owner_changed_slot, kRdService, session_path.c_str(), kRdSessionIface,
		"SelectionOwnerChanged", Impl::on_owner_changed, impl.get());
	sd_bus_match_signal(bus, &impl->transfer_slot, kRdService, session_path.c_str(), kRdSessionIface,
		"SelectionTransfer", Impl::on_selection_transfer, impl.get());

	// The sink is built before EnableClipboard, not after: that call
	// pumps the bus while it waits, and a SelectionOwnerChanged arriving
	// there reaches back through Impl::owner, which only exists once
	// this is built.
	auto sink = std::unique_ptr<GnomeClipboard>(new GnomeClipboard(loop, std::move(impl)));
	Impl &i = *sink->impl_;

	// EnableClipboard with no mime types: wraith is not claiming the
	// selection yet, it's only asking to be told about it. set_text()
	// claims it later via SetSelection.
	sd_bus_message *m = nullptr;
	int r = sd_bus_message_new_method_call(bus, &m, kRdService, session_path.c_str(), kRdSessionIface,
		"EnableClipboard");
	if (r >= 0) {
		r = sdbus_append_no_options(m);
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	if (r >= 0) {
		r = sd_bus_call(bus, m, 0, &error, nullptr);
	}
	sd_bus_message_unref(m);
	if (r < 0) {
		WLOG_INFO("clipboard: RemoteDesktop.Session.EnableClipboard failed (%s), "
				  "this session runs without clipboard sync",
			sdbus_error_text(error, r));
		sd_bus_error_free(&error);
		return nullptr; // ~GnomeClipboard unrefs the slots
	}
	sd_bus_error_free(&error);
	i.clipboard_enabled = true;

	WLOG_INFO("clipboard: mutter RemoteDesktop clipboard sync up");
	return sink;
}

GnomeClipboard::~GnomeClipboard() {
	close();
}

void GnomeClipboard::close() {
	Impl &i = *impl_;
	if (i.clipboard_enabled && i.bus) {
		sd_bus_call_method(i.bus, kRdService, i.session_path.c_str(), kRdSessionIface, "DisableClipboard",
			nullptr, nullptr, "");
		i.clipboard_enabled = false;
	}
	if (i.owner_changed_slot) {
		sd_bus_slot_unref(i.owner_changed_slot);
		i.owner_changed_slot = nullptr;
	}
	if (i.transfer_slot) {
		sd_bus_slot_unref(i.transfer_slot);
		i.transfer_slot = nullptr;
	}
	i.own_text.clear();
}

void GnomeClipboard::read_pipe(int fd, const std::string &mime) {
	pipes_.read(fd,
		[this, mime](std::string raw) { note_local_text(gdp::decode_clipboard_text(mime, raw)); });
}

void GnomeClipboard::write_pipe(int fd, std::string payload, uint32_t serial) {
	pipes_.write(fd, std::move(payload), [this, serial](bool ok) { impl_->write_done(serial, ok); });
}

void GnomeClipboard::set_text(const std::string &utf8) {
	Impl &i = *impl_;
	if (!i.clipboard_enabled) {
		return;
	}
	i.own_text = utf8;

	sd_bus_message *m = nullptr;
	int r = sd_bus_message_new_method_call(i.bus, &m, kRdService, i.session_path.c_str(), kRdSessionIface,
		"SetSelection");
	if (r >= 0) {
		// The whole offer set: mutter serves each of these to a pasting
		// app (and to Xwayland) by asking us for it via SelectionTransfer.
		r = append_mime_types_options(m);
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	if (r >= 0) {
		r = sd_bus_call(i.bus, m, 0, &error, nullptr);
	}
	sd_bus_message_unref(m);
	if (r < 0) {
		WLOG_ERROR("clipboard: SetSelection failed: %s", sdbus_error_text(error, r));
		sd_bus_error_free(&error);
		return;
	}
	sd_bus_error_free(&error);
	// Record it as the last thing seen too, so a later session's initial
	// push doesn't hand a peer back text that peer just sent. The
	// SelectionOwnerChanged this provokes carries session-is-owner and is
	// swallowed there.
	note_local_text(utf8);
}

} // namespace wraith
