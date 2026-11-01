// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/datagram.hpp"

#include "le_bytes.hpp"

#include <algorithm>
#include <cstring>

namespace gdp {

using le::get_u16;
using le::get_u32;
using le::put_u16;
using le::put_u32;

std::optional<DatagramChannel> peek_channel(const uint8_t *data, size_t len) {
	if (len < 1) {
		return std::nullopt;
	}
	switch (data[0]) {
	case static_cast<uint8_t>(DatagramChannel::kVideo): return DatagramChannel::kVideo;
	case static_cast<uint8_t>(DatagramChannel::kAudio): return DatagramChannel::kAudio;
	case static_cast<uint8_t>(DatagramChannel::kMicrophone): return DatagramChannel::kMicrophone;
	case static_cast<uint8_t>(DatagramChannel::kVideoParity): return DatagramChannel::kVideoParity;
	default: return std::nullopt;
	}
}

void VideoDatagramHeader::encode(uint8_t *out) const {
	out[0] = static_cast<uint8_t>(DatagramChannel::kVideo);
	out[1] = stream_id;
	put_u32(out + 2, frame_id);
	put_u16(out + 6, slice_idx);
	put_u16(out + 8, slice_count);
	out[10] = flags;
	put_u32(out + 11, pts);
	put_u16(out + 15, host_latency);
}

uint16_t VideoDatagramHeader::host_latency_from_us(int64_t us) {
	if (us < 0) {
		return 0;
	}
	int64_t units = (us + kHostLatencyUnitUs / 2) / kHostLatencyUnitUs;
	return (uint16_t)std::clamp<int64_t>(units, 1, 0xFFFF);
}

bool VideoDatagramHeader::decode(const uint8_t *data, size_t len, VideoDatagramHeader *out,
	const uint8_t **payload, size_t *payload_len) {
	if (len < 1 + kSize || data[0] != static_cast<uint8_t>(DatagramChannel::kVideo)) {
		return false;
	}
	out->stream_id = data[1];
	out->frame_id = get_u32(data + 2);
	out->slice_idx = get_u16(data + 6);
	out->slice_count = get_u16(data + 8);
	out->flags = data[10];
	out->pts = get_u32(data + 11);
	out->host_latency = get_u16(data + 15);
	if (out->slice_count == 0 || out->slice_idx >= out->slice_count) {
		return false;
	}
	*payload = data + 1 + kSize;
	*payload_len = len - 1 - kSize;
	return true;
}

size_t slice_payload_for(size_t max_datagram_size) {
	constexpr size_t kOverhead = 1 + VideoDatagramHeader::kSize;
	if (max_datagram_size <= kOverhead) {
		return kFallbackDatagramPayload;
	}
	size_t payload = max_datagram_size - kOverhead;
	return payload < kMaxSlicePayload ? payload : kMaxSlicePayload;
}

bool slice_video_frame(VideoDatagramHeader hdr, const uint8_t *data, size_t len, size_t max_payload,
	const std::function<bool(const uint8_t *datagram, size_t datagram_len)> &send) {
	if (max_payload == 0) {
		max_payload = 1;
	} else if (max_payload > kMaxSlicePayload) {
		max_payload = kMaxSlicePayload;
	}
	size_t slice_count = (len + max_payload - 1) / max_payload;
	if (slice_count == 0) {
		slice_count = 1; // an empty frame still goes out as one (empty) slice
	}
	hdr.slice_count = static_cast<uint16_t>(slice_count);

	uint8_t datagram[1 + VideoDatagramHeader::kSize + kMaxSlicePayload];
	for (size_t slice = 0; slice < slice_count; slice++) {
		size_t offset = slice * max_payload;
		size_t chunk = len - offset < max_payload ? len - offset : max_payload;

		hdr.slice_idx = static_cast<uint16_t>(slice);
		hdr.encode(datagram); // writes the channel byte + header, gdp-spec.md §3.2/§9
		if (chunk > 0) {
			memcpy(datagram + 1 + VideoDatagramHeader::kSize, data + offset, chunk);
		}
		if (!send(datagram, 1 + VideoDatagramHeader::kSize + chunk)) {
			return false;
		}
	}
	return true;
}

size_t fec_block_count(size_t slices) {
	if (slices == 0) {
		return 0;
	}
	size_t blocks = (slices + kFecMaxBlockData - 1) / kFecMaxBlockData;
	return blocks > kFecMaxBlocks ? 0 : blocks;
}

void VideoParityHeader::encode(uint8_t *out) const {
	out[0] = static_cast<uint8_t>(DatagramChannel::kVideoParity);
	out[1] = stream_id;
	put_u32(out + 2, frame_id);
	out[6] = block_idx;
	out[7] = block_count;
	out[8] = data_shards;
	out[9] = parity_idx;
	out[10] = parity_count;
	out[11] = flags;
	put_u32(out + 12, pts);
	put_u32(out + 16, frame_len);
}

bool VideoParityHeader::decode(const uint8_t *data, size_t len, VideoParityHeader *out,
	const uint8_t **payload, size_t *payload_len) {
	if (len < 1 + kSize + 1 || data[0] != static_cast<uint8_t>(DatagramChannel::kVideoParity)) {
		return false;
	}
	out->stream_id = data[1];
	out->frame_id = get_u32(data + 2);
	out->block_idx = data[6];
	out->block_count = data[7];
	out->data_shards = data[8];
	out->parity_idx = data[9];
	out->parity_count = data[10];
	out->flags = data[11];
	out->pts = get_u32(data + 12);
	out->frame_len = get_u32(data + 16);
	if (out->block_count == 0 || out->block_idx >= out->block_count || out->data_shards == 0 ||
		out->parity_count == 0 || out->parity_idx >= out->parity_count ||
		(size_t)out->data_shards + out->parity_count > kFecMaxBlockShards) {
		return false;
	}
	*payload = data + 1 + kSize;
	*payload_len = len - 1 - kSize;
	return true;
}

size_t fec_slice_payload_for(size_t max_datagram_size) {
	constexpr size_t kOverhead = 1 + VideoParityHeader::kSize;
	if (max_datagram_size <= kOverhead) {
		return kFallbackDatagramPayload;
	}
	size_t payload = max_datagram_size - kOverhead;
	return payload < kMaxSlicePayload ? payload : kMaxSlicePayload;
}

void AudioDatagramHeader::encode(uint8_t *out, DatagramChannel channel) const {
	out[0] = static_cast<uint8_t>(channel);
	put_u16(out + 1, seq);
	put_u32(out + 3, pts);
}

bool AudioDatagramHeader::decode(const uint8_t *data, size_t len, AudioDatagramHeader *out,
	const uint8_t **payload, size_t *payload_len, DatagramChannel channel) {
	if (len < 1 + kSize || data[0] != static_cast<uint8_t>(channel)) {
		return false;
	}
	out->seq = get_u16(data + 1);
	out->pts = get_u32(data + 3);
	*payload = data + 1 + kSize;
	*payload_len = len - 1 - kSize;
	return true;
}

} // namespace gdp
