// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/encoder_factory.hpp"

#include "util/config.hpp"
#include "util/log.hpp"
#include "encode/nvenc/nvenc_encoder.hpp"
#include "encode/refine/refine_encoder.hpp"
#include "encode/software/x264_encoder.hpp"
#include "encode/vaapi/vaapi_av1_encoder.hpp"
#include "encode/vaapi/vaapi_h264_encoder.hpp"
#include "encode/vaapi/vaapi_hevc_encoder.hpp"
#ifdef WRAITH_HAVE_PYROWAVE
#include "encode/pyrowave/pyrowave_encoder.hpp"
#endif

#include <va/va_drm.h>

#include <algorithm>
#include <functional>

namespace wraith {

namespace {

struct BackendRow {
	gdp::VideoCodec codec;
	// SessionAccept.encoder for this row.
	const char *name;
	// Hardware rows are skipped entirely under wraith -F.
	bool hardware;
	// Hardware rows also need a render node to even attempt.
	bool needs_drm_fd;
	std::function<std::unique_ptr<Encoder>()> make;
};

// The table. Rows are tried top to bottom within a codec, so the order
// within each codec group is the preference order.
//
// Adding a codec is adding rows here plus the backend they name; nothing
// else in wraith's encode path is codec-aware. docs/design/encoding.md
// lists the other three places a codec touches (the wire token in libgdp,
// spectre's decoder mapping, gdp-spec.md). Lossless refinement is not a
// row: it wraps whatever row opened (create_encoder below), so every
// codec here gets it without knowing.
const std::vector<BackendRow> &backend_table() {
	static const std::vector<BackendRow> rows = {
		// h264: the universal floor, and the only codec with a software
		// row. Every session can fall back here.
		{gdp::VideoCodec::H264, "vaapi", true, true,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<VaapiH264Encoder>(); }},
		{gdp::VideoCodec::H264, "nvenc", true, false,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<NvencEncoder>(); }},
		{gdp::VideoCodec::H264, "software", false, false,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<X264Encoder>(); }},

		// h265: hardware only. There is no software HEVC encoder here, so
		// on a host without a hardware HEVC encoder no row opens and
		// accept_session() moves on to the client's next preference.
		{gdp::VideoCodec::H265, "vaapi", true, true,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<VaapiHevcEncoder>(); }},
		{gdp::VideoCodec::H265, "nvenc", true, false,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<NvencEncoder>(); }},

		// av1: hardware only, like h265 -- there is no software AV1
		// encoder here, and on a GPU with no AV1 encode support (it is
		// newer silicon than HEVC encode: RDNA3 and up on AMD, Arc on
		// Intel, Ada on NVIDIA) no row opens and accept_session() moves on
		// to the client's next preference.
		{gdp::VideoCodec::AV1, "vaapi", true, true,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<VaapiAv1Encoder>(); }},
		{gdp::VideoCodec::AV1, "nvenc", true, false,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<NvencEncoder>(); }},

#ifdef WRAITH_HAVE_PYROWAVE
		// pyrowave: Vulkan compute on the render node's GPU, no fallback.
		// Not a VA-API or NVENC row, so hardware_encodable_codecs() never
		// counts it and it plays no part in picking the GPU.
		{gdp::VideoCodec::Pyrowave, "pyrowave", true, true,
			[]() -> std::unique_ptr<Encoder> { return std::make_unique<PyrowaveEncoder>(); }},
#endif
	};
	return rows;
}

} // namespace

const std::vector<std::string> &supported_video_codecs() {
	static const std::vector<std::string> codecs = [] {
		std::vector<std::string> result;
		for (const std::string &token : gdp::all_video_codec_tokens()) {
			gdp::VideoCodec codec = gdp::video_codec_from_token(token);
			bool have_backend = std::any_of(backend_table().begin(), backend_table().end(),
				[codec](const BackendRow &row) { return row.codec == codec; });
			if (have_backend && config().encode.codec_enabled(token)) {
				result.push_back(token);
			}
		}
		return result;
	}();
	return codecs;
}

std::vector<std::string> hardware_encodable_codecs(int drm_fd) {
	std::vector<std::string> result;

	// VA-API: an encode entrypoint for the profile each VA-API row would
	// ask for. The profile comes from the row's own encoder, so this can't
	// drift from what open() really creates. `display` stays null when
	// VA-API doesn't initialise on the node (any NVIDIA one, for a start).
	VADisplay display = vaGetDisplayDRM(drm_fd);
	if (display) {
		// libva prints its driver-loading banner through the info
		// callback, once per node probed; that's noise at startup.
		vaSetInfoCallback(display, nullptr, nullptr);
		int major = 0, minor = 0;
		if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
			// As in VaapiEncoderBase::init_display(): nothing to terminate.
			display = nullptr;
		}
	}
	auto has_encode_entrypoint = [display](VAProfile profile) {
		std::vector<VAEntrypoint> entrypoints((size_t)vaMaxNumEntrypoints(display));
		int count = 0;
		if (vaQueryConfigEntrypoints(display, profile, entrypoints.data(), &count) != VA_STATUS_SUCCESS) {
			return false;
		}
		return std::find(entrypoints.begin(), entrypoints.begin() + count, VAEntrypointEncSlice) !=
			entrypoints.begin() + count;
	};

	// NVENC: asked once for every codec, since asking costs a session.
	std::vector<gdp::VideoCodec> nvenc_codecs = NvencEncoder::encodable_codecs(drm_fd);

	for (const std::string &token : gdp::all_video_codec_tokens()) {
		gdp::VideoCodec codec = gdp::video_codec_from_token(token);
		bool encodable =
			std::any_of(backend_table().begin(), backend_table().end(), [&](const BackendRow &row) {
				if (row.codec != codec || !row.hardware) {
					return false;
				}
				std::unique_ptr<Encoder> encoder = row.make();
				if (auto *vaapi = dynamic_cast<VaapiEncoderBase *>(encoder.get())) {
					return display && has_encode_entrypoint(vaapi->enc_profile());
				}
				if (dynamic_cast<NvencEncoder *>(encoder.get())) {
					return std::find(nvenc_codecs.begin(), nvenc_codecs.end(), codec) != nvenc_codecs.end();
				}
				return false;
			});
		if (encodable) {
			result.push_back(token);
		}
	}
	if (display) {
		vaTerminate(display);
	}
	return result;
}

EncoderSelection create_encoder(const EncoderConfig &config, bool force_software, bool refine) {
	EncoderSelection selection;
	const char *token = gdp::video_codec_token(config.codec);
	bool any_row = false;

	for (const BackendRow &row : backend_table()) {
		if (row.codec != config.codec) {
			continue;
		}
		any_row = true;

		if (row.hardware && force_software) {
			WLOG_INFO("encoder: skipping %s %s backend (-F: software encode forced)", token, row.name);
			continue;
		}
		if (row.needs_drm_fd && config.drm_fd < 0) {
			WLOG_INFO("encoder: skipping %s %s backend (renderer has no DRM fd)", token, row.name);
			continue;
		}

		std::unique_ptr<Encoder> encoder = row.make();
		if (!encoder) {
			continue;
		}
		// The refinement wrapper opens its base itself, with this same
		// config, so a base that won't open still falls through to the
		// next row exactly as it would unwrapped.
		if (refine) {
			encoder = std::make_unique<RefineEncoder>(std::move(encoder));
		}
		// Backends that can't encode off-thread (NVENC) ignore it;
		// refinement passes it on to its base and keeps its own packets in
		// order (RefineEncoder::in_flight_).
		encoder->set_asynchronous(true);
		if (!encoder->open(config)) {
			WLOG_INFO("encoder: %s %s backend did not open, trying the next one", token, row.name);
			continue;
		}

		selection.encoder = std::move(encoder);
		selection.backend_name = row.name;
		if (refine) {
			selection.backend_name += "+refine";
		}
		WLOG_INFO("encoder: opened %ux%u %s (%s)", config.width, config.height, token,
			selection.backend_name.c_str());
		return selection;
	}

	if (!any_row) {
		WLOG_ERROR("encoder: no backend registered for codec \"%s\"", token);
	} else {
		WLOG_ERROR("encoder: every backend for codec \"%s\" failed to open", token);
	}
	return selection;
}

} // namespace wraith
