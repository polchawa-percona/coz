/**
 * Unit tests for coz-mcp latch statistics (libcoz/latch_stats.h).
 */

#include "latch_stats.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace coz_latch;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                            \
  static void test_##name();                  \
  static struct Register_##name {             \
    Register_##name() {                       \
      registry::instance().clear_for_test();  \
      registry::instance().set_enabled(true); \
      test_##name();                          \
    }                                         \
  } register_##name;                          \
  static void test_##name()

#define ASSERT_TRUE(expr)                                              \
  do {                                                                 \
    tests_run++;                                                       \
    if(!(expr)) {                                                      \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } else {                                                           \
      tests_passed++;                                                  \
    }                                                                  \
  } while(0)

static const char* FILE_A = "/src/a.cc";
static const char* FILE_B = "/src/b.cc";
static const char* NAME_M = "trx_sys_mutex";
static const char* NAME_L = "index_tree_rw_lock";

/// Extract the JSON object of the site "file:line" from a snapshot.
static std::string site_json(const std::string& snap, const std::string& loc,
                             const std::string& mode) {
  std::string key = "\"site\":\"" + loc + "\"";
  size_t pos = 0;
  while((pos = snap.find(key, pos)) != std::string::npos) {
    size_t begin = snap.rfind('{', pos);
    size_t end = snap.find('}', pos);
    std::string obj = snap.substr(begin, end - begin + 1);
    if(obj.find("\"mode\":\"" + mode + "\"") != std::string::npos) return obj;
    pos = end;
  }
  return "";
}

static long long field(const std::string& obj, const std::string& name) {
  size_t p = obj.find("\"" + name + "\":");
  if(p == std::string::npos) return -1;
  return atoll(obj.c_str() + p + name.size() + 3);
}

TEST(bucket_boundaries) {
  ASSERT_TRUE(bucket_of(0) == 0);
  ASSERT_TRUE(bucket_of(1) == 1);
  ASSERT_TRUE(bucket_of(2) == 2);
  ASSERT_TRUE(bucket_of(3) == 2);
  ASSERT_TRUE(bucket_of(1024) == 11);
  ASSERT_TRUE(bucket_of(~0ULL) == hist_buckets - 1);
}

TEST(wait_and_hold_recorded) {
  registry& r = registry::instance();
  int latch;
  r.wait_begin(&latch, FILE_A, 10, NAME_M, 0, 1000);
  r.acquired(&latch, 1300);
  r.release(&latch, 2300);
  r.wait_begin(&latch, FILE_A, 10, NAME_M, 0, 5000);
  r.acquired(&latch, 5100);
  r.release(&latch, 5600);
  std::string o = site_json(r.snapshot_json(9999), "/src/a.cc:10", "mutex");
  ASSERT_TRUE(!o.empty());
  ASSERT_TRUE(o.find("\"name\":\"trx_sys_mutex\"") != std::string::npos);
  ASSERT_TRUE(field(o, "count") == 2);
  ASSERT_TRUE(field(o, "wait_ns") == 400);
  ASSERT_TRUE(field(o, "wait_max_ns") == 300);
  ASSERT_TRUE(field(o, "hold_count") == 2);
  ASSERT_TRUE(field(o, "hold_ns") == 1500);
  ASSERT_TRUE(field(o, "hold_max_ns") == 1000);
}

TEST(nested_and_out_of_order_release) {
  registry& r = registry::instance();
  int a, b;
  r.wait_begin(&a, FILE_A, 1, NAME_M, 0, 0);
  r.acquired(&a, 0);
  r.wait_begin(&b, FILE_B, 2, NAME_L, 2, 100);
  r.acquired(&b, 100);
  r.release(&a, 1000);  // released before b
  r.release(&b, 1100);
  std::string snap = r.snapshot_json(2000);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:1", "mutex"), "hold_ns") == 1000);
  ASSERT_TRUE(field(site_json(snap, "/src/b.cc:2", "x"), "hold_ns") == 1000);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 0);
}

TEST(shared_holders_same_latch_different_threads) {
  registry& r = registry::instance();
  static int latch;
  std::thread t1([&] {
    r.wait_begin(&latch, FILE_A, 5, NAME_L, 1, 0);
    r.acquired(&latch, 10);
    r.release(&latch, 110);
  });
  std::thread t2([&] {
    r.wait_begin(&latch, FILE_A, 5, NAME_L, 1, 0);
    r.acquired(&latch, 20);
    r.release(&latch, 320);
  });
  t1.join();
  t2.join();
  std::string o = site_json(r.snapshot_json(1000), "/src/a.cc:5", "s");
  ASSERT_TRUE(field(o, "count") == 2);
  ASSERT_TRUE(field(o, "wait_ns") == 30);
  ASSERT_TRUE(field(o, "hold_ns") == 400);
}

TEST(unmatched_release_and_failed_trylock) {
  registry& r = registry::instance();
  int a, b;
  r.release(&a, 50);                          // released by a thread that never acquired it
  r.wait_begin(&b, FILE_A, 7, NAME_M, 0, 0);  // trylock failed: no acquired()
  r.wait_begin(&a, FILE_A, 8, NAME_M, 0, 100);
  r.acquired(&a, 150);
  r.release(&a, 250);
  std::string snap = r.snapshot_json(1000);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 1);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:7", "mutex"), "count") <= 0);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:8", "mutex"), "wait_ns") == 50);
}

TEST(stack_overflow_is_counted) {
  registry& r = registry::instance();
  static int latches[max_held + 3];
  for(int i = 0; i < max_held + 3; i++) {
    r.wait_begin(&latches[i], FILE_A, 30, NAME_M, 0, i);
    r.acquired(&latches[i], i);
  }
  for(int i = max_held + 2; i >= 0; i--) r.release(&latches[i], 1000);
  std::string snap = r.snapshot_json(2000);
  ASSERT_TRUE(field(snap, "stack_overflows") == 3);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 3);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:30", "mutex"), "hold_count") == max_held);
}

TEST(disabled_records_nothing) {
  registry& r = registry::instance();
  r.set_enabled(false);
  int a;
  r.wait_begin(&a, FILE_A, 40, NAME_M, 0, 0);
  r.acquired(&a, 10);
  r.release(&a, 20);
  std::string snap = r.snapshot_json(100);
  ASSERT_TRUE(snap.find("/src/a.cc:40") == std::string::npos);
  ASSERT_TRUE(snap.find("\"enabled\":false") != std::string::npos);
}

TEST(reset_zeroes_counters) {
  registry& r = registry::instance();
  int a;
  r.wait_begin(&a, FILE_A, 50, NAME_M, 0, 0);
  r.acquired(&a, 10);
  r.release(&a, 20);
  r.reset();
  std::string o = site_json(r.snapshot_json(100), "/src/a.cc:50", "mutex");
  ASSERT_TRUE(o.empty() || field(o, "count") == 0);
}

TEST(histogram_in_snapshot) {
  registry& r = registry::instance();
  int a;
  r.wait_begin(&a, FILE_A, 60, NAME_M, 0, 0);
  r.acquired(&a, 1024);     // wait bucket 11
  r.release(&a, 1024 + 3);  // hold bucket 2
  std::string o = site_json(r.snapshot_json(5000), "/src/a.cc:60", "mutex");
  ASSERT_TRUE(o.find("\"wait_hist\":[0,0,0,0,0,0,0,0,0,0,0,1,") != std::string::npos);
  ASSERT_TRUE(o.find("\"hold_hist\":[0,0,1,") != std::string::npos);
}

TEST(reenable_drops_stale_held_latches) {
  registry& r = registry::instance();
  int a;
  r.wait_begin(&a, FILE_A, 70, NAME_M, 0, 0);
  r.acquired(&a, 0);  // held when stats are turned off
  r.set_enabled(false);
  r.set_enabled(true);
  r.release(&a, 1000000);  // stale entry dropped: counted as unmatched
  std::string snap = r.snapshot_json(2000000);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:70", "mutex"), "hold_count") <= 0);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 1);
}

TEST(nested_wait_keeps_outer_wait) {
  registry& r = registry::instance();
  int outer, inner;
  r.wait_begin(&outer, FILE_A, 80, NAME_L, 2, 0);
  r.wait_begin(&inner, FILE_B, 81, NAME_M, 0, 100);  // e.g. sync array mutex
  r.acquired(&inner, 150);
  r.release(&inner, 200);
  r.acquired(&outer, 1000);
  r.release(&outer, 1500);
  std::string snap = r.snapshot_json(5000);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:80", "x"), "wait_ns") == 1000);
  ASSERT_TRUE(field(site_json(snap, "/src/b.cc:81", "mutex"), "wait_ns") == 50);
}

TEST(handoff_release_on_other_thread) {
  // InnoDB page read: the reading thread X-latches the block with a pass value
  // and an I/O handler thread releases it when the read completes.
  registry& r = registry::instance();
  static int latch;
  std::thread reader([&] {
    r.wait_begin(&latch, FILE_A, 90, NAME_L, 2, 0);
    r.acquired_handoff(&latch, 100);
  });
  reader.join();
  std::thread io([&] { r.release_handoff(&latch, 5000); });
  io.join();
  std::string snap = r.snapshot_json(10000);
  std::string o = site_json(snap, "/src/a.cc:90", "x");
  ASSERT_TRUE(field(o, "count") == 1);
  ASSERT_TRUE(field(o, "wait_ns") == 100);
  ASSERT_TRUE(field(o, "hold_count") == 0);  // I/O time is not a hold
  ASSERT_TRUE(field(snap, "unmatched_releases") == 0);
  ASSERT_TRUE(field(snap, "stack_overflows") == 0);
  ASSERT_TRUE(field(snap, "handoff_acquires") == 1);
  ASSERT_TRUE(field(snap, "handoff_releases") == 1);
  ASSERT_TRUE(field(snap, "held_now") == 0);
}

TEST(handoff_does_not_fill_held_stack) {
  // A page cleaner flushes many pages: each SX latch taken with a pass value
  // is released by an I/O thread and must not stay on the cleaner's stack.
  registry& r = registry::instance();
  static int pages[max_held + 10];
  int mine;
  r.wait_begin(&mine, FILE_B, 91, NAME_M, 0, 0);
  r.acquired(&mine, 0);
  for(int i = 0; i < max_held + 10; i++) {
    r.wait_begin(&pages[i], FILE_A, 92, NAME_L, 3, i);
    r.acquired_handoff(&pages[i], i);
  }
  std::thread io([&] {
    for(int i = 0; i < max_held + 10; i++) r.release_handoff(&pages[i], 1000);
  });
  io.join();
  r.release(&mine, 2000);
  std::string snap = r.snapshot_json(3000);
  ASSERT_TRUE(field(snap, "stack_overflows") == 0);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 0);
  ASSERT_TRUE(field(snap, "held_now") == 0);
  ASSERT_TRUE(field(site_json(snap, "/src/b.cc:91", "mutex"), "hold_ns") == 2000);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:92", "sx"), "count") == max_held + 10);
}

TEST(handoff_same_thread_balanced) {
  // Synchronous page read: the same thread acquires and releases.
  registry& r = registry::instance();
  int outer, page;
  r.wait_begin(&outer, FILE_B, 93, NAME_M, 0, 0);
  r.acquired(&outer, 0);
  r.wait_begin(&page, FILE_A, 94, NAME_L, 2, 10);
  r.acquired_handoff(&page, 20);
  r.release_handoff(&page, 500);
  r.release(&outer, 700);
  std::string snap = r.snapshot_json(1000);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 0);
  ASSERT_TRUE(field(snap, "held_now") == 0);
  ASSERT_TRUE(field(site_json(snap, "/src/b.cc:93", "mutex"), "hold_ns") == 700);
  ASSERT_TRUE(field(site_json(snap, "/src/a.cc:94", "x"), "wait_ns") == 10);
}

TEST(normal_cross_thread_release_is_still_unmatched) {
  // Without the hand-off API the acquiring thread keeps the latch on its stack.
  registry& r = registry::instance();
  static int latch;
  std::thread reader([&] {
    r.wait_begin(&latch, FILE_A, 95, NAME_L, 2, 0);
    r.acquired(&latch, 100);
  });
  reader.join();
  r.release(&latch, 5000);
  std::string snap = r.snapshot_json(10000);
  ASSERT_TRUE(field(snap, "unmatched_releases") == 1);
  ASSERT_TRUE(field(snap, "held_now") == 1);
}

static thread_local int test_lock_depth = 0;
static const int* test_lock_depth_source() {
  return &test_lock_depth;
}

TEST(lock_depth_of_live_threads) {
  registry& r = registry::instance();
  r.set_lock_depth_source(test_lock_depth_source);
  std::atomic<int> step{0};
  int latch;
  std::thread t([&] {
    test_lock_depth = 3;
    r.wait_begin(&latch, FILE_A, 96, NAME_M, 0, 0);  // registers the thread
    step = 1;
    while(step != 2) std::this_thread::yield();
    test_lock_depth = 0;
    step = 3;
    while(step != 4) std::this_thread::yield();
  });
  while(step != 1) std::this_thread::yield();
  std::string snap = r.snapshot_json(100);
  ASSERT_TRUE(field(snap, "lock_depth_raised") == 1);
  ASSERT_TRUE(field(snap, "lock_depth_max") == 3);
  step = 2;
  while(step != 3) std::this_thread::yield();
  snap = r.snapshot_json(200);
  ASSERT_TRUE(field(snap, "lock_depth_raised") == 0);
  ASSERT_TRUE(field(snap, "lock_depth_threads") >= 1);
  step = 4;
  t.join();
  // An exited thread is no longer read (its TLS is gone).
  snap = r.snapshot_json(300);
  ASSERT_TRUE(field(snap, "lock_depth_threads") == 0);
  r.set_lock_depth_source(nullptr);
}

int main() {
  printf("%d/%d latch stats tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
