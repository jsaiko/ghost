// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "util/band_pool.hpp"

namespace wraith {

BandPool &BandPool::get() {
	static BandPool pool;
	return pool;
}

BandPool::BandPool() {
	unsigned cores = std::thread::hardware_concurrency();
	size_t n = cores >= 8 ? 3 : (cores >= 4 ? 1 : 0);
	for (size_t i = 0; i < n; i++) {
		threads_.emplace_back([this, i] { work(i + 1); });
	}
}

BandPool::~BandPool() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
	}
	start_.notify_all();
	for (auto &t : threads_) {
		t.join();
	}
}

void BandPool::run(const std::function<void(size_t)> &job) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		job_ = &job;
		pending_ = threads_.size();
		generation_++;
	}
	start_.notify_all();
	job(0);
	std::unique_lock<std::mutex> lock(mutex_);
	done_.wait(lock, [this] { return pending_ == 0; });
	job_ = nullptr;
}

void BandPool::work(size_t index) {
	uint64_t seen = 0;
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		start_.wait(lock, [&] { return quit_ || generation_ != seen; });
		if (quit_) {
			return;
		}
		seen = generation_;
		const std::function<void(size_t)> *job = job_;
		lock.unlock();
		(*job)(index);
		lock.lock();
		if (--pending_ == 0) {
			done_.notify_one();
		}
	}
}

} // namespace wraith
