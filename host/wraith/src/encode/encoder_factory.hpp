// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Which encoder backend serves which wire codec (docs/design/encoding.md).
//
// Everything codec-specific about wraith's encode side is the table in
// encoder_factory.cpp: one row per (codec, backend) pair, in the order
// they should be tried. create_encoder() walks the rows for the requested
// codec and returns the first backend that opens -- so "VA-API H.264,
// falling back to x264" is two rows rather than branching logic, and
// adding a codec is adding rows plus the backend they name.
//
// supported_video_codecs() is the same table read the other way: the
// codecs that have at least one backend this build could plausibly open,
// which is what wraith offers in negotiation (gdp-spec.md §6.6).
#pragma once

#include "encode/encoder.hpp"

#include "gdp/video_codec.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wraith {

struct EncoderSelection {
	std::unique_ptr<Encoder> encoder;
	// SessionAccept.encoder (gdp-spec.md §6.5): the backend
	// that actually opened -- "vaapi", "nvenc", "pyrowave" or "software", with "+refine"
	// appended when the lossless refinement wrapper sits on top. Empty
	// when nothing opened.
	std::string backend_name;

	explicit operator bool() const { return encoder != nullptr; }
};

// Opens the best available backend for `config.codec`. `force_software`
// skips every hardware row (wraith -F). `refine` wraps whichever backend
// opened in the lossless refinement decorator (encode/refine/
// refine_encoder.hpp) -- the session negotiated the "refine" capability;
// the base selection is identical either way. Returns an empty selection,
// having logged why, if no row for the codec opened.
EncoderSelection create_encoder(const EncoderConfig &config, bool force_software, bool refine);

// The codecs to advertise in negotiation, in the preference order
// gdp::all_video_codec_tokens() defines, filtered down to those with a
// registered backend and on in wraith.toml's [encode.codecs]. Cheap and
// constant -- it does not probe the hardware, so a codec listed here can
// still fail to open at create_encoder() time, which negotiation tolerates:
// GdpSession::accept_session() moves on to the client's next preference.
const std::vector<std::string> &supported_video_codecs();

// The codecs the render node `drm_fd` has a hardware encoder for -- a
// VA-API encode entrypoint, or NVENC on an NVIDIA GPU -- in the same
// preference order -- the one place wraith probes the
// hardware ahead of a session, so preferred_render_node() (util/
// render_node.hpp) can pick the GPU that encodes the best codec on a
// multi-GPU host. An entrypoint is necessary but not sufficient, so as
// with supported_video_codecs() create_encoder() can still fail later.
// Empty when neither VA-API nor NVENC initialises on the node.
std::vector<std::string> hardware_encodable_codecs(int drm_fd);

} // namespace wraith
