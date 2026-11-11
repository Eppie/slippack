#pragma once
#include "binary_io.h"
#include <algorithm>
#include <atomic>
#include <cfenv>
#include <exception>
#include <mutex>
#include <thread>

namespace slpz {
// Joining ownership also works on libc++ versions without std::jthread.
class JoiningThread {
  std::thread thread_;

public:
  template <class Fn> explicit JoiningThread(Fn &&fn) : thread_(std::forward<Fn>(fn)) {}
  JoiningThread(JoiningThread &&) noexcept = default;
  JoiningThread(const JoiningThread &) = delete;
  ~JoiningThread() {
    if (thread_.joinable())
      thread_.join();
  }
};
class FailureState {
  std::atomic<bool> stopped_{false};
  std::mutex mutex_;
  std::exception_ptr error_;

public:
  bool stopped() const { return stopped_.load(std::memory_order_relaxed); }
  void capture() noexcept {
    std::lock_guard lock(mutex_);
    if (!error_)
      error_ = std::current_exception();
    stopped_.store(true, std::memory_order_relaxed);
  }
  void rethrow() const {
    if (error_)
      std::rethrow_exception(error_);
  }
};
class RoundingGuard {
  int previous_ = std::fegetround();

public:
  RoundingGuard() {
    require(previous_ != -1 && std::fesetround(FE_TONEAREST) == 0, "cannot set IEEE rounding mode");
  }
  ~RoundingGuard() { std::fesetround(previous_); }
};
inline void validate_threads(int threads) {
  require(threads >= 1 && threads <= 256, "threads must be between 1 and 256");
}
template <class Fn> void run_workers(int threads, Fn work) {
  validate_threads(threads);
  FailureState failure;
  auto worker = [&] {
    try {
      RoundingGuard rounding;
      work(failure);
    } catch (...) {
      failure.capture();
    }
  };
  std::vector<JoiningThread> pool;
  try {
    for (int t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker();
  } catch (...) {
    failure.capture();
  }
  pool.clear(); // joins, including when worker creation failed
  failure.rethrow();
}
template <class Fn> void parallel_for(size_t count, int threads, Fn work) {
  if (!count)
    return;
  std::atomic<size_t> next{0};
  run_workers(int(std::min<size_t>(count, threads)), [&](FailureState &failure) {
    while (!failure.stopped()) {
      auto i = next.fetch_add(1);
      if (i >= count)
        return;
      work(i);
    }
  });
}
} // namespace slpz
