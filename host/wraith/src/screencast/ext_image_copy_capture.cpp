// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/ext_image_copy_capture.hpp"

#include "screencast/wayland_client.hpp"
#include "util/clock.hpp"
#include "util/render_node.hpp"

#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <wayland-client-protocol.h>

#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include "util/log.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace wraith {

namespace {

// Three: one held by the consumer (FrameHold's newest), one in flight at
// the compositor, one free so the next capture can be submitted the
// moment a frame is delivered without waiting for the release.
constexpr int kPoolSize = 3;
constexpr uint32_t kFormat = DRM_FORMAT_XRGB8888;
constexpr int kOpenRoundtrips = 20;

// Cursor image shm formats wraith takes, best first (0 = not taken; note
// WL_SHM_FORMAT_ARGB8888 itself is 0).
// wlroots offers the cursor session a single format, the renderer's
// read-back format; where GL reports no alpha bits (NVIDIA) that is an X
// variant, but the read-back is still GL_RGBA/GL_BGRA, so the X byte
// carries the cursor's alpha all the same.
int cursor_format_rank(uint32_t format) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888: return 4;
	case WL_SHM_FORMAT_ABGR8888: return 3;
	case WL_SHM_FORMAT_XRGB8888: return 2;
	case WL_SHM_FORMAT_XBGR8888: return 1;
	default: return 0;
	}
}

} // namespace

struct ExtImageCopyCapture::Impl {
	Globals g;
	Params params;

	struct ext_image_capture_source_v1 *source = nullptr;
	struct ext_image_copy_capture_session_v1 *session = nullptr;

	// Session constraints (buffer_size .. done).
	uint32_t width = 0, height = 0;
	bool have_size = false;
	bool have_device = false;
	dev_t device{};
	std::vector<uint64_t> dmabuf_modifiers;
	bool dmabuf_format_ok = false;
	bool shm_format_ok = false;
	bool done = false;

	struct Buffer {
		struct wl_buffer *wl = nullptr;
		struct gbm_bo *bo = nullptr;
		DmabufFrame frame{};
		void *shm_data = nullptr;
		size_t shm_size = 0;
		uint32_t shm_stride = 0;
		enum class State { Free, InFlight, Held } state = State::Free;
	};
	std::vector<Buffer> pool;
	bool use_dmabuf = false;
	// Deliver the dmabuf pool's pixels through on_cpu_frame by mapping the
	// BO, rather than handing the dmabuf itself to the consumer: the
	// fallback for a consumer with no dmabuf read-back of its own. See
	// open()'s comment.
	bool map_dmabuf_for_cpu = false;
	int drm_fd = -1;
	struct gbm_device *gbm = nullptr;

	// The one frame in flight, and the buffer it captures into.
	struct ext_image_copy_capture_frame_v1 *frame = nullptr;
	Buffer *frame_buffer = nullptr;
	int64_t frame_pts_us = 0;
	bool frame_has_pts = false;

	// Cursor: the pointer cursor session (position/hotspot/enter/leave)
	// plus its own capture session for the image, one shm buffer. The
	// wl_pointer comes from g.seat's capabilities, whenever they arrive
	// (a headless compositor has none until wraith's virtual pointer
	// exists, which is after open()).
	struct wl_pointer *pointer = nullptr;
	struct ext_image_copy_capture_cursor_session_v1 *cursor_session = nullptr;
	struct ext_image_copy_capture_session_v1 *cursor_capture = nullptr;
	uint32_t cursor_w = 0, cursor_h = 0;
	// The best of cursor_format_rank()'s formats the cursor session
	// offered, valid once cursor_format_ok.
	uint32_t cursor_shm_format = 0;
	bool cursor_format_ok = false;
	bool cursor_format_logged = false;
	int32_t hotspot_x = 0, hotspot_y = 0;
	Buffer cursor_buffer;
	struct ext_image_copy_capture_frame_v1 *cursor_frame = nullptr;
	std::vector<uint8_t> cursor_argb;

	bool closed = false;

	std::function<void(const DmabufFrame &, int64_t, void *, const DamageRegion *)> on_dmabuf_frame_cb;
	std::function<void(const uint8_t *, uint32_t, uint32_t, uint32_t, int64_t, const DamageRegion *, void *)>
		on_cpu_frame_cb;
	// Damage rectangles received for the in-flight output frame (on_damage),
	// handed on with its pixels in on_ready and cleared there.
	DamageRegion frame_damage;
	bool damage_reported = false;
	std::function<void(uint32_t, uint32_t, int32_t, int32_t, const uint8_t *)> on_cursor_shape_cb;
	std::function<void(double, double)> on_cursor_position_cb;
	std::function<void()> on_cursor_hidden_cb;
	std::function<void(const std::string &)> on_closed_cb;

	void report_closed(const std::string &reason) {
		if (closed) {
			return;
		}
		closed = true;
		WLOG_ERROR("screencast: ext capture: %s", reason.c_str());
		if (on_closed_cb) {
			auto cb = on_closed_cb;
			cb(reason);
		}
	}

	// --- session events (shared by the output session and the cursor
	// image session; told apart by the proxy) ---

	static const struct ext_image_copy_capture_session_v1_listener session_listener;

	static void on_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t w,
		uint32_t h) {
		auto *impl = static_cast<Impl *>(data);
		if (s == impl->cursor_capture) {
			impl->cursor_w = w;
			impl->cursor_h = h;
			return;
		}
		if (impl->have_size && !impl->pool.empty() && (w != impl->width || h != impl->height)) {
			// Renegotiating the pool mid-session is not implemented; a
			// virtual output never changes size anyway.
			impl->report_closed("buffer size changed mid-session (not supported)");
			return;
		}
		impl->width = w;
		impl->height = h;
		impl->have_size = true;
	}

	static void on_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format) {
		auto *impl = static_cast<Impl *>(data);
		if (s == impl->cursor_capture) {
			int rank = cursor_format_rank(format);
			if (rank > 0 && (!impl->cursor_format_ok || rank > cursor_format_rank(impl->cursor_shm_format))) {
				impl->cursor_shm_format = format;
				impl->cursor_format_ok = true;
			}
			return;
		}
		if (format == WL_SHM_FORMAT_XRGB8888) {
			impl->shm_format_ok = true;
		}
	}

	static void on_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
		struct wl_array *device) {
		auto *impl = static_cast<Impl *>(data);
		if (s != impl->session || device->size < sizeof(dev_t)) {
			return;
		}
		std::memcpy(&impl->device, device->data, sizeof(dev_t));
		impl->have_device = true;
	}

	static void on_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format,
		struct wl_array *modifiers) {
		auto *impl = static_cast<Impl *>(data);
		if (s != impl->session || format != kFormat) {
			return;
		}
		impl->dmabuf_modifiers.clear();
		size_t n = modifiers->size / sizeof(uint64_t);
		const auto *mods = static_cast<const uint64_t *>(modifiers->data);
		impl->dmabuf_modifiers.assign(mods, mods + n);
		impl->dmabuf_format_ok = true;
	}

	static void on_done(void *data, struct ext_image_copy_capture_session_v1 *s) {
		auto *impl = static_cast<Impl *>(data);
		if (s == impl->cursor_capture) {
			impl->cursor_constraints_done();
			return;
		}
		impl->done = true;
	}

	static void on_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {
		auto *impl = static_cast<Impl *>(data);
		if (s == impl->cursor_capture) {
			return; // cursor image just stops updating; not fatal
		}
		impl->report_closed("compositor stopped the capture session");
	}

	// --- frame events (output frame and cursor frame, told apart by proxy) ---

	static const struct ext_image_copy_capture_frame_v1_listener frame_listener;

	static void on_transform(void *, struct ext_image_copy_capture_frame_v1 *, uint32_t) {}
	// Damage for the output frame, delivered before its ready event (one
	// event per rectangle). Collected for lossless refinement's tile
	// tracker; the cursor frame's damage is not interesting.
	static void on_damage(void *data, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
		int32_t w, int32_t h) {
		auto *impl = static_cast<Impl *>(data);
		if (f != impl->frame) {
			return;
		}
		impl->frame_damage.rects.push_back(DamageRect{x, y, w, h});
	}

	static void on_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t sec_hi,
		uint32_t sec_lo, uint32_t nsec) {
		auto *impl = static_cast<Impl *>(data);
		if (f != impl->frame) {
			return;
		}
		int64_t sec = ((int64_t)sec_hi << 32) | sec_lo;
		impl->frame_pts_us = sec * 1'000'000 + nsec / 1000;
		impl->frame_has_pts = true;
	}

	static void on_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
		auto *impl = static_cast<Impl *>(data);
		if (f == impl->cursor_frame) {
			impl->cursor_ready();
			return;
		}
		if (f != impl->frame) {
			return;
		}
		Buffer *buf = impl->frame_buffer;
		ext_image_copy_capture_frame_v1_destroy(impl->frame);
		impl->frame = nullptr;
		impl->frame_buffer = nullptr;
		int64_t pts_us = impl->frame_has_pts ? impl->frame_pts_us : monotonic_now_us();
		impl->frame_has_pts = false;
		// A frame that came with no damage events at all is treated as
		// unknown rather than unchanged -- the compositor is supposed to
		// send at least one, and "nothing changed" is the one answer the
		// tracker must never be given by mistake.
		DamageRegion frame_damage;
		frame_damage.rects.swap(impl->frame_damage.rects);
		const DamageRegion *damage = frame_damage.rects.empty() ? nullptr : &frame_damage;
		impl->log_damage(damage);
		// Ask for the next frame before handing this one on. Delivering
		// can encode it there and then (NVENC takes ~14 ms at 4K), and a
		// capture requested only after that misses the output's next
		// refresh, halving the frame rate. This buffer is still InFlight,
		// so the request takes another; the call below covers a pool that
		// had none free.
		impl->submit_frame();

		if (impl->use_dmabuf && impl->map_dmabuf_for_cpu) {
			impl->deliver_mapped(buf, pts_us, damage);
			buf->state = Buffer::State::Free;
		} else if (impl->use_dmabuf) {
			buf->state = Buffer::State::Held;
			if (impl->on_dmabuf_frame_cb) {
				impl->on_dmabuf_frame_cb(buf->frame, pts_us, buf, damage);
			}
		} else {
			if (impl->on_cpu_frame_cb) {
				impl->on_cpu_frame_cb(static_cast<const uint8_t *>(buf->shm_data), impl->width, impl->height,
					buf->shm_stride, pts_us, damage, nullptr);
			}
			buf->state = Buffer::State::Free;
		}
		impl->submit_frame();
	}

	static void on_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
		auto *impl = static_cast<Impl *>(data);
		if (f == impl->cursor_frame) {
			ext_image_copy_capture_frame_v1_destroy(impl->cursor_frame);
			impl->cursor_frame = nullptr;
			if (reason != EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
				impl->submit_cursor_frame();
			}
			return;
		}
		if (f != impl->frame) {
			return;
		}
		impl->frame_damage.rects.clear();
		ext_image_copy_capture_frame_v1_destroy(impl->frame);
		impl->frame = nullptr;
		if (impl->frame_buffer) {
			impl->frame_buffer->state = Buffer::State::Free;
			impl->frame_buffer = nullptr;
		}
		impl->frame_has_pts = false;
		switch (reason) {
		case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
			impl->report_closed("capture stopped");
			break;
		case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
			impl->report_closed("buffer constraints changed (renegotiation not supported)");
			break;
		default:
			WLOG_INFO("screencast: ext capture: frame failed (unknown reason), retrying");
			impl->submit_frame();
			break;
		}
	}

	// --- seat: the cursor session follows the pointer capability ---

	static void on_seat_capabilities(void *data, struct wl_seat *, uint32_t caps) {
		auto *impl = static_cast<Impl *>(data);
		bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
		if (has_pointer && !impl->pointer) {
			impl->open_cursor_session();
		} else if (!has_pointer && impl->pointer) {
			impl->close_cursor_session();
			if (impl->on_cursor_hidden_cb) {
				impl->on_cursor_hidden_cb();
			}
		}
	}
	static void on_seat_name(void *, struct wl_seat *, const char *) {}

	void open_cursor_session() {
		if (closed || !source || !g.seat || pointer) {
			return;
		}
		pointer = wl_seat_get_pointer(g.seat);
		cursor_session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(g.capture_manager,
			source, pointer);
		static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
			.enter = on_cursor_enter,
			.leave = on_cursor_leave,
			.position = on_cursor_position,
			.hotspot = on_cursor_hotspot,
		};
		ext_image_copy_capture_cursor_session_v1_add_listener(cursor_session, &cursor_listener, this);
		cursor_capture = ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor_session);
		ext_image_copy_capture_session_v1_add_listener(cursor_capture, &session_listener, this);
		g.client->flush();
		WLOG_INFO("screencast: ext capture: the seat has a pointer, cursor session opened");
	}

	void close_cursor_session() {
		if (cursor_frame) {
			ext_image_copy_capture_frame_v1_destroy(cursor_frame);
			cursor_frame = nullptr;
		}
		if (cursor_capture) {
			ext_image_copy_capture_session_v1_destroy(cursor_capture);
			cursor_capture = nullptr;
		}
		if (cursor_session) {
			ext_image_copy_capture_cursor_session_v1_destroy(cursor_session);
			cursor_session = nullptr;
		}
		if (pointer) {
			wl_pointer_destroy(pointer);
			pointer = nullptr;
		}
		free_buffer(&cursor_buffer);
		cursor_format_ok = false;
		cursor_w = cursor_h = 0;
	}

	// --- cursor session events ---

	// leave hides the cursor, but the compositor only answers the pending
	// cursor capture when the image *changes* -- re-entering with the
	// same shape would leave it hidden, so replay the last one.
	static void on_cursor_enter(void *data, struct ext_image_copy_capture_cursor_session_v1 *) {
		auto *impl = static_cast<Impl *>(data);
		// A buffer_size renegotiation may have changed cursor_w/h since
		// the image was copied; it will be re-sent at the new size.
		bool cached = !impl->cursor_argb.empty() &&
			impl->cursor_argb.size() == (size_t)impl->cursor_w * impl->cursor_h * 4;
		if (cached && impl->on_cursor_shape_cb) {
			impl->on_cursor_shape_cb(impl->cursor_w, impl->cursor_h, impl->hotspot_x, impl->hotspot_y,
				impl->cursor_argb.data());
		}
	}

	static void on_cursor_leave(void *data, struct ext_image_copy_capture_cursor_session_v1 *) {
		auto *impl = static_cast<Impl *>(data);
		if (impl->on_cursor_hidden_cb) {
			impl->on_cursor_hidden_cb();
		}
	}

	static void on_cursor_position(void *data, struct ext_image_copy_capture_cursor_session_v1 *, int32_t x,
		int32_t y) {
		auto *impl = static_cast<Impl *>(data);
		if (impl->have_size && impl->width && impl->height && impl->on_cursor_position_cb) {
			impl->on_cursor_position_cb((double)x / impl->width, (double)y / impl->height);
		}
	}

	static void on_cursor_hotspot(void *data, struct ext_image_copy_capture_cursor_session_v1 *, int32_t x,
		int32_t y) {
		auto *impl = static_cast<Impl *>(data);
		impl->hotspot_x = x;
		impl->hotspot_y = y;
	}

	// --- buffers ---

	bool alloc_dmabuf(Buffer *buf) {
		if (drm_fd < 0) {
			drm_fd = open_render_node(device);
			if (drm_fd < 0) {
				WLOG_ERROR("screencast: ext capture: no DRM render node for the compositor's device");
				return false;
			}
			gbm = gbm_create_device(drm_fd);
			if (!gbm) {
				WLOG_ERROR("screencast: ext capture: gbm_create_device failed");
				return false;
			}
		}
		// Prefer LINEAR. Unlike PipeWire (mutter/kwin hand wraith buffers
		// the compositor made), here wraith picks the modifier, and LINEAR
		// is what the other backends feed the encoder in practice, so
		// compressed modifiers (AMD DCC), untested as encoder input, are
		// never chosen where LINEAR works.
		const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
		bool compositor_allows_linear =
			std::find(dmabuf_modifiers.begin(), dmabuf_modifiers.end(), linear) != dmabuf_modifiers.end();
		if (compositor_allows_linear) {
			buf->bo =
				gbm_bo_create_with_modifiers2(gbm, width, height, kFormat, &linear, 1, GBM_BO_USE_RENDERING);
		}
		if (!buf->bo) {
			buf->bo = gbm_bo_create(gbm, width, height, kFormat, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
		}
		// NVIDIA can't render to LINEAR (its GBM refuses the allocation,
		// and its EGL calls a LINEAR XRGB8888 import external-only), so
		// fall back to a tiled modifier both the compositor and the
		// consumer take. A consumer mapping the pool for the CPU (empty
		// import_modifiers) needs LINEAR, so it gets no fallback.
		if (!buf->bo && !params.import_modifiers.empty()) {
			std::vector<uint64_t> both;
			for (uint64_t m : dmabuf_modifiers) {
				if (m != linear && m != DRM_FORMAT_MOD_INVALID &&
					std::find(params.import_modifiers.begin(), params.import_modifiers.end(), m) !=
						params.import_modifiers.end()) {
					both.push_back(m);
				}
			}
			if (!both.empty()) {
				buf->bo = gbm_bo_create_with_modifiers2(gbm, width, height, kFormat, both.data(),
					(unsigned)both.size(), GBM_BO_USE_RENDERING);
			}
		}
		if (!buf->bo) {
			WLOG_ERROR("screencast: ext capture: gbm_bo_create %ux%u failed", width, height);
			return false;
		}
		DmabufFrame &f = buf->frame;
		f.width = (int32_t)width;
		f.height = (int32_t)height;
		f.format = kFormat;
		f.modifier = gbm_bo_get_modifier(buf->bo);
		f.n_planes = std::min(gbm_bo_get_plane_count(buf->bo), DmabufFrame::kMaxPlanes);
		struct zwp_linux_buffer_params_v1 *bparams = zwp_linux_dmabuf_v1_create_params(g.dmabuf);
		for (int i = 0; i < f.n_planes; i++) {
			f.fd[i] = gbm_bo_get_fd_for_plane(buf->bo, i);
			f.offset[i] = gbm_bo_get_offset(buf->bo, i);
			f.stride[i] = gbm_bo_get_stride_for_plane(buf->bo, i);
			zwp_linux_buffer_params_v1_add(bparams, f.fd[i], (uint32_t)i, f.offset[i], f.stride[i],
				(uint32_t)(f.modifier >> 32), (uint32_t)(f.modifier & 0xffffffffu));
		}
		buf->wl =
			zwp_linux_buffer_params_v1_create_immed(bparams, (int32_t)width, (int32_t)height, kFormat, 0);
		zwp_linux_buffer_params_v1_destroy(bparams);
		return buf->wl != nullptr;
	}

	bool alloc_shm(Buffer *buf, uint32_t w, uint32_t h, uint32_t wl_format) {
		uint32_t stride = w * 4;
		size_t size = (size_t)stride * h;
		int fd = memfd_create("wraith-ext-capture", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
			if (fd >= 0) {
				::close(fd);
			}
			return false;
		}
		buf->shm_data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (buf->shm_data == MAP_FAILED) {
			buf->shm_data = nullptr;
			::close(fd);
			return false;
		}
		buf->shm_size = size;
		buf->shm_stride = stride;
		struct wl_shm_pool *shm_pool = wl_shm_create_pool(g.shm, fd, (int32_t)size);
		buf->wl = wl_shm_pool_create_buffer(shm_pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, wl_format);
		wl_shm_pool_destroy(shm_pool);
		::close(fd);
		return buf->wl != nullptr;
	}

	// The same report the PipeWire capture makes (pipewire_capture.cpp's
	// read_damage_meta): once at INFO whether this compositor sends damage
	// at all, then per frame at DEBUG how much of the frame it covers.
	void log_damage(const DamageRegion *damage) {
		if (!damage_reported) {
			damage_reported = true;
			if (!damage) {
				WLOG_INFO("screencast: ext capture: first frame came with no damage events; "
						  "lossless refinement will settle video tiles on their hash alone");
			} else {
				WLOG_INFO("screencast: ext capture: compositor sends damage, first frame carries %zu rect(s)",
					damage->rects.size());
			}
		}
		if (damage) {
			uint64_t area = 0;
			for (const DamageRect &r : damage->rects) {
				area += (uint64_t)std::max(r.width, 0) * (uint64_t)std::max(r.height, 0);
			}
			uint64_t frame_area = (uint64_t)width * height;
			WLOG_DEBUG("screencast: frame damage %zu rect(s), %.1f%% of the frame", damage->rects.size(),
				frame_area ? 100.0 * (double)area / (double)frame_area : 0.0);
		}
	}

	// Maps the just-captured BO and hands its pixels to on_cpu_frame. The
	// pool is XRGB8888/LINEAR, which is exactly what push_cpu() wants, so
	// this is a map and a callback -- no format conversion. The mapping
	// lasts only for the callback, matching on_cpu_frame's contract that
	// the data is valid for the call and nothing else.
	void deliver_mapped(Buffer *buf, int64_t pts_us, const DamageRegion *damage) {
		if (!on_cpu_frame_cb) {
			return;
		}
		uint32_t map_stride = 0;
		void *map_data = nullptr;
		void *pixels = gbm_bo_map(buf->bo, 0, 0, width, height, GBM_BO_TRANSFER_READ, &map_stride, &map_data);
		if (!pixels || pixels == MAP_FAILED) {
			report_closed("gbm_bo_map of a captured frame failed (cannot feed a CPU-frame encoder)");
			return;
		}
		on_cpu_frame_cb(static_cast<const uint8_t *>(pixels), width, height, map_stride, pts_us, damage,
			nullptr);
		gbm_bo_unmap(buf->bo, map_data);
	}

	void free_buffer(Buffer *buf) {
		if (buf->wl) {
			wl_buffer_destroy(buf->wl);
			buf->wl = nullptr;
		}
		for (int i = 0; i < buf->frame.n_planes; i++) {
			if (buf->frame.fd[i] >= 0) {
				::close(buf->frame.fd[i]);
			}
			buf->frame.fd[i] = -1;
		}
		buf->frame.n_planes = 0;
		if (buf->bo) {
			gbm_bo_destroy(buf->bo);
			buf->bo = nullptr;
		}
		if (buf->shm_data) {
			munmap(buf->shm_data, buf->shm_size);
			buf->shm_data = nullptr;
		}
		buf->state = Buffer::State::Free;
	}

	// --- the pull loop ---

	void submit_frame() {
		if (frame || closed || !session) {
			return;
		}
		Buffer *buf = nullptr;
		for (Buffer &b : pool) {
			if (b.state == Buffer::State::Free) {
				buf = &b;
				break;
			}
		}
		if (!buf) {
			return; // release() will call back here
		}
		frame = ext_image_copy_capture_session_v1_create_frame(session);
		ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, this);
		ext_image_copy_capture_frame_v1_attach_buffer(frame, buf->wl);
		ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)width, (int32_t)height);
		ext_image_copy_capture_frame_v1_capture(frame);
		buf->state = Buffer::State::InFlight;
		frame_buffer = buf;
		g.client->flush();
	}

	void cursor_constraints_done() {
		if (!cursor_format_ok || cursor_w == 0 || cursor_h == 0 || !g.shm) {
			if (!cursor_format_logged) {
				cursor_format_logged = true;
				WLOG_ERROR(
					"screencast: ext capture: no cursor image (shm %s, a 32-bit RGB format %s, size %ux%u)",
					g.shm ? "bound" : "absent", cursor_format_ok ? "offered" : "not offered", cursor_w,
					cursor_h);
			}
			return;
		}
		if (cursor_buffer.wl &&
			(cursor_buffer.frame.width != (int32_t)cursor_w ||
				cursor_buffer.frame.height != (int32_t)cursor_h)) {
			if (cursor_frame) {
				ext_image_copy_capture_frame_v1_destroy(cursor_frame);
				cursor_frame = nullptr;
			}
			free_buffer(&cursor_buffer);
		}
		if (!cursor_buffer.wl) {
			if (!alloc_shm(&cursor_buffer, cursor_w, cursor_h, cursor_shm_format)) {
				return;
			}
			cursor_buffer.frame.width = (int32_t)cursor_w; // size bookkeeping only
			cursor_buffer.frame.height = (int32_t)cursor_h;
		}
		submit_cursor_frame();
	}

	void submit_cursor_frame() {
		if (cursor_frame || closed || !cursor_capture || !cursor_buffer.wl) {
			return;
		}
		cursor_frame = ext_image_copy_capture_session_v1_create_frame(cursor_capture);
		ext_image_copy_capture_frame_v1_add_listener(cursor_frame, &frame_listener, this);
		ext_image_copy_capture_frame_v1_attach_buffer(cursor_frame, cursor_buffer.wl);
		ext_image_copy_capture_frame_v1_damage_buffer(cursor_frame, 0, 0, (int32_t)cursor_w,
			(int32_t)cursor_h);
		ext_image_copy_capture_frame_v1_capture(cursor_frame);
		g.client->flush();
	}

	void cursor_ready() {
		ext_image_copy_capture_frame_v1_destroy(cursor_frame);
		cursor_frame = nullptr;
		// wl_shm ARGB8888 is B,G,R,A premultiplied in memory -- exactly
		// wraith's CursorShape convention, so this is a straight copy.
		// XRGB8888 is the same layout (see cursor_format_rank() for its X
		// byte); ABGR8888 and XBGR8888 are R,G,B,A and need red and blue
		// swapped.
		const bool swap_rb =
			cursor_shm_format == WL_SHM_FORMAT_ABGR8888 || cursor_shm_format == WL_SHM_FORMAT_XBGR8888;
		const auto *src = static_cast<const uint8_t *>(cursor_buffer.shm_data);
		cursor_argb.resize((size_t)cursor_w * cursor_h * 4);
		for (uint32_t y = 0; y < cursor_h; y++) {
			uint8_t *row = cursor_argb.data() + (size_t)y * cursor_w * 4;
			std::memcpy(row, src + (size_t)y * cursor_buffer.shm_stride, (size_t)cursor_w * 4);
			if (swap_rb) {
				for (uint32_t x = 0; x < cursor_w; x++) {
					std::swap(row[x * 4], row[x * 4 + 2]);
				}
			}
		}
		if (on_cursor_shape_cb) {
			on_cursor_shape_cb(cursor_w, cursor_h, hotspot_x, hotspot_y, cursor_argb.data());
		}
		// The compositor answers the next capture when the image changes.
		submit_cursor_frame();
	}
};

const struct ext_image_copy_capture_frame_v1_listener ExtImageCopyCapture::Impl::frame_listener = {
	.transform = ExtImageCopyCapture::Impl::on_transform,
	.damage = ExtImageCopyCapture::Impl::on_damage,
	.presentation_time = ExtImageCopyCapture::Impl::on_presentation_time,
	.ready = ExtImageCopyCapture::Impl::on_ready,
	.failed = ExtImageCopyCapture::Impl::on_failed,
};

const struct ext_image_copy_capture_session_v1_listener ExtImageCopyCapture::Impl::session_listener = {
	.buffer_size = ExtImageCopyCapture::Impl::on_buffer_size,
	.shm_format = ExtImageCopyCapture::Impl::on_shm_format,
	.dmabuf_device = ExtImageCopyCapture::Impl::on_dmabuf_device,
	.dmabuf_format = ExtImageCopyCapture::Impl::on_dmabuf_format,
	.done = ExtImageCopyCapture::Impl::on_done,
	.stopped = ExtImageCopyCapture::Impl::on_stopped,
};

ExtImageCopyCapture::ExtImageCopyCapture(const Globals &globals) : impl_(std::make_unique<Impl>()) {
	impl_->g = globals;
}

ExtImageCopyCapture::~ExtImageCopyCapture() {
	close();
}

bool ExtImageCopyCapture::open(const Params &params) {
	impl_->params = params;
	impl_->on_dmabuf_frame_cb = on_dmabuf_frame;
	impl_->on_cpu_frame_cb = on_cpu_frame;
	impl_->on_cursor_shape_cb = on_cursor_shape;
	impl_->on_cursor_position_cb = on_cursor_position;
	impl_->on_cursor_hidden_cb = on_cursor_hidden;
	impl_->on_closed_cb = on_closed;
	impl_->closed = false;

	if (!impl_->g.client || !impl_->g.output || !impl_->g.source_manager || !impl_->g.capture_manager) {
		WLOG_ERROR("screencast: ext capture: missing globals");
		return false;
	}

	impl_->source =
		ext_output_image_capture_source_manager_v1_create_source(impl_->g.source_manager, impl_->g.output);
	impl_->session =
		ext_image_copy_capture_manager_v1_create_session(impl_->g.capture_manager, impl_->source, 0);
	ext_image_copy_capture_session_v1_add_listener(impl_->session, &Impl::session_listener, impl_.get());

	// The cursor session is opened from the seat's capabilities event,
	// now (in the roundtrips below) if the seat already has a pointer, or
	// later when one appears.
	for (int &fd : impl_->cursor_buffer.frame.fd) {
		fd = -1;
	}
	if (impl_->g.seat) {
		static const struct wl_seat_listener seat_listener = {
			.capabilities = Impl::on_seat_capabilities,
			.name = Impl::on_seat_name,
		};
		wl_seat_add_listener(impl_->g.seat, &seat_listener, impl_.get());
	}

	for (int i = 0; i < kOpenRoundtrips && !impl_->done && !impl_->closed; i++) {
		if (!impl_->g.client->roundtrip()) {
			break;
		}
	}
	if (!impl_->done || !impl_->have_size) {
		WLOG_ERROR("screencast: ext capture: session constraints never arrived");
		close();
		return false;
	}

	impl_->use_dmabuf = impl_->g.dmabuf && impl_->have_device && impl_->dmabuf_format_ok;
	// Empty import_modifiers means the consumer wants pixels in host
	// memory and has no way to read a dmabuf back itself (ScreencastHost
	// offers its DmabufReader's modifiers when it has one, and then gets
	// dmabufs here like a zero-copy encoder would, damage included).
	//
	// That does *not* mean falling back to shm capture: wlroots advertises
	// its shm format as whatever the renderer reads back most naturally,
	// which with the GLES2 renderer is typically XBGR8888, while the dmabuf
	// format is the XRGB8888 push_cpu() wants. Capturing into the dmabuf
	// pool and mapping it therefore yields the right format with no
	// channel swizzle, and keeps one capture path for both kinds of
	// encoder.
	impl_->map_dmabuf_for_cpu = impl_->use_dmabuf && impl_->params.import_modifiers.empty();
	if (!impl_->use_dmabuf && !(impl_->g.shm && impl_->shm_format_ok)) {
		WLOG_ERROR("screencast: ext capture: no usable format (dmabuf: %s, device %s, XRGB8888 %s; "
				   "shm: %s, XRGB8888 %s)",
			impl_->g.dmabuf ? "bound" : "absent", impl_->have_device ? "known" : "unknown",
			impl_->dmabuf_format_ok ? "offered" : "not offered", impl_->g.shm ? "bound" : "absent",
			impl_->shm_format_ok ? "offered" : "not offered");
		close();
		return false;
	}
	impl_->pool.resize(kPoolSize);
	for (Impl::Buffer &buf : impl_->pool) {
		for (int &fd : buf.frame.fd) {
			fd = -1;
		}
		bool ok = impl_->use_dmabuf
			? impl_->alloc_dmabuf(&buf)
			: impl_->alloc_shm(&buf, impl_->width, impl_->height, WL_SHM_FORMAT_XRGB8888);
		if (!ok) {
			close();
			return false;
		}
	}
	WLOG_INFO("screencast: ext capture: %ux%u %s, pool of %d (modifier 0x%016llx)", impl_->width,
		impl_->height,
		impl_->map_dmabuf_for_cpu ? "dmabuf mapped to cpu" : (impl_->use_dmabuf ? "dmabuf" : "shm/cpu"),
		kPoolSize, (unsigned long long)(impl_->pool.empty() ? 0 : impl_->pool[0].frame.modifier));

	impl_->submit_frame();
	return true;
}

void ExtImageCopyCapture::close() {
	Impl &i = *impl_;
	if (i.frame) {
		ext_image_copy_capture_frame_v1_destroy(i.frame);
		i.frame = nullptr;
		i.frame_buffer = nullptr;
	}
	i.close_cursor_session();
	if (i.g.seat) {
		wl_seat_destroy(i.g.seat);
		i.g.seat = nullptr;
	}
	for (Impl::Buffer &buf : i.pool) {
		i.free_buffer(&buf);
	}
	i.pool.clear();
	if (i.session) {
		ext_image_copy_capture_session_v1_destroy(i.session);
		i.session = nullptr;
	}
	if (i.source) {
		ext_image_capture_source_v1_destroy(i.source);
		i.source = nullptr;
	}
	if (i.gbm) {
		gbm_device_destroy(i.gbm);
		i.gbm = nullptr;
	}
	if (i.drm_fd >= 0) {
		::close(i.drm_fd);
		i.drm_fd = -1;
	}
	if (i.g.client) {
		i.g.client->flush();
	}
	i.done = false;
	i.have_size = false;
}

void ExtImageCopyCapture::release(void *release_token) {
	auto *buf = static_cast<Impl::Buffer *>(release_token);
	for (Impl::Buffer &b : impl_->pool) {
		if (&b == buf && b.state == Impl::Buffer::State::Held) {
			b.state = Impl::Buffer::State::Free;
			impl_->submit_frame();
			return;
		}
	}
}

} // namespace wraith
