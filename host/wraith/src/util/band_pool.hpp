// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A few long-lived helper threads that each take one band of a frame's
// per-frame work while the calling thread takes another, for work that
// splits into independent bands: colour conversion (xrgb_convert.cpp) and
// lossless refinement's tile hashing (tile_tracker.cpp). On one thread
// they are most of wraith's main thread at 4K -- also capture and
// input; both are memory-bound, so a handful of bands is enough. The
// encoder's own threads are idle while they run.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace wraith {

class BandPool {
public:
	static BandPool &get();

	// How many bands run() splits into: the helpers plus the caller.
	size_t bands() const { return threads_.size() + 1; }

	// Runs job(0..bands()-1) once each, job(0) on the calling thread, and
	// returns when every one has finished. Not reentrant, and for one
	// calling thread at a time (wraith's main thread).
	void run(const std::function<void(size_t)> &job);

private:
	BandPool();
	~BandPool();
	void work(size_t index);

	std::mutex mutex_;
	std::condition_variable start_, done_;
	std::vector<std::thread> threads_;
	const std::function<void(size_t)> *job_ = nullptr;
	size_t pending_ = 0;
	uint64_t generation_ = 0;
	bool quit_ = false;
};

} // namespace wraith
