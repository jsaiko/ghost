// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Forward error correction for video (gdp-spec.md §9.7, capability "fec").
//
// The code is systematic Reed-Solomon over GF(2^8) (polynomial 0x11d) with
// a Cauchy generator: data shards go out unchanged, parity shard j of a
// block of k data shards is
//
//     parity[j] = sum over i of data[i] / ((255 - j) xor i)
//
// so any k of a block's k + m shards rebuild the rest (every square
// submatrix of a Cauchy matrix is invertible). A shard shorter than the
// block's shard length counts as zero-padded. The region arithmetic uses
// SSSE3 or AVX2 (chosen at run time) on x86 and NEON on arm64: a 4K
// keyframe's parity is a few megabytes of multiply-adds.
#pragma once

#include "gdp/datagram.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gdp {

// SessionHello/SessionAccept.capabilities name (gdp-spec.md §6.7, §9.7).
inline constexpr const char *kCapabilityFec = "fec";

// Writes `m` parity shards of `shard_len` bytes each from `k` data shards;
// data[i] has data_len[i] <= shard_len bytes, the rest zero. k + m must not
// exceed kFecMaxBlockShards.
void fec_encode(const uint8_t *const *data, const size_t *data_len, size_t k, uint8_t *const *parity,
	size_t m, size_t shard_len);

// Rebuilds the data shards not marked in `have_data`, each `shard_len`
// bytes at data[i] (present ones zero-padded to that length), from the
// parity shards marked in `have_parity`. False, touching nothing, when
// fewer parity shards are present than data shards are missing.
bool fec_recover(uint8_t *const *data, const bool *have_data, size_t k, const uint8_t *const *parity,
	const bool *have_parity, size_t m, size_t shard_len);

// Which multiply-add the codec runs on this CPU ("avx2", "ssse3", "neon"
// or "scalar"), for logs.
const char *fec_implementation();

// Sends the parity datagrams of one frame whose data slices
// slice_video_frame() just sent with the same `hdr`, `data`, `len` and
// `max_payload`. `parity_for(k)` says how many parity shards a block of k
// data shards gets (0 for none; capped so the block stays within
// kFecMaxBlockShards). Parity goes out round-robin over the blocks, like
// the data. `scratch` holds the parity between calls, so a session reuses
// its allocation. Returns the parity payload bytes sent; stops early when
// `send` does.
size_t send_video_parity(const VideoDatagramHeader &hdr, const uint8_t *data, size_t len, size_t max_payload,
	const std::function<size_t(size_t k)> &parity_for,
	const std::function<bool(const uint8_t *datagram, size_t datagram_len)> &send,
	std::vector<uint8_t> *scratch);

} // namespace gdp
