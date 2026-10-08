// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// spectre_netprobe: a headless GDP session client for measuring the
// transport and wraith's rate control over a real (or netem-shaped) link.
// It speaks the session exactly as spectre does -- the same SessionClient,
// so the same SessionHello, reassembly, and 250 ms StatsReports -- but
// decodes nothing and opens no window: it counts what arrives and prints
// one line a second, then a summary. tools/netem-harness drives
// it against a wraith in a network namespace.
#include "log.hpp"
#include "net/session_client.hpp"

#include "gdp/cert_fingerprint.hpp"
#include "gdp/clock.hpp"

#include <poll.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage(const char *argv0) {
	fprintf(stderr,
		"Usage: %s -h host -p port -t token -P sha256 [-s seconds] [-N profile] [-r WIDTHxHEIGHT] [-C codec] [-o file] [-R] [-L seconds]\n"
		"  Connects like spectre, decodes nothing, and prints per-second arrival stats:\n"
		"  frames, keyframes, Mbps, RTT, and the one-way delay above the run's minimum\n"
		"  (queuing). -s runs that long (default 20) then prints a summary and exits.\n"
		"  -N is spectre's -N (auto, lan, internet, mobile); -C offers only this codec (default\n"
		"  h264 -- nothing is decoded, so any the host can encode will do); -R offers lossless refinement,\n"
		"  which puts the host on its CPU-frame path. -o writes every received coded frame to\n"
		"  a file, for checking the stream with ffmpeg afterwards. -L sends a LogoutRequest that\n"
		"  many seconds in and waits for the host to end the session (wraith's teardown path).\n",
		argv0);
}

// p-th percentile (0..100) of `v`, which it sorts.
double percentile(std::vector<double> &v, double p) {
	if (v.empty()) {
		return 0;
	}
	std::sort(v.begin(), v.end());
	size_t i = (size_t)(p / 100.0 * (double)(v.size() - 1) + 0.5);
	return v[std::min(i, v.size() - 1)];
}

} // namespace

int main(int argc, char *argv[]) {
	spectre::Log::init();
	std::string host;
	std::string token;
	std::string cert;
	std::string profile = "auto";
	std::string codec = "h264";
	std::string dump_path;
	uint32_t width = 0;
	uint32_t height = 0;
	int port = 0;
	int seconds = 20;
	int logout_after = 0; // -L: 0 for never
	bool refine = false;
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (strcmp(arg, "-R") == 0) {
			refine = true;
			continue;
		}
		const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
		if (!v) {
			usage(argv[0]);
			return 1;
		}
		i++;
		if (strcmp(arg, "-h") == 0) {
			host = v;
		} else if (strcmp(arg, "-p") == 0) {
			port = atoi(v);
		} else if (strcmp(arg, "-t") == 0) {
			token = v;
		} else if (strcmp(arg, "-P") == 0) {
			// Colons and case as spectre -P accepts them.
			cert.clear();
			for (const char *c = v; *c; c++) {
				if (*c != ':') {
					cert += (char)tolower((unsigned char)*c);
				}
			}
			if (!gdp::is_sha256_hex(cert)) {
				fprintf(stderr, "netprobe: -P is not a SHA-256 fingerprint\n");
				return 1;
			}
		} else if (strcmp(arg, "-s") == 0) {
			seconds = atoi(v);
		} else if (strcmp(arg, "-N") == 0) {
			profile = v;
		} else if (strcmp(arg, "-C") == 0) {
			codec = v;
		} else if (strcmp(arg, "-L") == 0) {
			logout_after = atoi(v);
		} else if (strcmp(arg, "-o") == 0) {
			dump_path = v;
		} else if (strcmp(arg, "-r") == 0) {
			if (sscanf(v, "%ux%u", &width, &height) != 2) {
				usage(argv[0]);
				return 1;
			}
		} else {
			usage(argv[0]);
			return 1;
		}
	}
	if (host.empty() || port <= 0 || cert.empty()) {
		usage(argv[0]);
		return 1;
	}

	spectre::SessionClient client;
	client.set_decodable_codecs({codec});
	client.set_network_profile(profile);
	client.set_requested_display(width, height);
	client.set_lossless_refinement(refine);

	FILE *dump = nullptr;
	if (!dump_path.empty() && !(dump = fopen(dump_path.c_str(), "wb"))) {
		fprintf(stderr, "netprobe: can't open %s\n", dump_path.c_str());
		return 1;
	}

	bool accepted = false;
	bool done = false;
	std::string end_reason;
	uint64_t frames = 0, keyframes = 0, bytes = 0;
	// Gaps between delivered frames, in ms, once the session is measured:
	// stutter shows up here long before it moves the per-second fps.
	uint64_t last_frame_us = 0;
	bool measuring = false;
	std::vector<double> frame_gaps;
	client.on_accepted = [&](const spectre::SessionInfo &session, const spectre::DisplayInfo &display,
							 const spectre::AudioInfo &) {
		accepted = true;
		printf("netprobe: accepted, %s via %s, %ux%u\n", session.codec.c_str(), session.encoder.c_str(),
			display.width, display.height);
		printf("  t   fps  key   Mbps   rtt_ms  queue_ms(avg)  lost/64\n");
	};
	client.on_disconnected = [&](const std::string &reason) {
		done = true;
		end_reason = reason;
	};
	client.on_video_frame = [&](bool keyframe, const uint8_t *data, size_t len) {
		if (dump) {
			fwrite(data, 1, len, dump);
		}
		uint64_t now = gdp::monotonic_us();
		if (measuring && last_frame_us != 0) {
			frame_gaps.push_back((now - last_frame_us) / 1000.0);
		}
		last_frame_us = now;
		frames++;
		keyframes += keyframe ? 1 : 0;
		bytes += len;
		return true;
	};

	if (!client.connect(host, (uint16_t)port, token, cert)) {
		fprintf(stderr, "netprobe: connect failed\n");
		return 1;
	}

	uint64_t start = gdp::monotonic_us();
	uint64_t next_report = start + 250'000;
	uint64_t next_print = 0;
	uint64_t last_frames = 0, last_keyframes = 0, last_bytes = 0;
	int32_t delay_floor = INT32_MAX;
	std::vector<double> fps_samples, mbps_samples, queue_samples, rtt_samples;
	int tick = 0;
	while (!done) {
		struct pollfd pfd{client.notify_fd(), POLLIN, 0};
		poll(&pfd, 1, 4);
		client.dispatch();
		uint64_t now = gdp::monotonic_us();
		if (!accepted) {
			if (now - start > 10'000'000) {
				fprintf(stderr, "netprobe: no SessionAccept after 10 s\n");
				return 1;
			}
			continue;
		}
		if (next_print == 0) {
			next_print = now + 1'000'000;
			start = now;
			measuring = true;
		}
		if (now >= next_report) {
			client.send_stats_report();
			next_report = now + 250'000;
			spectre::SessionClient::LinkStats link = client.link_stats();
			if (link.delay_samples > 0) {
				delay_floor = std::min(delay_floor, link.delay_min_us);
				queue_samples.push_back((link.delay_avg_us - delay_floor) / 1000.0);
			}
		}
		if (now >= next_print) {
			next_print += 1'000'000;
			tick++;
			spectre::SessionClient::LinkStats link = client.link_stats();
			double fps = (double)(frames - last_frames);
			double mbps = (double)(bytes - last_bytes) * 8 / 1e6;
			double queue = link.delay_samples > 0 && delay_floor != INT32_MAX
				? (link.delay_avg_us - delay_floor) / 1000.0
				: 0;
			printf("%3d  %4.0f  %3llu  %6.2f  %7.1f  %13.1f  %4u/%u\n", tick, fps,
				(unsigned long long)(keyframes - last_keyframes), mbps, link.rtt_us / 1000.0, queue,
				link.lost_recent, link.window_span);
			fflush(stdout);
			fps_samples.push_back(fps);
			mbps_samples.push_back(mbps);
			rtt_samples.push_back(link.rtt_us / 1000.0);
			last_frames = frames;
			last_keyframes = keyframes;
			last_bytes = bytes;
			if (logout_after > 0 && tick == logout_after) {
				printf("netprobe: sending LogoutRequest\n");
				client.send_logout_request();
			}
			if (tick >= seconds) {
				break;
			}
		}
	}
	if (dump) {
		fclose(dump);
	}
	if (done) {
		printf("netprobe: disconnected: %s\n", end_reason.c_str());
	}
	if (fps_samples.empty()) {
		return 1;
	}
	double fps_avg = 0, mbps_avg = 0;
	for (double f : fps_samples)
		fps_avg += f;
	for (double m : mbps_samples)
		mbps_avg += m;
	fps_avg /= fps_samples.size();
	mbps_avg /= mbps_samples.size();
	printf("summary: %zu s, %.1f fps avg, %.2f Mbps avg, %llu keyframes, "
		   "queuing delay p50 %.1f / p95 %.1f / max %.1f ms, rtt p50 %.1f ms\n",
		fps_samples.size(), fps_avg, mbps_avg, (unsigned long long)keyframes, percentile(queue_samples, 50),
		percentile(queue_samples, 95), percentile(queue_samples, 100), percentile(rtt_samples, 50));
	if (!frame_gaps.empty()) {
		double median = percentile(frame_gaps, 50);
		size_t late = 0;
		for (double g : frame_gaps)
			late += g > median * 1.5 ? 1 : 0;
		printf("frame gaps: p50 %.1f / p99 %.1f / max %.1f ms, %zu of %zu over 1.5x the median\n", median,
			percentile(frame_gaps, 99), percentile(frame_gaps, 100), late, frame_gaps.size());
	}
	return 0;
}
