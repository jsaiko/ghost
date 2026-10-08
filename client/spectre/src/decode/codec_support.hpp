// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Which video codecs this client can actually decode -- what SessionHello.
// codecs offers (gdp-spec.md §6.6). FFmpeg merely having a decoder is
// not enough: its av1 decoder is hwaccel-only, so offering av1 on a GPU
// with no AV1 decode (say an NVIDIA Turing) lets wraith pick a codec every
// one of Decoder::open()'s backends then fails on, and the session never
// shows a frame. Wraith only ever sees the offer, so it has to be right up
// front.
//
// Runs before connecting, which is before the presenter exists (its window
// is sized from SessionAccept), so it looks at the GPU on its own: a
// throwaway Vulkan instance on the same physical device VulkanDevice will
// pick, then the platform decoder (D3D11VA / VA-API / VideoToolbox) on that
// same GPU.
#pragma once

#include "decode/decoder.hpp"

#include <string>
#include <vector>

namespace spectre {

// gdp's codec tokens, in its preference order, narrowed to those at least
// one of the decoders Decoder::open() would try for `backend` (spectre -X)
// can decode here: Vulkan Video, the platform's own, V4L2 (Linux),
// PyroWave's compute decode, or -- for h264 only -- FFmpeg software.
// `drm_render_node` is the node VulkanPresenter::init() will get (Linux
// only). h264 in software is always there, so the list is never empty;
// with -X software it is exactly h264.
std::vector<std::string> probe_decodable_codecs(DecodeBackend backend, const char *drm_render_node);

// Every decode path that works here, as "<path>:<codec>" -- "vulkan:av1",
// "vaapi:h264", "compute:pyrowave", "software:h264" -- for spectre
// --probe-decoders, which Wisp's agent reports to Veil. Software lists
// every codec FFmpeg decodes in software, not only the ones it offers.
std::vector<std::string> probe_decode_paths(const char *drm_render_node);

// Linux: whether a V4L2 memory-to-memory device (/dev/video*) decodes
// `codec` as a stateful decoder -- what FFmpeg's h264_v4l2m2m and
// hevc_v4l2m2m drive, e.g. the Raspberry Pi 4's bcm2835-codec. (Stateless
// ones, like the Pi's HEVC block, take a different API and don't count.)
// Always false elsewhere.
bool v4l2_m2m_decodes(gdp::VideoCodec codec);

} // namespace spectre
