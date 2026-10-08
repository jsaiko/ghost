// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/video_codec.hpp"

namespace gdp {

VideoCodec video_codec_from_token(const std::string &token) {
	if (token == kVideoCodecH264) {
		return VideoCodec::H264;
	}
	if (token == kVideoCodecH265) {
		return VideoCodec::H265;
	}
	if (token == kVideoCodecAV1) {
		return VideoCodec::AV1;
	}
	if (token == kVideoCodecPyrowave) {
		return VideoCodec::Pyrowave;
	}
	return VideoCodec::Unknown;
}

const char *video_codec_token(VideoCodec codec) {
	switch (codec) {
	case VideoCodec::H264: return kVideoCodecH264;
	case VideoCodec::H265: return kVideoCodecH265;
	case VideoCodec::AV1: return kVideoCodecAV1;
	case VideoCodec::Pyrowave: return kVideoCodecPyrowave;
	case VideoCodec::Unknown: break;
	}
	return "";
}

const std::vector<std::string> &all_video_codec_tokens() {
	static const std::vector<std::string> tokens = {
		kVideoCodecPyrowave,
		kVideoCodecAV1,
		kVideoCodecH265,
		kVideoCodecH264,
	};
	return tokens;
}

} // namespace gdp
