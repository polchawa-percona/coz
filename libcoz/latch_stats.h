/*
 * coz-mcp latch statistics: wait and hold time per acquire site.
 *
 * A site is (file, line, latch name, mode) as passed to COZ_LATCH_WAIT_BEGIN.
 * Each thread keeps its own counters (single writer, relaxed atomics) so the
 * hot path never touches a shared cache line; snapshots sum all threads.
 * Times are passed in by the caller (nanoseconds, monotonic) so the logic can
 * be tested without a clock.
 */

#ifndef COZ_LATCH_STATS_H
#define COZ_LATCH_STATS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace coz_latch {

/// log2 buckets of nanoseconds: bucket i holds values in [2^(i-1), 2^i).
constexpr int hist_buckets = 40;

/// Maximum latches one thread can hold at once before releases are lost.
constexpr int max_held = 64;

int bucket_of(uint64_t ns);

struct counters {
  std::atomic<uint64_t> count{0};  // acquisitions with a measured wait
  std::atomic<uint64_t> wait_ns{0};
  std::atomic<uint64_t> wait_max_ns{0};
  std::atomic<uint64_t> hold_count{0};
  std::atomic<uint64_t> hold_ns{0};
  std::atomic<uint64_t> hold_max_ns{0};
  std::atomic<uint64_t> wait_hist[hist_buckets] = {};
  std::atomic<uint64_t> hold_hist[hist_buckets] = {};

  void reset();
};

struct site {
  const char* file;
  int line;
  const char* name;
  int mode;
  counters c;
};

struct thread_data;

class registry {
public:
  static registry& instance();

  void set_enabled(bool on) { _enabled.store(on, std::memory_order_relaxed); }
  bool enabled() const { return _enabled.load(std::memory_order_relaxed); }

  /// Hot path, called by the thread that acquires/releases the latch.
  void wait_begin(const void* latch, const char* file, int line,
                  const char* name, int mode, uint64_t now_ns);
  void acquired(const void* latch, uint64_t now_ns);
  void release(const void* latch, uint64_t now_ns);

  /// Zero all counters (races with concurrent updates are tolerated).
  void reset();

  /// All sites with their counters as a JSON object.
  std::string snapshot_json(uint64_t now_ns);

  /// Drop all thread data (tests only; no thread may use the registry).
  void clear_for_test();

private:
  thread_data* local();

  std::atomic<bool> _enabled{false};
  std::mutex _threads_mutex;
  std::vector<thread_data*> _threads;    // never freed: stats outlive threads
  std::atomic<uint64_t> _generation{0};  // bumped by clear_for_test
};

}  // namespace coz_latch

#endif
