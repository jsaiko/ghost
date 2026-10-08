// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/pipewire_capture.hpp"
#include "gdp/framing.hpp"
#include "util/clock.hpp"
#include "util/pipewire_init.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/utils/result.h>

// wraith's event loop is a libwayland-server wl_event_loop; the PipeWire
// loop's fd is dispatched from it.
#include <wayland-server-core.h>

#include "util/log.hpp"

#include <libdrm/drm_fourcc.h>

#include <cinttypes>
#include <cstring>
#include <unordered_set>

namespace wraith {

namespace {

// va_fourcc_from_drm's lesson (vaapi_encoder_base.cpp) applies here too: a SPA
// video format name follows GStreamer's *memory byte order* convention,
// not DRM's bit-order-derived one, so "RGBA" and DRM_FORMAT_ARGB8888 (which
// is B,G,R,A in little-endian memory) are not the same layout despite the
// similar name. Both kwin (addCursorMetadata) and mutter
// (meta_screen_cast_stream_src_set_cursor_sprite_metadata, which reads the
// sprite back as COGL_PIXEL_FORMAT_RGBA_8888_PRE) tag the bitmap
// SPA_VIDEO_FORMAT_RGBA with premultiplied alpha, so a straight R<->B swap
// is the whole conversion into wraith's GDP CursorShape convention
// (DRM_FORMAT_ARGB8888, B,G,R,A, premultiplied).
void rgba_to_argb8888_premultiplied(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride,
	std::vector<uint8_t> *out) {
	out->resize((size_t)width * height * 4);
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *src_row = rgba + (size_t)y * stride;
		uint8_t *dst_row = out->data() + (size_t)y * width * 4;
		for (uint32_t x = 0; x < width; x++) {
			uint8_t r = src_row[x * 4 + 0];
			uint8_t g = src_row[x * 4 + 1];
			uint8_t b = src_row[x * 4 + 2];
			uint8_t a = src_row[x * 4 + 3];
			dst_row[x * 4 + 0] = b;
			dst_row[x * 4 + 1] = g;
			dst_row[x * 4 + 2] = r;
			dst_row[x * 4 + 3] = a;
		}
	}
}

} // namespace

struct PipeWireCapture::Impl {
	Params config;
	uint32_t node_id = 0;

	struct pw_loop *pw_loop_ = nullptr;
	struct pw_context *pw_context_ = nullptr;
	struct pw_core *pw_core_ = nullptr;
	struct pw_stream *pw_stream_ = nullptr;
	struct spa_hook stream_listener{};
	struct wl_event_source *pw_source = nullptr;

	// Negotiated once the format param settles: is_dmabuf tells
	// on_process which of the two frame callbacks to use.
	bool is_dmabuf = false;
	uint64_t neg_modifier = DRM_FORMAT_MOD_INVALID;
	uint32_t neg_width = 0, neg_height = 0;

	// Reused across calls so a static cursor doesn't reallocate every
	// frame; only touched from on_process on the event-loop thread.
	std::vector<uint8_t> cursor_argb;
	uint32_t cursor_width = 0, cursor_height = 0;
	// Set by an id-0 meta (the pointer left the stream). kwin sends no
	// bitmap when it comes back -- its cache only invalidates on a shape
	// change -- so the cached shape above has to be replayed then.
	bool cursor_off_stream = false;
	// Likewise for read_damage_meta()'s result.
	DamageRegion damage_scratch;
	bool damage_meta_reported = false;

	bool closed = false;

	// Every pw_buffer PipeWire currently has allocated on this stream
	// (add_buffer .. remove_buffer). Only these are valid release()
	// tokens: PipeWire frees a buffer the moment it withdraws it, and it
	// withdraws all of them when the producer disappears -- which on a
	// desktop logout happens while ScreencastHost is still holding one.
	std::unordered_set<struct pw_buffer *> live_buffers;

	static void on_pw_add_buffer(void *data, struct pw_buffer *b) {
		static_cast<Impl *>(data)->live_buffers.insert(b);
	}

	static void on_pw_remove_buffer(void *data, struct pw_buffer *b) {
		auto *impl = static_cast<Impl *>(data);
		impl->live_buffers.erase(b);
		WLOG_DEBUG("screencast: pipewire withdrew buffer %p", (void *)b);
		if (impl->on_buffer_removed_cb) {
			impl->on_buffer_removed_cb(b);
		}
	}

	static void on_pw_state_changed(void *data, enum pw_stream_state, enum pw_stream_state state,
		const char *error) {
		if (state == PW_STREAM_STATE_ERROR) {
			static_cast<Impl *>(data)->report_closed(error ? error : "pipewire stream error");
		}
	}

	static void on_pw_param_changed(void *data, uint32_t id, const struct spa_pod *format) {
		if (!format) {
			return;
		}
		auto *impl = static_cast<Impl *>(data);
		if (id == SPA_PARAM_Buffers) {
			// How many buffers mutter's pool actually ended up with --
			// the ceiling on how many wraith may hold (FrameHold) before
			// mutter's stream starts dropping frames for want of a free
			// one.
			int32_t n_buffers = 0;
			if (spa_pod_parse_object(format, SPA_TYPE_OBJECT_ParamBuffers, nullptr, SPA_PARAM_BUFFERS_buffers,
					SPA_POD_Int(&n_buffers)) >= 0) {
				WLOG_INFO("screencast: pool of %d buffers", n_buffers);
			}
			return;
		}
		if (id != SPA_PARAM_Format) {
			return;
		}
		struct spa_video_info_raw info{};
		if (spa_format_video_raw_parse(format, &info) < 0) {
			return;
		}
		impl->neg_width = info.size.width;
		impl->neg_height = info.size.height;
		impl->is_dmabuf = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
		impl->neg_modifier = impl->is_dmabuf ? info.modifier : DRM_FORMAT_MOD_INVALID;
		WLOG_INFO("screencast: negotiated %ux%u %s (modifier 0x%016" PRIx64 ")", impl->neg_width,
			impl->neg_height, impl->is_dmabuf ? "dmabuf" : "memfd/cpu", impl->neg_modifier);
	}

	static void on_pw_process(void *data) { static_cast<Impl *>(data)->process(); }

	// Takes everything the producer has queued, not one buffer per call:
	// its pool is small (2-4; KWin asks for 3, and wraith keeps one held
	// as the last frame), and while buffers wait here it has none to
	// draw into. KWin then drops the frame it was about to send -- its
	// record() returns on a failed dequeue with the pending repaint
	// already cleared, and nothing schedules it again -- so whatever
	// changed (a popup menu opening) stays off the stream until the next
	// repaint, typically the pointer hovering it. Every cursor-only
	// update goes straight back after its cursor meta is read; of the
	// video frames only the newest is delivered, the older ones' damage
	// carried into it.
	void process() {
		struct pw_buffer *newest = nullptr;
		DamageRegion superseded;
		bool superseded_unknown = false;
		for (;;) {
			struct pw_buffer *b = pw_stream_dequeue_buffer(pw_stream_);
			if (!b) {
				break;
			}
			struct spa_buffer *buf = b->buffer;
			handle_cursor_meta(buf);

			// With cursor mode `metadata`, mutter and KWin send a frame
			// for every cursor move even when nothing on screen changed.
			// For those cursor-only frames they do not re-render the
			// pixels at all -- they only rewrite SPA_META_Cursor and flag
			// the data chunk as invalid (SPA_CHUNK_FLAG_CORRUPTED), so the
			// buffer still holds whatever this pool slot was last rendered
			// into, several frames ago. Encoding that would replay a stale
			// frame; the cursor meta above is all such a frame carries,
			// hand the buffer straight back.
			bool pixels_valid = buf->n_datas > 0 &&
				!(buf->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) && buf->datas[0].chunk->size > 0;
			if (!pixels_valid) {
				WLOG_DEBUG("screencast: cursor-only frame (chunk flags 0x%x size %u), pixels skipped",
					buf->n_datas > 0 ? buf->datas[0].chunk->flags : 0u,
					buf->n_datas > 0 ? buf->datas[0].chunk->size : 0u);
				pw_stream_queue_buffer(pw_stream_, b);
				continue;
			}
			if (newest) {
				const DamageRegion *damage = read_damage_meta(newest->buffer);
				if (!damage) {
					superseded_unknown = true;
				} else {
					superseded.rects.insert(superseded.rects.end(), damage->rects.begin(),
						damage->rects.end());
				}
				WLOG_DEBUG("screencast: video frame in buffer %p superseded before delivery", (void *)newest);
				pw_stream_queue_buffer(pw_stream_, newest);
			}
			newest = b;
		}
		if (newest) {
			deliver(newest, superseded_unknown ? nullptr : &superseded);
		}
	}

	// `superseded`: the damage of video frames dropped in favour of this
	// one; null if any of theirs was unknown.
	void deliver(struct pw_buffer *b, const DamageRegion *superseded) {
		struct spa_buffer *buf = b->buffer;
		int64_t pts_us = monotonic_now_us();
		auto *header = static_cast<struct spa_meta_header *>(
			spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(struct spa_meta_header)));
		if (header && header->pts > 0) {
			pts_us = header->pts / 1000;
		}
		const DamageRegion *damage = read_damage_meta(buf);
		if (!superseded) {
			damage = nullptr;
		} else if (damage && !superseded->rects.empty()) {
			damage_scratch.rects.insert(damage_scratch.rects.end(), superseded->rects.begin(),
				superseded->rects.end());
		}

		bool consumed = false;
		if (buf->datas[0].type == SPA_DATA_DmaBuf && config.event_loop) {
			DmabufFrame frame;
			frame.width = (int32_t)neg_width;
			frame.height = (int32_t)neg_height;
			frame.format = DRM_FORMAT_XRGB8888;
			frame.modifier = neg_modifier;
			frame.n_planes = (int)std::min<uint32_t>(buf->n_datas, DmabufFrame::kMaxPlanes);
			for (int i = 0; i < frame.n_planes; i++) {
				frame.fd[i] = (int)buf->datas[i].fd;
				frame.offset[i] = buf->datas[i].chunk->offset;
				frame.stride[i] = (uint32_t)buf->datas[i].chunk->stride;
			}
			WLOG_DEBUG("screencast: video frame pts %" PRId64 " in buffer %p", pts_us, (void *)b);
			if (on_dmabuf_frame_cb) {
				on_dmabuf_frame_cb(frame, pts_us, b, damage);
			}
			consumed = true; // caller releases via PipeWireCapture::release()
		} else if (buf->datas[0].data) {
			uint32_t stride =
				buf->datas[0].chunk->stride > 0 ? (uint32_t)buf->datas[0].chunk->stride : neg_width * 4;
			if (on_cpu_frame_cb) {
				// Lent, not copied: the buffer stays mapped (MAP_BUFFERS) and
				// out of the producer's pool until release(), so the holder can
				// re-encode it without a copy of its own -- 33 MB a frame at 4K.
				on_cpu_frame_cb(static_cast<const uint8_t *>(buf->datas[0].data), neg_width, neg_height,
					stride, pts_us, damage, b);
				consumed = true; // caller releases via PipeWireCapture::release()
			}
		}

		if (!consumed) {
			pw_stream_queue_buffer(pw_stream_, b);
		}
	}

	// SPA_META_VideoDamage, if the producer filled it: what changed in this
	// frame relative to the previous one, for lossless refinement's tile
	// tracker (encode/refine/tile_tracker.hpp). Returns null -- "unknown,
	// treat everything as changed" -- when the meta is absent *or* holds no
	// valid region: a producer that allocates the slot but never writes it
	// leaves it zeroed, and "nothing changed" is the one answer that must
	// never be given by mistake, since a tile the tracker wrongly believes
	// unchanged stays stale on the client until it next changes.
	const DamageRegion *read_damage_meta(struct spa_buffer *buf) {
		auto *meta = spa_buffer_find_meta(buf, SPA_META_VideoDamage);
		damage_scratch.rects.clear();
		if (meta) {
			struct spa_meta_region *region;
			spa_meta_for_each(region, meta) {
				if (!spa_meta_region_is_valid(region)) {
					break;
				}
				damage_scratch.rects.push_back(
					DamageRect{region->region.position.x, region->region.position.y,
						(int32_t)region->region.size.width, (int32_t)region->region.size.height});
			}
		}
		// Say once, at INFO, what this compositor actually delivers: it
		// decides whether refinement can hold a playing video lossy, and
		// nothing else in the log would show which.
		if (!damage_meta_reported) {
			damage_meta_reported = true;
			if (!meta) {
				WLOG_INFO("screencast: no SPA_META_VideoDamage slot in the stream's buffers; "
						  "lossless refinement will settle video tiles on their hash alone");
			} else {
				WLOG_INFO(
					"screencast: SPA_META_VideoDamage negotiated (%zu bytes), first frame carries %zu rect(s)",
					(size_t)meta->size, damage_scratch.rects.size());
			}
		}
		if (meta) {
			// Area as a share of the frame, so a producer that "reports
			// damage" by naming the whole screen every frame is visible
			// in the log as ~100%.
			uint64_t area = 0;
			for (const DamageRect &r : damage_scratch.rects) {
				area += (uint64_t)std::max(r.width, 0) * (uint64_t)std::max(r.height, 0);
			}
			uint64_t frame_area = (uint64_t)neg_width * neg_height;
			WLOG_DEBUG("screencast: frame damage %zu rect(s), %.1f%% of the frame",
				damage_scratch.rects.size(), frame_area ? 100.0 * (double)area / (double)frame_area : 0.0);
		}
		return damage_scratch.rects.empty() ? nullptr : &damage_scratch;
	}

	void handle_cursor_meta(struct spa_buffer *buf) {
		auto *cursor = static_cast<struct spa_meta_cursor *>(
			spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(struct spa_meta_cursor)));
		if (!cursor) {
			return;
		}
		if (cursor->id == 0) {
			cursor_off_stream = true;
			if (on_cursor_hidden_cb) {
				on_cursor_hidden_cb();
			}
			return;
		}
		bool was_off_stream = cursor_off_stream;
		cursor_off_stream = false;
		if (cursor->bitmap_offset >= sizeof(struct spa_meta_cursor)) {
			auto *bitmap = reinterpret_cast<struct spa_meta_bitmap *>(
				reinterpret_cast<uint8_t *>(cursor) + cursor->bitmap_offset);
			if (bitmap->offset > 0 && bitmap->size.width > 0 && bitmap->size.height > 0) {
				const uint8_t *pixels = reinterpret_cast<uint8_t *>(bitmap) + bitmap->offset;
				rgba_to_argb8888_premultiplied(pixels, bitmap->size.width, bitmap->size.height,
					(uint32_t)bitmap->stride, &cursor_argb);
				cursor_width = bitmap->size.width;
				cursor_height = bitmap->size.height;
				if (on_cursor_shape_cb) {
					on_cursor_shape_cb(cursor_width, cursor_height, cursor->hotspot.x, cursor->hotspot.y,
						cursor_argb.data());
				}
			} else {
				// A bitmap header with no pixels is mutter's "empty sprite"
				// (set_empty_cursor_sprite_metadata: a cursor with no
				// texture): the pointer exists but draws nothing.
				cursor_argb.clear();
				if (on_cursor_hidden_cb) {
					on_cursor_hidden_cb();
				}
				return;
			}
		} else if (was_off_stream && !cursor_argb.empty() && on_cursor_shape_cb) {
			on_cursor_shape_cb(cursor_width, cursor_height, cursor->hotspot.x, cursor->hotspot.y,
				cursor_argb.data());
		}
		if (on_cursor_position_cb && neg_width > 0 && neg_height > 0) {
			on_cursor_position_cb((double)cursor->position.x / neg_width,
				(double)cursor->position.y / neg_height);
		}
	}

	void report_closed(const std::string &error) {
		if (closed) {
			return;
		}
		closed = true;
		if (on_error_cb) {
			on_error_cb(error);
		}
	}
	std::function<void(const std::string &)> on_error_cb;

	// Copies of the public std::functions, captured at open() time so
	// process()/handle_cursor_meta() (called from PipeWire's own
	// callbacks, which only carry `this`) can reach them.
	std::function<void(const DmabufFrame &, int64_t, void *, const DamageRegion *)> on_dmabuf_frame_cb;
	std::function<void(const uint8_t *, uint32_t, uint32_t, uint32_t, int64_t, const DamageRegion *, void *)>
		on_cpu_frame_cb;
	std::function<void(uint32_t, uint32_t, int32_t, int32_t, const uint8_t *)> on_cursor_shape_cb;
	std::function<void(double, double)> on_cursor_position_cb;
	std::function<void()> on_cursor_hidden_cb;
	std::function<void(void *)> on_buffer_removed_cb;

	bool start_pipewire(uint32_t node_id) {
		ensure_pw_init();

		pw_loop_ = pw_loop_new(nullptr);
		if (!pw_loop_) {
			report_closed("pw_loop_new failed");
			return false;
		}
		pw_loop_enter(pw_loop_);

		pw_source = wl_event_loop_add_fd(
			config.event_loop, pw_loop_get_fd(pw_loop_), WL_EVENT_READABLE,
			[](int, uint32_t, void *data) {
				auto *impl = static_cast<Impl *>(data);
				pw_loop_iterate(impl->pw_loop_, 0);
				return 0;
			},
			this);

		pw_context_ = pw_context_new(pw_loop_, nullptr, 0);
		pw_core_ = pw_context_ ? pw_context_connect(pw_context_, nullptr, 0) : nullptr;
		if (!pw_core_) {
			report_closed("pw_context_connect failed");
			return false;
		}

		struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
			"Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
		pw_stream_ = pw_stream_new(pw_core_, "wraith-screencast-capture", props);
		if (!pw_stream_) {
			report_closed("pw_stream_new failed");
			return false;
		}

		static const struct pw_stream_events stream_events = [] {
			struct pw_stream_events ev{};
			ev.version = PW_VERSION_STREAM_EVENTS;
			ev.state_changed = Impl::on_pw_state_changed;
			ev.param_changed = Impl::on_pw_param_changed;
			ev.process = Impl::on_pw_process;
			ev.add_buffer = Impl::on_pw_add_buffer;
			ev.remove_buffer = Impl::on_pw_remove_buffer;
			return ev;
		}();
		pw_stream_add_listener(pw_stream_, &stream_listener, &stream_events, this);

		uint8_t buffer[2048];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		const struct spa_pod *params[8];
		int n_params = 0;

		// BGRx at the output's size, up to 60 fps: with `modifiers` the
		// dmabuf format, without them the MemFd/CPU fallback (no modifier
		// prop at all, the convention mutter's own shm-format param uses).
		auto enum_format = [&](const std::vector<uint64_t> &modifiers) {
			struct spa_pod_frame f[2];
			struct spa_rectangle rect{config.width, config.height};
			struct spa_fraction def_fr{0, 1}, min_fr{0, 1}, max_fr{60, 1};
			spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
			spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
			spa_pod_builder_add(&b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx), 0);
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&rect), 0);
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&def_fr), 0);
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_maxFramerate,
				SPA_POD_CHOICE_RANGE_Fraction(SPA_POD_Fraction(&max_fr), SPA_POD_Fraction(&min_fr),
					SPA_POD_Fraction(&max_fr)),
				0);
			if (!modifiers.empty()) {
				spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
				spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
				spa_pod_builder_long(&b, (int64_t)modifiers[0]); // default
				for (uint64_t m : modifiers) {
					spa_pod_builder_long(&b, (int64_t)m);
				}
				spa_pod_builder_pop(&b, &f[1]);
			}
			return static_cast<const struct spa_pod *>(spa_pod_builder_pop(&b, &f[0]));
		};
		if (!config.import_modifiers.empty()) {
			params[n_params++] = enum_format(config.import_modifiers);
		}
		params[n_params++] = enum_format({});
		{
			struct spa_pod_frame f;
			spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
			spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 4),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd)), 0);
			params[n_params++] = static_cast<const struct spa_pod *>(spa_pod_builder_pop(&b, &f));
		}
		{
			// Without requesting these, negotiated buffers can come back
			// with no cursor meta slot at all even if the producer offers
			// one: the meta types that end up allocated are the
			// intersection of both ends' SPA_PARAM_Meta (PipeWire's
			// negotiation rule, not a compositor quirk).
			//
			// The size MUST be offered as a range, not a fixed Int. mutter
			// offers SPA_META_Cursor with a *fixed* size (CURSOR_META_SIZE
			// (384, 384) in meta-screen-cast-stream-src.c) and PipeWire's
			// param filter only accepts two fixed values if they are
			// equal, so a fixed request of any other size makes the link
			// silently drop the cursor meta, and GNOME delivers no cursor at
			// all (kwin offers a range, so a fixed request works there). A
			// range covering both intersects either way: a fixed producer
			// value inside the range wins as-is, range vs range picks the
			// producer's default.
			// The range tops out at the wire's cap (gdp::kMaxCursorDim),
			// so no producer can hand over a shape too big to send.
			constexpr uint32_t kDefaultCursorDim = 384;
			constexpr uint32_t kMaxCursorDim = gdp::kMaxCursorDim;
			constexpr int kCursorMetaBase =
				(int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap));
			params[n_params++] = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
				SPA_PARAM_META_size,
				SPA_POD_CHOICE_RANGE_Int(kCursorMetaBase + (int)(kDefaultCursorDim * kDefaultCursorDim * 4),
					kCursorMetaBase + 4, kCursorMetaBase + (int)(kMaxCursorDim * kMaxCursorDim * 4))));
			params[n_params++] = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header))));
			// Damage regions, for lossless refinement (read_damage_meta()).
			// Optional on the producer's side: a compositor that doesn't
			// offer it simply allocates no slot, and the tracker falls
			// back to hashing every tile. Offered as a range so that a
			// producer with a fixed idea of the array size still
			// intersects (the cursor meta's lesson above).
			constexpr int kDamageRegionBytes = (int)sizeof(struct spa_meta_region);
			params[n_params++] = static_cast<const struct spa_pod *>(
				spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
					SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size,
					SPA_POD_CHOICE_RANGE_Int(kDamageRegionBytes * 16, kDamageRegionBytes,
						kDamageRegionBytes * 256)));
		}

		auto flags = (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
		if (pw_stream_connect(pw_stream_, PW_DIRECTION_INPUT, node_id, flags, params, n_params) != 0) {
			report_closed("pw_stream_connect failed");
			return false;
		}
		return true;
	}
};

PipeWireCapture::PipeWireCapture(uint32_t node_id) : impl_(std::make_unique<Impl>()) {
	impl_->node_id = node_id;
}
PipeWireCapture::~PipeWireCapture() {
	close();
}

bool PipeWireCapture::open(const Params &config) {
	impl_->config = config;
	impl_->on_dmabuf_frame_cb = on_dmabuf_frame;
	impl_->on_cpu_frame_cb = on_cpu_frame;
	impl_->on_buffer_removed_cb = on_buffer_removed;
	impl_->on_cursor_shape_cb = on_cursor_shape;
	impl_->on_cursor_position_cb = on_cursor_position;
	impl_->on_cursor_hidden_cb = on_cursor_hidden;
	impl_->on_error_cb = on_closed;
	impl_->neg_width = config.width;
	impl_->neg_height = config.height;

	return impl_->start_pipewire(impl_->node_id);
}

void PipeWireCapture::close() {
	if (impl_->pw_source) {
		wl_event_source_remove(impl_->pw_source);
		impl_->pw_source = nullptr;
	}
	if (impl_->pw_stream_) {
		pw_stream_destroy(impl_->pw_stream_);
		impl_->pw_stream_ = nullptr;
	}
	if (impl_->pw_core_) {
		pw_core_disconnect(impl_->pw_core_);
		impl_->pw_core_ = nullptr;
	}
	if (impl_->pw_context_) {
		pw_context_destroy(impl_->pw_context_);
		impl_->pw_context_ = nullptr;
	}
	if (impl_->pw_loop_) {
		pw_loop_leave(impl_->pw_loop_);
		pw_loop_destroy(impl_->pw_loop_);
		impl_->pw_loop_ = nullptr;
	}
}

void PipeWireCapture::release(void *release_token) {
	auto *b = static_cast<struct pw_buffer *>(release_token);
	if (impl_->pw_stream_ && b && impl_->live_buffers.count(b)) {
		pw_stream_queue_buffer(impl_->pw_stream_, b);
	}
}

} // namespace wraith
