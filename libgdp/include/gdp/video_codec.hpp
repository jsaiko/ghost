// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The video codec tokens a session can run on (gdp-spec.md §6.6) and
// how a token maps to an enum.
//
// A token names a *wire format*, never an implementation -- VA-API, NVENC
// and x264 output are all "h264" (gdp-spec.md §6.6). Adding a codec is:
//   1. a token + enum entry here,
//   2. an encoder backend registered in wraith's encode/encoder_factory.cpp,
//   3. a decoder mapping in spectre's decode/decoder.cpp,
//   4. a line in gdp-spec.md §6.6.
//  Lossless refinement
// (gdp/refine.hpp) is orthogonal: a capability that wraps any of these,
// so a new codec gets it for free.
#pragma once

#include <string>
#include <vector>

namespace gdp {

// Wire tokens (SessionHello.codecs / SessionAccept.codec). Lowercase.
inline constexpr const char *kVideoCodecH264 = "h264";
inline constexpr const char *kVideoCodecH265 = "h265";
inline constexpr const char *kVideoCodecAV1 = "av1";
// PyroWave (github.com/Themaister/pyrowave): an intra-only wavelet codec
// for wired LANs -- every frame stands alone, at several times the
// bitrate of the others. Offered and accepted only under the rules in
// gdp-spec.md §6.6.
inline constexpr const char *kVideoCodecPyrowave = "pyrowave";

enum class VideoCodec {
	Unknown,
	H264,
	H265,
	AV1,
	Pyrowave,
};

// Unknown for any token this build doesn't define (a peer from the future,
// or a typo) -- never an error by itself; negotiation just won't match it.
VideoCodec video_codec_from_token(const std::string &token);

// "" for VideoCodec::Unknown.
const char *video_codec_token(VideoCodec codec);

// Every token this build knows, in the order a client offers them when it
// has expressed no preference of its own -- which puts pyrowave first (a
// client offers it only on a wired LAN, gdp-spec.md §6.6), then av1, then
// h265, then h264 last, preferring the newer/more efficient wire formats when the
// host can serve them. av1 and h265 need a hardware encoder on the host
// (VA-API or NVENC) and have no software fallback; h264 comes last as the
// fallback of last resort because it is the only codec with one (x264,
// after the hardware rows -- see host/wraith/src/encode/encoder_factory.cpp).
// spectre -C moves a token to the front.
//
// Each end intersects this with what it can actually encode/decode --
// wraith in SessionServices::supported_video_codecs(), spectre in
// SessionClient's offered list -- so nothing here is a promise on its own.
const std::vector<std::string> &all_video_codec_tokens();

} // namespace gdp
