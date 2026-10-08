// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Video/audio datagram headers (gdp-spec.md §3.2, §9, §10). Pure
// wire encode/decode, no allocation, no transport dependency -- exercised
// directly by a fuzzer (libgdp/fuzz/), since untrusted lengths reach
// buffer arithmetic here rather than in protobuf's generated parsers
// (refine.hpp's container parser is the other such place).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace gdp {

// Payload bytes per video slice when the connection's real limit isn't
// known (slice_payload_for() below). 1100 bytes of payload plus the
// channel byte, header and QUIC's own overhead fits under QUIC's 1200-byte
// minimum UDP payload, so it fits every path QUIC can run on at all. Also
// the bound audio is sized against: a 10ms Opus frame is always far under
// it, so audio is never sliced.
inline constexpr size_t kFallbackDatagramPayload = 1100;

// The most payload one video slice ever carries, whatever the connection
// allows: slice_video_frame()'s stack buffer is sized by it. The
// transport caps path MTU discovery at 1500, which already keeps its
// maximum datagram below this; the cap only matters for a path MTU raised
// past it.
inline constexpr size_t kMaxSlicePayload = 1500;

enum class DatagramChannel : uint8_t {
	kVideo = 0x01,
	kAudio = 0x02,
	kMicrophone = 0x03, // client -> host, laid out as kAudio (capability "microphone")
};

// Byte 0 of every QUIC DATAGRAM frame on a session connection. Returns
// nullopt for an empty or unrecognized datagram (§3.2: unrecognized
// channels are silently dropped, forward-compatible with future channel
// types).
std::optional<DatagramChannel> peek_channel(const uint8_t *data, size_t len);

struct VideoDatagramHeader {
	static constexpr size_t kSize = 16;
	static constexpr uint8_t kFlagKeyframe = 0x01;

	uint8_t stream_id = 0;
	uint32_t frame_id = 0;
	uint16_t slice_idx = 0;
	uint16_t slice_count = 1;
	uint8_t flags = 0;
	uint32_t pts = 0;
	// How long the host held this frame, capture to hand-off for sending,
	// in kHostLatencyUnitUs units (gdp-spec.md §9.1); 0 = not reported.
	// host_latency_from_us() builds it, host_latency_us() reads it back.
	uint16_t host_latency = 0;

	static constexpr uint32_t kHostLatencyUnitUs = 10;

	bool keyframe() const { return (flags & kFlagKeyframe) != 0; }

	// Wire value for a host latency of `us`: rounded to the unit,
	// at least 1 (0 means unreported), saturating at 0xFFFF (~655 ms).
	// A negative `us` (the capture stamp is from a clock ahead of ours)
	// is unreported.
	static uint16_t host_latency_from_us(int64_t us);
	// 0 when unreported.
	uint32_t host_latency_us() const { return (uint32_t)host_latency * kHostLatencyUnitUs; }

	// Writes DatagramChannel::kVideo + this header (1 + kSize bytes) to
	// `out`, which must have at least 1 + kSize bytes of room.
	void encode(uint8_t *out) const;

	// Parses a full datagram (channel byte + header + payload). On
	// success, `payload`/`payload_len` point into `data` (no copy).
	static bool decode(const uint8_t *data, size_t len, VideoDatagramHeader *out, const uint8_t **payload,
		size_t *payload_len);
};

// Payload bytes per video slice for a connection whose largest datagram
// is `max_datagram_size` (gdp::Connection::max_datagram_size()): that
// minus the channel byte and VideoDatagramHeader, capped at
// kMaxSlicePayload. kFallbackDatagramPayload while the limit is unknown
// (0) or too small to carry a header at all.
size_t slice_payload_for(size_t max_datagram_size);

// Splits one coded frame into ceil(len / max_payload) (at least 1) video
// datagrams per gdp-spec.md §9 and hands each complete datagram
// (channel byte + header + payload) to `send`, in slice order. `hdr`
// supplies stream_id/frame_id/flags/pts, shared by every slice; slice_idx
// and slice_count are filled in here. `max_payload` is clamped to
// [1, kMaxSlicePayload]; every slice but the last carries exactly that
// much. Alloc-free: the datagram lives in a stack buffer that's only
// valid for the duration of the callback. Stops early and returns false
// if `send` does (the frame can't be delivered partially, so there's no
// point sending the rest).
bool slice_video_frame(VideoDatagramHeader hdr, const uint8_t *data, size_t len, size_t max_payload,
	const std::function<bool(const uint8_t *datagram, size_t datagram_len)> &send);

struct AudioDatagramHeader {
	static constexpr size_t kSize = 6;

	uint16_t seq = 0;
	uint32_t pts = 0;

	// `channel` is kAudio (host -> client) or kMicrophone (client -> host):
	// the two share one layout.
	void encode(uint8_t *out, DatagramChannel channel = DatagramChannel::kAudio) const;

	static bool decode(const uint8_t *data, size_t len, AudioDatagramHeader *out, const uint8_t **payload,
		size_t *payload_len, DatagramChannel channel = DatagramChannel::kAudio);
};

} // namespace gdp
