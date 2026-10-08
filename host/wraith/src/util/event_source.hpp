// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// RAII owner of a wl_event_source (an fd or timer registered with the
// compositor's wl_event_loop): wl_event_source_remove() on destruction or
// reset(), so nothing has to remember to unregister on every teardown path.
#pragma once

#include <wayland-server-core.h>

#include <functional>
#include <utility>

namespace wraith {

class EventSource {
public:
	EventSource() = default;
	explicit EventSource(struct wl_event_source *source) : source_(source) {}
	~EventSource() { reset(); }

	EventSource(const EventSource &) = delete;
	EventSource &operator=(const EventSource &) = delete;

	EventSource(EventSource &&other) noexcept : source_(other.source_) { other.source_ = nullptr; }
	EventSource &operator=(EventSource &&other) noexcept {
		if (this != &other) {
			reset(other.source_);
			other.source_ = nullptr;
		}
		return *this;
	}

	void reset(struct wl_event_source *source = nullptr) {
		if (source_) {
			wl_event_source_remove(source_);
		}
		source_ = source;
	}

	struct wl_event_source *get() const { return source_; }
	explicit operator bool() const { return source_ != nullptr; }

private:
	struct wl_event_source *source_ = nullptr;
};

// One deferred callback per event-loop iteration: schedule() arms an idle
// source the first time and is a no-op until it has fired, so a burst of
// calls inside one dispatch (a run of injected pointer motions, say) costs
// one callback. cancel() and the destructor take a pending one off the
// loop.
class IdleCoalescer {
public:
	IdleCoalescer() = default;
	~IdleCoalescer() { cancel(); }

	IdleCoalescer(const IdleCoalescer &) = delete;
	IdleCoalescer &operator=(const IdleCoalescer &) = delete;

	void schedule(struct wl_event_loop *loop, std::function<void()> callback) {
		if (source_) {
			return;
		}
		callback_ = std::move(callback);
		source_ = wl_event_loop_add_idle(
			loop,
			[](void *data) {
				auto *self = static_cast<IdleCoalescer *>(data);
				// The idle source removes itself after firing.
				self->source_ = nullptr;
				std::function<void()> callback = std::move(self->callback_);
				callback();
			},
			this);
	}

	void cancel() {
		if (source_) {
			wl_event_source_remove(source_);
			source_ = nullptr;
		}
		callback_ = nullptr;
	}

	bool pending() const { return source_ != nullptr; }

private:
	struct wl_event_source *source_ = nullptr;
	std::function<void()> callback_;
};

} // namespace wraith
