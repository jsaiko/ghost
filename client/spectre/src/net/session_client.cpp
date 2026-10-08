// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "net/session_client.hpp"

#include "gdp/cert_fingerprint.hpp"
#include "gdp/clock.hpp"
#include "gdp/datagram.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/framing.hpp"
#include "gdp/negotiation.hpp"
#include "gdp/refine.hpp"
#include "gdp/version.hpp"
#include "session.pb.h"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <utility>

namespace spectre {

namespace {

// SessionHello.decoders: informational only. Decoder tries Vulkan Video
// and the platform's own hardware decoder (in spectre -X's order) and falls
// back to FFmpeg software decode (decoder.hpp); which one actually opens
// isn't known until after SessionAccept, so those three are listed. (V4L2
// and PyroWave's compute decode go unnamed.)
#if defined(_WIN32)
const std::vector<std::string> kDecoders = {"vulkan", "d3d11va", "software"};
#elif defined(__APPLE__)
const std::vector<std::string> kDecoders = {"vulkan", "videotoolbox", "software"};
#else
const std::vector<std::string> kDecoders = {"vulkan", "vaapi", "software"};
#endif
// SessionHello.capabilities: optional extensions this build understands
// (gdp-spec.md §6.7). SDL's clipboard API is cross-platform, so spectre
// offers clipboard sync unconditionally; it's the host that may have no
// mechanism for it (see wraith's matching list in gdp_session.cpp).
// Lossless refinement is offered unless set_lossless_refinement() turned it
// off: whether spectre's layer is built is the menu's Lossless row, a
// RefinePause away, not something fixed at connect. "gamepad" and
// "microphone" are offered only on request (set_gamepad_forwarding(),
// set_microphone()) -- see offered_capabilities(). The host only accepts
// "gamepad" when it can actually create devices (wraith's
// gdp_session.cpp), so what's in effect is exactly what works.
const std::vector<std::string> kAlwaysOfferedCapabilities = {gdp::kCapabilityClipboard};

// The one output spectre shows (see SessionClient::reassembler_).
constexpr uint8_t kStreamId = 0;

// Bits of AckState::completed_mask that refer to frames that existed (see
// AckState's comment).
uint64_t seen_mask(uint32_t window_span) {
	return window_span >= 64 ? ~0ull : ((1ull << window_span) - 1);
}

// An InputEnvelope stamped with the send time, ready for one event.
gdp::session::InputEnvelope make_input_envelope() {
	gdp::session::InputEnvelope env;
	env.set_client_time_us(gdp::monotonic_us());
	return env;
}

// SessionAccept.network_profile / NetworkProfileChanged.profile as
// host_network_profile() spells it; AUTO (a wraith that doesn't report
// one) and anything newer than this build are empty.
std::string network_profile_name(gdp::session::NetworkProfile profile) {
	switch (profile) {
	case gdp::session::NETWORK_PROFILE_LAN: return "lan";
	case gdp::session::NETWORK_PROFILE_INTERNET: return "internet";
	case gdp::session::NETWORK_PROFILE_MOBILE: return "mobile";
	default: return "";
	}
}

DisplayInfo display_info(const gdp::session::DisplayDescriptor &d) {
	DisplayInfo display;
	display.width = d.width();
	display.height = d.height();
	display.refresh_mhz = d.refresh_mhz();
	display.scale = d.scale();
	return display;
}

} // namespace

SessionClient::SessionClient() = default;
SessionClient::~SessionClient() = default;

std::vector<std::string> SessionClient::offered_capabilities() const {
	std::vector<std::string> offered = kAlwaysOfferedCapabilities;
	if (offer_gamepad_) {
		offered.push_back(gdp::kCapabilityGamepad);
		if (offer_hid_) {
			offered.push_back(gdp::kCapabilityHid);
		}
	}
	if (offer_refine_) {
		offered.push_back(gdp::kCapabilityRefine);
	}
	if (offer_microphone_) {
		offered.push_back(gdp::kCapabilityMicrophone);
	}
	return offered;
}

void SessionClient::set_decodable_codecs(std::vector<std::string> codecs) {
	offered_codecs_ = std::move(codecs);
}

void SessionClient::set_preferred_codec(const std::string &codec) {
	const std::vector<std::string> &offered = offered_codecs_;
	if (std::find(offered.begin(), offered.end(), codec) == offered.end()) {
		std::string have;
		for (const std::string &token : offered) {
			have += " " + token;
		}
		SLOG_INFO("session_client: ignoring preferred codec \"%s\" -- this client can't decode it (have:%s)",
			codec.c_str(), have.c_str());
		return;
	}
	preferred_codec_ = codec;
}

void SessionClient::set_network_profile(const std::string &profile) {
	if (profile != "auto" && profile != "lan" && profile != "internet" && profile != "mobile") {
		SLOG_INFO("session_client: ignoring network profile \"%s\" -- expected auto, lan, internet or mobile",
			profile.c_str());
		return;
	}
	network_profile_ = profile;
}

bool SessionClient::connect(const std::string &host, uint16_t port, const std::string &token,
	const std::string &cert_sha256) {
	token_ = token;
	expected_cert_sha256_ = cert_sha256;
	on_certificate = [this](const std::string &presented) {
		presented_cert_sha256_ = presented;
		return presented == expected_cert_sha256_;
	};
	return connect_transport(host, port, gdp::kAlpn);
}

void SessionClient::on_connected() {
	control_ = conn_->open_control_stream();
	input_ = conn_->open_input_stream();
	if (!control_ || !input_) {
		disconnect("failed to open control/input streams");
		return;
	}
	control_->on_data = [this](const uint8_t *data, size_t len) { handle_control_data(data, len); };
	input_->on_data = [this](const uint8_t *data, size_t len) { handle_input_data(data, len); };

	gdp::session::ControlEnvelope hello_env;
	auto *hello = hello_env.mutable_hello();
	hello->set_token(token_);
	hello->set_take_over(take_over_);
	// -C's pick first, then the default order with that token dropped from
	// wherever it sat, so the rest stays available as fallback.
	if (!preferred_codec_.empty()) {
		hello->add_codecs(preferred_codec_);
	}
	for (const auto &codec : offered_codecs_) {
		if (codec == preferred_codec_) {
			continue;
		}
		hello->add_codecs(codec);
	}
	for (const auto &decoder : kDecoders) {
		hello->add_decoders(decoder);
	}
	for (const auto &name : offered_capabilities()) {
		hello->add_capabilities(name);
	}
	if (network_profile_ == "lan") {
		hello->set_network_profile(gdp::session::NETWORK_PROFILE_LAN);
	} else if (network_profile_ == "internet") {
		hello->set_network_profile(gdp::session::NETWORK_PROFILE_INTERNET);
	} else if (network_profile_ == "mobile") {
		hello->set_network_profile(gdp::session::NETWORK_PROFILE_MOBILE);
	}
	if (requested_width_ > 0 && requested_height_ > 0) {
		auto *display = hello->add_displays();
		display->set_width(requested_width_);
		display->set_height(requested_height_);
	}
	send_control(hello_env);

	conn_->on_datagram = [this](const uint8_t *data, size_t len, uint64_t arrival_us) {
		handle_datagram(data, len, arrival_us);
	};
}

void SessionClient::on_connection_shutdown() {
	if (certificate_rejected()) {
		disconnect("the host's certificate does not match the one the login server vouched for "
				   "(expected " +
			gdp::format_fingerprint(expected_cert_sha256_) + ", got " +
			gdp::format_fingerprint(presented_cert_sha256_) + ") -- refusing to send the session token");
		return;
	}
	if (!disconnect_reported_) {
		disconnect_code_ = conn_->shutdown_error_code();
	}
	disconnect(conn_->shutdown_reason());
}

void SessionClient::disconnect(const std::string &reason, uint64_t error_code) {
	if (disconnect_reported_) {
		return;
	}
	disconnect_reported_ = true;
	if (error_code != 0 && conn_) {
		conn_->close(error_code);
	}
	if (on_disconnected) {
		on_disconnected(reason);
	}
}

void SessionClient::send_control(const gdp::session::ControlEnvelope &env) {
	if (!control_->send_message(env)) {
		SLOG_ERROR("session_client: failed to encode control message");
	}
}

void SessionClient::send_input(const gdp::session::InputEnvelope &env) {
	if (!input_ || !accepted_) {
		return;
	}
	if (!input_->send_message(env)) {
		SLOG_ERROR("session_client: failed to encode input message");
	}
}

void SessionClient::handle_control_data(const uint8_t *data, size_t len) {
	gdp::session::ControlEnvelope env;
	bool ok = control_reader_.feed_and_drain(data, len, &env, [&] {
		if (env.has_accept()) {
			return handle_accept(env);
		} else if (env.has_reject()) {
			disconnect(env.reject().reason());
			return false;
		} else if (env.has_ping()) {
			gdp::session::ControlEnvelope pong_env;
			pong_env.mutable_pong()->set_nonce(env.ping().nonce());
			send_control(pong_env);
		} else if (env.has_gateway_path()) {
			upstream_rtt_us_.store(env.gateway_path().upstream_rtt_us(), std::memory_order_relaxed);
		} else if (env.has_cursor_shape()) {
			const auto &shape = env.cursor_shape();
			if (on_cursor_shape && shape.width() <= gdp::kMaxCursorDim &&
				shape.height() <= gdp::kMaxCursorDim &&
				shape.argb8888().size() == (size_t)shape.width() * shape.height() * 4) {
				on_cursor_shape(shape.width(), shape.height(), (int32_t)shape.hotspot_x(),
					(int32_t)shape.hotspot_y(), (const uint8_t *)shape.argb8888().data());
			}
		} else if (env.has_cursor_position()) {
			if (on_cursor_position) {
				on_cursor_position(env.cursor_position().x(), env.cursor_position().y());
			}
		} else if (env.has_clipboard()) {
			handle_clipboard_data(env.clipboard());
		} else if (env.has_network_profile_changed()) {
			host_network_profile_ = network_profile_name(env.network_profile_changed().profile());
		} else if (env.has_diagnostics_request()) {
			// The host asks, the client isn't told why: log it, and answer at
			// most every 30 s, so a host can't drain the log by asking.
			constexpr std::chrono::seconds kDiagnosticsInterval(30);
			auto now = std::chrono::steady_clock::now();
			bool recently = last_diagnostics_.time_since_epoch().count() != 0 &&
				now - last_diagnostics_ < kDiagnosticsInterval;
			SLOG_INFO("session_client: the host asked for diagnostics%s",
				recently ? " (too soon, ignored)" : "");
			if (on_diagnostics_request && !recently) {
				last_diagnostics_ = now;
				// The newest part is the useful part: keep the tail.
				constexpr size_t kMaxDiagnostics = 512 * 1024;
				std::string text = on_diagnostics_request();
				if (text.size() > kMaxDiagnostics) {
					text.erase(0, text.size() - kMaxDiagnostics);
				}
				gdp::session::ControlEnvelope reply;
				reply.mutable_diagnostics_report()->set_text(text);
				send_control(reply);
			}
		} else if (env.has_displays_changed()) {
			const auto &changed = env.displays_changed();
			if (changed.outputs_size() > 0 && on_displays_changed) {
				on_displays_changed(display_info(changed.outputs(0).display()));
			}
		}
		return true;
	});
	if (!ok) {
		disconnect("malformed control frame", control_reader_.error_code());
	}
}

bool SessionClient::handle_accept(const gdp::session::ControlEnvelope &env) {
	const auto &accept = env.accept();

	// gdp-spec.md §6.6: wraith's pick must be one of the codecs offered in
	// SessionHello. Anything else means the two ends disagree about the
	// protocol, and there's nothing this build could decode anyway.
	SessionInfo session;
	session.codec = accept.codec();
	via_gateway_.store(accept.via_gateway(), std::memory_order_relaxed);
	const std::vector<std::string> &offered = offered_codecs_;
	if (std::find(offered.begin(), offered.end(), session.codec) == offered.end()) {
		disconnect("host chose video codec \"" + session.codec + "\", which was not offered",
			static_cast<uint64_t>(gdp::ErrorCode::kNoCommonCodec));
		return false;
	}
	// Likewise only capabilities spectre offered can be in effect; a
	// name wraith made up on its own is dropped rather than failing the
	// session (it's harmless: nothing here would act on it).
	session.capabilities = gdp::negotiate_capabilities(offered_capabilities(),
		std::vector<std::string>(accept.capabilities().begin(), accept.capabilities().end()));
	clipboard_enabled_ = gdp::has_capability(session.capabilities, gdp::kCapabilityClipboard);
	clipboard_echo_.reset();
	gamepad_enabled_ = gdp::has_capability(session.capabilities, gdp::kCapabilityGamepad);
	hid_enabled_.store(gamepad_enabled_ && gdp::has_capability(session.capabilities, gdp::kCapabilityHid),
		std::memory_order_release);
	microphone_enabled_ = gdp::has_capability(session.capabilities, gdp::kCapabilityMicrophone);
	if (microphone_enabled_) {
		// The format is the host's to dictate (gdp-spec.md §6.8); a codec this build
		// cannot encode just runs the session without a microphone.
		const auto &mic = accept.microphone();
		microphone_format_.codec = mic.codec();
		microphone_format_.sample_rate_hz = mic.sample_rate_hz();
		microphone_format_.channels = mic.channels();
		microphone_format_.frame_ms = mic.frame_ms();
		if (mic.codec() != gdp::kAudioCodecOpus || microphone_format_.samples_per_frame() == 0 ||
			microphone_format_.channels == 0 || microphone_format_.channels > 2) {
			microphone_enabled_ = false;
		}
	}
	microphone_seq_ = 0;
	session.encoder = accept.encoder();
	session.host_user = accept.host_user();
	session.host_name = accept.host_name();
	host_network_profile_ = network_profile_name(accept.network_profile());

	DisplayInfo display;
	if (accept.outputs_size() > 0) {
		display = display_info(accept.outputs(0).display());
	}

	AudioInfo audio;
	if (accept.has_audio()) {
		audio.valid = true;
		audio.codec = accept.audio().codec();
		audio.sample_rate_hz = accept.audio().sample_rate_hz();
		audio.channels = accept.audio().channels();
		audio.frame_ms = accept.audio().frame_ms();
		if (audio.codec.empty()) {
			// No codec named: Opus (gdp-spec.md §10).
			audio.codec = gdp::kAudioCodecOpus;
		}
		if (audio.codec != gdp::kAudioCodecOpus) {
			// Unlike video there's no way to negotiate this yet (spectre
			// offers no audio codec list), and audio is optional -- so an
			// unknown one just means a silent session, not a failed one.
			SLOG_INFO("session_client: host audio codec \"%s\" unsupported, continuing without audio",
				audio.codec.c_str());
			audio.valid = false;
		} else if (audio.samples_per_frame_all_channels() == 0) {
			// Nothing downstream can size a frame from this (the audio
			// callback would loop forever filling zero-byte frames).
			SLOG_INFO("session_client: host audio config %u Hz, %u ch, %ums is unusable, continuing without"
					  " audio",
				audio.sample_rate_hz, audio.channels, audio.frame_ms);
			audio.valid = false;
		}
	}

	accepted_ = true;
	// This client's session clock zeroes here, as wraith's did when it
	// sent SessionAccept (gdp-spec.md §11).
	session_epoch_us_ = gdp::monotonic_us();
	receive_ = ReceiveAccum{};
	receive_.window_start_us = session_epoch_us_;
	if (on_accepted) {
		on_accepted(session, display, audio);
	}
	return true;
}

void SessionClient::handle_datagram(const uint8_t *data, size_t len, uint64_t arrival_us) {
	auto channel = gdp::peek_channel(data, len);
	if (channel == gdp::DatagramChannel::kAudio) {
		gdp::AudioDatagramHeader hdr;
		const uint8_t *payload;
		size_t payload_len;
		if (gdp::AudioDatagramHeader::decode(data, len, &hdr, &payload, &payload_len) && on_audio_frame) {
			on_audio_frame(hdr.seq, hdr.pts, payload, payload_len);
		}
		return;
	}
	if (channel != gdp::DatagramChannel::kVideo) {
		return; // anything else is dropped per gdp-spec.md §3.2
	}

	gdp::VideoDatagramHeader hdr;
	const uint8_t *payload;
	size_t payload_len;
	if (!gdp::VideoDatagramHeader::decode(data, len, &hdr, &payload, &payload_len)) {
		return; // malformed datagram: drop, gdp-spec.md §3.2 has no error path for these
	}

	if (hdr.stream_id != kStreamId) {
		return; // an output spectre doesn't show (see reassembler_)
	}

	receive_.bytes += len;
	if (hdr.slice_idx == 0) {
		// The first slice waits behind nothing of its own frame, so its
		// delay moves with the path's queues rather than with frame size.
		uint32_t now = (uint32_t)(gdp::monotonic_us() - session_epoch_us_);
		int32_t delay = gdp::pts_diff(now, hdr.pts);
		if (receive_.delay_samples == 0 || delay < receive_.delay_min_us) {
			receive_.delay_min_us = delay;
		}
		receive_.delay_sum_us += delay;
		receive_.delay_samples++;
	}

	TrainTiming &train = train_;
	if (!train.active || train.frame_id != hdr.frame_id) {
		train = TrainTiming{};
		train.active = true;
		train.frame_id = hdr.frame_id;
		train.first_us = arrival_us;
	} else {
		train.bytes_after_first += len;
	}
	train.last_us = arrival_us;
	train.datagrams++;

	gdp::VideoFrameReassembler::Frame frame;
	if (!reassembler_.push(hdr, payload, payload_len, &frame)) {
		return;
	}
	note_train(hdr.slice_count);
	// Every slice carries the same value; this one's stands for the frame.
	if (uint32_t host_us = hdr.host_latency_us()) {
		HostLatency &hl = host_latency_;
		if (hl.samples == 0 || host_us < hl.min_us) {
			hl.min_us = host_us;
		}
		hl.max_us = std::max(hl.max_us, host_us);
		hl.sum_us += host_us;
		hl.samples++;
	}
	bool consumed = true;
	if (on_video_frame) {
		consumed = on_video_frame(frame.keyframe, frame.data.data(), frame.data.size());
	}
	note_frame_seen(frame.frame_id, consumed);
}

void SessionClient::note_train(uint32_t slice_count) {
	// Fewer datagrams than this spread too little to time against the
	// receive path's own jitter; a report carries at most kMaxTrains.
	constexpr uint32_t kMinTrainDatagrams = 16;
	constexpr size_t kMaxTrains = 8;
	TrainTiming &train = train_;
	// A duplicate would count twice; only a train of exactly the frame's
	// slices is a clean one.
	if (train.datagrams != slice_count || train.datagrams < kMinTrainDatagrams ||
		train.last_us <= train.first_us || receive_.trains.size() >= kMaxTrains) {
		return;
	}
	receive_.trains.push_back(TrainSample{train.frame_id, train.datagrams,
		(uint32_t)std::min<uint64_t>(train.bytes_after_first, UINT32_MAX),
		(uint32_t)std::min<uint64_t>(train.last_us - train.first_us, UINT32_MAX)});
	train.active = false;
}

void SessionClient::note_frame_seen(uint32_t frame_id, bool completed) {
	AckState &st = ack_;
	uint64_t bit = completed ? 1ull : 0ull;
	if (!st.has_data) {
		st.highest_frame_id = frame_id;
		st.completed_mask = bit;
		st.window_span = 1;
		st.has_data = true;
		return;
	}
	if (frame_id == st.highest_frame_id) {
		st.completed_mask = (st.completed_mask & ~1ull) | bit;
		return;
	}
	// gdp-spec.md §9.1: frame_id is monotonically increasing (mod 2^32) --
	// pts_diff's signed-subtraction trick applies here too.
	int32_t diff = gdp::pts_diff(frame_id, st.highest_frame_id);
	if (diff > 0) {
		uint32_t shift = (uint32_t)diff;
		st.completed_mask = (shift >= 64) ? bit : ((st.completed_mask << shift) | bit);
		st.window_span = (shift >= 64 - st.window_span) ? 64 : st.window_span + shift;
		st.highest_frame_id = frame_id;
	} else {
		uint32_t age = (uint32_t)(-diff);
		if (age < 64) {
			if (completed) {
				st.completed_mask |= (1ull << age);
			} else {
				st.completed_mask &= ~(1ull << age);
			}
		}
	}
}

void SessionClient::send_key(uint32_t hid_usage, bool pressed) {
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *key = env.mutable_key();
	key->set_hid_usage(hid_usage);
	key->set_state(pressed ? gdp::session::KEY_STATE_PRESSED : gdp::session::KEY_STATE_RELEASED);
	send_input(env);
}

void SessionClient::send_pointer_motion(double dx, double dy, bool absolute) {
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *motion = env.mutable_pointer_motion();
	motion->set_dx(dx);
	motion->set_dy(dy);
	motion->set_absolute(absolute);
	send_input(env);
}

void SessionClient::send_pointer_button(uint32_t button, bool pressed) {
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *btn = env.mutable_pointer_button();
	btn->set_button(button);
	btn->set_state(pressed ? gdp::session::KEY_STATE_PRESSED : gdp::session::KEY_STATE_RELEASED);
	send_input(env);
}

void SessionClient::send_pointer_axis(double horizontal, double vertical) {
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *axis = env.mutable_pointer_axis();
	axis->set_horizontal(horizontal);
	axis->set_vertical(vertical);
	send_input(env);
}

void SessionClient::handle_input_data(const uint8_t *data, size_t len) {
	// gdp-spec.md §8.4: the host sends here only under "hid"; anything
	// else is ignored rather than fatal.
	gdp::session::HostInputEnvelope env;
	bool ok = input_reader_.feed_and_drain(data, len, &env, [&] {
		if (!hid_enabled()) {
			return true;
		}
		if (env.has_hid_rejected()) {
			if (on_hid_rejected) {
				on_hid_rejected(env.hid_rejected().pad_index(), env.hid_rejected().reason());
			}
		} else if (env.has_hid_output()) {
			if (on_hid_output) {
				on_hid_output(env.hid_output().pad_index(), env.hid_output().data());
			}
		} else if (env.has_hid_get_report()) {
			const auto &g = env.hid_get_report();
			if (on_hid_get_report) {
				on_hid_get_report(g.pad_index(), g.request_id(), g.report_id(), g.type());
			}
		} else if (env.has_hid_set_report()) {
			const auto &r = env.hid_set_report();
			if (on_hid_set_report) {
				on_hid_set_report(r.pad_index(), r.request_id(), r.report_id(), r.type(), r.data());
			}
		}
		return true;
	});
	if (!ok) {
		disconnect("malformed frame on the input stream");
	}
}

void SessionClient::send_hid_connect(const gdp::session::HidConnect &connect) {
	if (!hid_enabled()) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	*env.mutable_hid_connect() = connect;
	send_input(env);
}

void SessionClient::send_hid_input(uint32_t pad_index, const uint8_t *data, size_t len) {
	if (!hid_enabled()) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *in = env.mutable_hid_input();
	in->set_pad_index(pad_index);
	in->set_data(data, std::min(len, gdp::kMaxHidReport));
	send_input(env);
}

void SessionClient::send_hid_get_report_reply(uint32_t pad_index, uint32_t request_id, bool failed,
	const uint8_t *data, size_t len) {
	if (!hid_enabled()) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *reply = env.mutable_hid_get_report_reply();
	reply->set_pad_index(pad_index);
	reply->set_request_id(request_id);
	reply->set_failed(failed);
	if (!failed) {
		reply->set_data(data, std::min(len, gdp::kMaxHidReport));
	}
	send_input(env);
}

void SessionClient::send_hid_set_report_reply(uint32_t pad_index, uint32_t request_id, bool failed) {
	if (!hid_enabled()) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *reply = env.mutable_hid_set_report_reply();
	reply->set_pad_index(pad_index);
	reply->set_request_id(request_id);
	reply->set_failed(failed);
	send_input(env);
}

void SessionClient::send_microphone_packet(const uint8_t *data, size_t len) {
	if (!microphone_enabled_ || !conn_ || len == 0) {
		return;
	}
	gdp::AudioDatagramHeader hdr;
	hdr.seq = microphone_seq_.fetch_add(1, std::memory_order_relaxed);
	hdr.pts = (uint32_t)(gdp::monotonic_us() - session_epoch_us_);
	std::vector<uint8_t> datagram(1 + gdp::AudioDatagramHeader::kSize + len);
	hdr.encode(datagram.data(), gdp::DatagramChannel::kMicrophone);
	std::memcpy(datagram.data() + 1 + gdp::AudioDatagramHeader::kSize, data, len);
	conn_->send_datagram(datagram.data(), datagram.size(), /*priority=*/true);
}

void SessionClient::send_gamepad_connect(uint32_t pad_index, const std::string &name) {
	if (!gamepad_enabled_) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *connect = env.mutable_gamepad_connect();
	connect->set_pad_index(pad_index);
	connect->set_name(name);
	send_input(env);
}

void SessionClient::send_gamepad_disconnect(uint32_t pad_index) {
	if (!gamepad_enabled_) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	env.mutable_gamepad_disconnect()->set_pad_index(pad_index);
	send_input(env);
}

void SessionClient::send_gamepad_state(uint32_t pad_index, const gdp::GamepadSnapshot &state) {
	if (!gamepad_enabled_) {
		return;
	}
	gdp::session::InputEnvelope env = make_input_envelope();
	auto *gamepad = env.mutable_gamepad();
	gamepad->set_pad_index(pad_index);
	// The whole state every time, in gdp/gamepad.hpp's order: the host
	// diffs, so a lost or reordered frame can never leave a button stuck.
	for (float axis : state.axes) {
		gamepad->add_axes(axis);
	}
	for (bool button : state.buttons) {
		gamepad->add_buttons(button);
	}
	send_input(env);
}

void SessionClient::request_keyframe() {
	if (!control_ || !accepted_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	env.mutable_keyframe_request()->set_stream_id(kStreamId);
	send_control(env);
}

void SessionClient::send_clipboard(const std::string &utf8) {
	if (!control_ || !accepted_ || !clipboard_enabled_) {
		return;
	}
	if (utf8.size() > gdp::kMaxClipboardBytes) {
		SLOG_INFO("session_client: not sending a %zu-byte clipboard, over the %zu-byte cap", utf8.size(),
			gdp::kMaxClipboardBytes);
		return;
	}
	if (!clipboard_echo_.should_send(utf8)) {
		// Empty (an unfocused SDL window on Wayland reads ""), or the very
		// text wraith just sent us -- forwarding that back is how a copy
		// ping-pongs forever.
		return;
	}
	clipboard_echo_.note_sent(utf8);
	gdp::session::ControlEnvelope env;
	auto *clipboard = env.mutable_clipboard();
	clipboard->set_mime_type(gdp::kClipboardMimeText);
	clipboard->set_data(utf8);
	send_control(env);
}

void SessionClient::handle_clipboard_data(const gdp::session::ClipboardData &clipboard) {
	if (!clipboard_enabled_) {
		// A host sending what it never negotiated: dropped rather than
		// acted on, whatever it claims (gdp-spec.md §7.9).
		return;
	}
	if (clipboard.mime_type() != gdp::kClipboardMimeText) {
		SLOG_INFO("session_client: dropping ClipboardData with mime \"%s\" (v1 carries text/plain only)",
			clipboard.mime_type().c_str());
		return;
	}
	if (!clipboard_echo_.should_apply(clipboard.data()) || !on_clipboard_text) {
		return;
	}
	// The dedup bookkeeping for the *other* direction happens in
	// note_clipboard_applied(), which StreamSession calls right before
	// SDL_SetClipboardText: that's when the text is actually applied (it
	// may be held until SDL is up), and SDL raises
	// SDL_EVENT_CLIPBOARD_UPDATE from inside that call.
	on_clipboard_text(clipboard.data());
}

void SessionClient::send_logout_request() {
	if (!control_ || !accepted_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	env.mutable_logout_request();
	send_control(env);
}

void SessionClient::send_resolution_change(uint32_t width, uint32_t height) {
	if (!control_ || !accepted_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	auto *change = env.mutable_resolution_change();
	change->set_stream_id(0); // the one video stream (SessionAccept.outputs)
	change->mutable_display()->set_width(width);
	change->mutable_display()->set_height(height);
	send_control(env);
}

void SessionClient::send_refine_pause(bool paused) {
	if (!control_ || !accepted_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	env.mutable_refine_pause()->set_paused(paused);
	send_control(env);
}

SessionClient::LinkStats SessionClient::link_stats() const {
	LinkStats stats;
	if (conn_) {
		stats.rtt_us = conn_->rtt_us() + upstream_rtt_us_.load(std::memory_order_relaxed);
	}
	if (ack_.has_data) {
		stats.window_span = ack_.window_span;
		stats.lost_recent =
			(uint32_t)std::bitset<64>(~ack_.completed_mask & seen_mask(ack_.window_span)).count();
	}
	stats.delay_min_us = last_delay_min_us_;
	stats.delay_avg_us = last_delay_avg_us_;
	stats.delay_samples = last_delay_samples_;
	return stats;
}

SessionClient::HostLatency SessionClient::take_host_latency() {
	HostLatency out = host_latency_;
	host_latency_ = HostLatency{};
	return out;
}

void SessionClient::record_frame_timing(uint32_t decode_us, uint32_t present_us) {
	timing_.decode_us_sum += decode_us;
	timing_.present_us_sum += present_us;
	timing_.sample_count++;
}

void SessionClient::send_stats_report() {
	if (!control_ || !accepted_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	auto *report = env.mutable_stats();
	report->set_rtt_us(conn_->rtt_us() + upstream_rtt_us_.load(std::memory_order_relaxed));

	if (ack_.has_data) {
		auto *stream_stats = report->add_streams();
		stream_stats->set_stream_id(kStreamId);
		stream_stats->set_highest_frame_id_acked(ack_.highest_frame_id);
		stream_stats->set_loss_bitmap(~ack_.completed_mask & seen_mask(ack_.window_span));
		if (timing_.sample_count > 0) {
			stream_stats->set_decode_us((uint32_t)(timing_.decode_us_sum / timing_.sample_count));
			stream_stats->set_present_us((uint32_t)(timing_.present_us_sum / timing_.sample_count));
		}
		timing_ = TimingAccum{};

		uint64_t now = gdp::monotonic_us();
		// Never 0: a zero interval is how wraith recognizes a client that
		// doesn't report these at all.
		stream_stats->set_interval_us((uint32_t)std::max<uint64_t>(1, now - receive_.window_start_us));
		stream_stats->set_bytes_received(receive_.bytes);
		last_delay_samples_ = receive_.delay_samples;
		last_delay_min_us_ = 0;
		last_delay_avg_us_ = 0;
		if (receive_.delay_samples > 0) {
			last_delay_min_us_ = receive_.delay_min_us;
			last_delay_avg_us_ = (int32_t)(receive_.delay_sum_us / receive_.delay_samples);
			stream_stats->set_delay_samples(receive_.delay_samples);
			stream_stats->set_delay_min_us(last_delay_min_us_);
			stream_stats->set_delay_avg_us(last_delay_avg_us_);
		}
		for (const TrainSample &sample : receive_.trains) {
			gdp::session::FrameTrain *train = stream_stats->add_trains();
			train->set_frame_id(sample.frame_id);
			train->set_datagrams(sample.datagrams);
			train->set_bytes(sample.bytes);
			train->set_span_us(sample.span_us);
		}
		receive_ = ReceiveAccum{};
		receive_.window_start_us = now;
	}
	send_control(env);
}

} // namespace spectre
