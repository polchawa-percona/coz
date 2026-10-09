/*
 * Copyright (c) 2015, Charlie Curtsinger and Emery Berger,
 *                     University of Massachusetts Amherst
 * This file is part of the Coz project. See LICENSE.md file at the top-level
 * directory of this distribution and at http://github.com/plasma-umass/coz.
 */

#include <dlfcn.h>
#ifdef __APPLE__
  #include <limits.h>
  #include <mach-o/dyld.h>
#else
  #include <linux/limits.h>
#endif
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <sstream>
#include <string>
#include <unordered_set>

#include "inspect.h"
#include <sys/mman.h>

#include "latch_stats.h"
#include "profiler.h"
#include "progress_point.h"
#include "real.h"
#include "util.h"

#include "ccutil/log.h"

// Include the client header file
#include "coz.h"

using namespace std;

/// The type of a main function
typedef int (*main_fn_t)(int, char**, char**);

/// The program's real main function
main_fn_t real_main;

static bool end_to_end = false;
bool initialized = false;

__thread int coz_lock_depth __attribute__((tls_model("initial-exec"))) = 0;

/*
 * coz-mcp lock-aware delays (COZ_LOCK_AWARE_DELAYS, on by default with
 * COZ_CONTROL_SOCKET). Rules:
 * - before acquiring a lock, catch up: delays and slowdown debt from samples
 *   taken so far are paid outside the new critical section;
 * - while the thread holds a lock, virtual delays are deferred (add_delays);
 *   slowdown sleeps are not, because samples processed then were taken inside
 *   the critical section;
 * - before releasing, catch up again (slowdown sleeps for samples in the
 *   critical section, delay credit published before waiters wake up);
 * - after the last lock is released, pay the deferred delays.
 * With the option off, the pthread wrappers behave exactly like upstream Coz.
 */
/*
 * Slowdown debt is paid in proportion to the code it belongs to: before a
 * release, percent% of the time this lock was held; before an outermost
 * acquire, percent% of the time since the thread last held no lock. Each
 * execution of a slowed critical section thus becomes a little longer, as it
 * would with really slower code, instead of a few executions becoming much
 * longer. The sampled debt still caps the total.
 */
static constexpr int MaxTrackedLocks = 64;
static __thread size_t coz_lock_since[MaxTrackedLocks] __attribute__((tls_model("initial-exec")));
static __thread size_t coz_unlocked_since __attribute__((tls_model("initial-exec"))) = 0;

static inline uint64_t get_monotonic_time() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/// Pay slowdown debt for the code run since the thread last held no lock.
static inline void pay_slowdown_outside(profiler& p) {
  if(coz_lock_depth == 0 && p.slowdown_active()) {
    // Never count code that ran before this experiment started.
    size_t since = coz_unlocked_since;
    if(since < p.experiment_start_mono()) since = p.experiment_start_mono();
    p.pay_slowdown_slice(false, since ? get_monotonic_time() - since : 0);
  }
}

static inline void lock_before_acquire() {
  if(!initialized) return;
  profiler& p = profiler::get_instance();
  if(p.lock_aware_delays() && p.experiment_active()) {
    p.catch_up();
    pay_slowdown_outside(p);
  }
}

static inline void lock_after_acquire() {
  if(coz_lock_depth < MaxTrackedLocks) {
    coz_lock_since[coz_lock_depth] =
        initialized && profiler::get_instance().slowdown_active() ? get_monotonic_time() : 0;
  }
  coz_lock_depth++;
}

static inline void lock_before_release() {
  if(!initialized) return;
  profiler& p = profiler::get_instance();
  if(!p.experiment_active()) return;
  p.catch_up();
  if(p.lock_aware_delays() && p.slowdown_active() && coz_lock_depth > 0 &&
     coz_lock_depth <= MaxTrackedLocks) {
    size_t since = coz_lock_since[coz_lock_depth - 1];
    if(since && since < p.experiment_start_mono()) since = p.experiment_start_mono();
    p.pay_slowdown_slice(true, since ? get_monotonic_time() - since : 0);
  }
}

static inline void lock_after_release(bool pay_deferred) {
  if(coz_lock_depth > 0) coz_lock_depth--;
  if(coz_lock_depth > 0 || !initialized) return;
  profiler& p = profiler::get_instance();
  if(p.slowdown_active()) coz_unlocked_since = get_monotonic_time();
  // Pay virtual delays deferred while the lock was held.
  if(pay_deferred && p.lock_aware_delays()) p.catch_up_if_active();
}
static bool init_in_progress = false;

/**
 * Called by the application at progress points to check and apply delays.
 * This ensures worker threads check their delay debt at progress points,
 * which is critical on macOS where per-thread timers are not available.
 */
extern "C" void _coz_add_delays() {
  if(initialized) {
    profiler::get_instance().catch_up();
  }
}

/**
 * Called by the application before a custom blocking operation.
 * For use with synchronization primitives not intercepted by Coz
 * (e.g., MySQL's custom mutexes, RocksDB internal locks).
 */
extern "C" void _coz_pre_block() {
  if(initialized) profiler::get_instance().pre_block();
}

/**
 * Called by the application after a custom blocking operation completes.
 * If skip_delays is non-zero, delays inserted during the blocked period
 * are skipped (use when woken by another thread).
 */
extern "C" void _coz_post_block(int skip_delays) {
  if(initialized) profiler::get_instance().post_block(skip_delays != 0);
}

/*
 * coz-mcp latch hooks (COZ_LATCH_* in coz.h). Each hook first lets the thread
 * catch up on Coz delays, so slowdown sleeps for samples taken inside a
 * critical section are paid before the latch is released. catch_up() only
 * runs during experiments.
 *
 * Waits can nest: InnoDB acquires a mutex while waiting for another latch
 * (sync array cell reservation). Each pending wait is kept on a small stack;
 * acquired() pops its own wait and any waits above it, which were abandoned
 * (e.g. a nowait attempt that failed).
 */
struct latch_wait {
  const void* latch;
  bool blocked;  // pre_block() was called for this wait
};
static constexpr int MaxLatchWaits = 8;
static __thread latch_wait coz_latch_waits[MaxLatchWaits] __attribute__((tls_model("initial-exec")));
static __thread int coz_latch_wait_depth __attribute__((tls_model("initial-exec"))) = 0;

static inline void latch_end_wait(const void* latch) {
  for(int i = coz_latch_wait_depth - 1; i >= 0; i--) {
    if(coz_latch_waits[i].latch != latch) continue;
    // Abandoned waits above the matched one end without skipping delays.
    for(int j = coz_latch_wait_depth - 1; j >= i; j--) {
      if(coz_latch_waits[j].blocked) profiler::get_instance().post_block(j == i);
    }
    coz_latch_wait_depth = i;
    return;
  }
}

static void latch_wait_begin(const void* latch, const char* file, int line,
                             const char* name, int mode) {
  if(initialized) {
    profiler& p = profiler::get_instance();
    bool blocked = false;
    if(p.experiment_active()) {
      p.catch_up();
      if(p.lock_aware_delays()) {
        pay_slowdown_outside(p);
        // A thread waiting for a latch (spinning or sleeping) is woken by the
        // holder, like a pthread_mutex_lock caller: it must not pay virtual
        // delays inserted while it waited, or it would delay the handoff.
        p.pre_block();
        blocked = true;
      }
    }
    if(coz_latch_wait_depth < MaxLatchWaits) {
      coz_latch_waits[coz_latch_wait_depth++] = {latch, blocked};
    } else if(blocked) {
      p.post_block(false);  // too deep to track: do not stay blocked
    }
  }
  coz_latch::registry& r = coz_latch::registry::instance();
  if(r.enabled()) r.wait_begin(latch, file, line, name, mode, get_monotonic_time());
}

static void latch_acquired(const void* latch) {
  lock_after_acquire();
  if(initialized) latch_end_wait(latch);
  coz_latch::registry& r = coz_latch::registry::instance();
  if(r.enabled()) r.acquired(latch, get_monotonic_time());
}

static void latch_release(const void* latch) {
  // Catch up first: a slowdown paid here is time the latch is really held.
  lock_before_release();
  coz_latch::registry& r = coz_latch::registry::instance();
  if(r.enabled()) r.release(latch, get_monotonic_time());
  // The latch is still held until the caller releases it, so deferred virtual
  // delays are left for the next drain (next acquire or sample batch).
  lock_after_release(false);
}

/*
 * Hand-off latches: the latch is acquired by one thread and released by
 * another, e.g. an InnoDB block latch taken with a pass value for an
 * asynchronous read or write and released by the I/O handler thread when the
 * I/O completes. The acquiring thread does not hold the latch afterwards in
 * any sense that matters to Coz: it does not run a critical section, so its
 * lock depth is not raised (virtual delays are not deferred, slowdown debt is
 * paid as outside a lock) and the releasing thread's depth is not lowered.
 * The wait itself is an ordinary latch wait: the latch may be held by
 * another thread (pre_block()/post_block(true) as for any latch).
 */
static void latch_acquired_handoff(const void* latch) {
  if(initialized) latch_end_wait(latch);
  coz_latch::registry& r = coz_latch::registry::instance();
  if(r.enabled()) r.acquired_handoff(latch, get_monotonic_time());
}

static void latch_release_handoff(const void* latch) {
  // The release may wake threads waiting for the latch: publish this thread's
  // delays first, as before any other release.
  if(initialized) {
    profiler& p = profiler::get_instance();
    if(p.experiment_active()) p.catch_up();
  }
  coz_latch::registry& r = coz_latch::registry::instance();
  if(r.enabled()) r.release_handoff(latch, get_monotonic_time());
}

static void latch_pre_block() {
  _coz_pre_block();
}
static void latch_post_block(int skip_delays) {
  _coz_post_block(skip_delays);
}

/// Lets latch statistics report the lock depth of each thread.
static const int* current_lock_depth() {
  return &coz_lock_depth;
}
static const bool lock_depth_source_set = [] {
  coz_latch::registry::instance().set_lock_depth_source(current_lock_depth);
  return true;
}();

extern "C" coz_latch_api_t* _coz_get_latch_api() {
  static coz_latch_api_t api = {latch_wait_begin, latch_acquired, latch_release};
  return &api;
}

extern "C" coz_latch_api2_t* _coz_get_latch_api2() {
  static coz_latch_api2_t api = {latch_wait_begin, latch_acquired,
                                 latch_release, latch_acquired_handoff,
                                 latch_release_handoff, latch_pre_block,
                                 latch_post_block};
  return &api;
}

#ifdef __APPLE__
/**
 * Helper functions called from mac_interpose.cpp
 * These are defined as extern "C" to allow linking from the interposition code
 * which doesn't include profiler.h to avoid static initialization issues.
 */
extern "C" bool coz_initialized() {
  return initialized;
}

extern "C" int coz_handle_pthread_create(pthread_t* thread,
                                          const pthread_attr_t* attr,
                                          thread_fn_t fn,
                                          void* arg) {
  return profiler::get_instance().handle_pthread_create(thread, attr, fn, arg);
}

extern "C" void coz_handle_pthread_exit(void* result) {
  profiler::get_instance().handle_pthread_exit(result);
}

extern "C" void coz_pre_block() {
  profiler::get_instance().pre_block();
}

extern "C" void coz_post_block(bool skip_delays) {
  profiler::get_instance().post_block(skip_delays);
}

extern "C" void coz_catch_up() {
  profiler::get_instance().catch_up();
}
#endif // __APPLE__

/**
 * Called by the application to get/create a progress point
 */
extern "C" coz_counter_t* _coz_get_counter(progress_point_type t, const char* name) {
  if(t == progress_point_type::throughput) {
    throughput_point* p = profiler::get_instance().get_throughput_point(name);
    if(p) return p->get_counter_struct();
    else return nullptr;
    
  } else if(t == progress_point_type::begin) {
    latency_point* p = profiler::get_instance().get_latency_point(name);
    if(p) return p->get_begin_counter_struct();
    else return nullptr;
    
  } else if(t == progress_point_type::end) {
    latency_point* p = profiler::get_instance().get_latency_point(name);
    if(p) return p->get_end_counter_struct();
    else return nullptr;
    
  } else {
    WARNING << "Unknown progress point type " << ((int)t) << " named " << name;
    return nullptr;
  }
}

/**
 * Read a link's contents and return it as a string
 */
static string readlink_str(const char* path) {
#ifdef __APPLE__
  // macOS: use _NSGetExecutablePath to get the executable path
  uint32_t exe_size = 1024;
  while(true) {
    char exe_path[exe_size];
    if(_NSGetExecutablePath(exe_path, &exe_size) == 0) {
      // Successfully got the path, now resolve it to an absolute path
      char resolved_path[PATH_MAX];
      REQUIRE(realpath(exe_path, resolved_path) != nullptr)
        << "Unable to resolve executable path";
      return string(resolved_path);
    }
    // Buffer too small, exe_size has been updated with required size
  }
#else
  // Linux: use readlink on /proc/self/exe
  size_t exe_size = 1024;
  ssize_t exe_used;

  while(true) {
    char exe_path[exe_size];

    exe_used = readlink(path, exe_path, exe_size - 1);
    REQUIRE(exe_used > 0) << "Unable to read link " << path;

    if(exe_used < exe_size - 1) {
      exe_path[exe_used] = '\0';
      return string(exe_path);
    }

    exe_size += 1024;
  }
#endif
}

/*
 * Initialize coz.  This will either happen as main() is called using
 * __libc_start_main, or on the first call to pthread_create() (if
 * that happen earlier, which might happen if some shared library
 * dependency create a thread in its initializer).
 */
void init_coz(void) {
  if (init_in_progress) {
    VERBOSE << "init_coz in progress, do not recurse";
    return;
  }
  init_in_progress = true;
  VERBOSE << "bootstrapping coz";
  initialized = false;

  // Remove Coz from LD_PRELOAD. Just clearing LD_PRELOAD for now FIXME!
  unsetenv("LD_PRELOAD");

  // Read settings out of environment variables
  string output_file = getenv_safe("COZ_OUTPUT", "profile.coz");

  vector<string> binary_scope_v = split(getenv_safe("COZ_BINARY_SCOPE"), '\t');
  unordered_set<string> binary_scope(binary_scope_v.begin(), binary_scope_v.end());

  vector<string> source_scope_v = split(getenv_safe("COZ_SOURCE_SCOPE"), '\t');
  unordered_set<string> source_scope(source_scope_v.begin(), source_scope_v.end());
  if(source_scope.empty()) {
    source_scope.insert("%");
  }

  vector<string> progress_points_v = split(getenv_safe("COZ_PROGRESS_POINTS"), '\t');
  unordered_set<string> progress_points(progress_points_v.begin(), progress_points_v.end());

  end_to_end = getenv("COZ_END_TO_END");
  string fixed_line_name = getenv_safe("COZ_FIXED_LINE", "");
  int fixed_speedup;
  stringstream(getenv_safe("COZ_FIXED_SPEEDUP", "-1")) >> fixed_speedup;

  // Replace 'MAIN' in the binary_scope with the real path of the main executable
  if(binary_scope.find("MAIN") != binary_scope.end()) {
    binary_scope.erase("MAIN");
#ifdef __APPLE__
    // On macOS, find the main executable by looking for the one with mach_header type MH_EXECUTE
    // _NSGetExecutablePath is the reliable way to get the main executable
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    string main_name;
    if (_NSGetExecutablePath(path, &size) == 0) {
      // Resolve any symlinks
      char real_path[PATH_MAX];
      if (realpath(path, real_path)) {
        main_name = real_path;
      } else {
        main_name = path;
      }
    }
#else
    string main_name = readlink_str("/proc/self/exe");
#endif
    binary_scope.insert(main_name);
    VERBOSE << "Including MAIN, which is " << main_name;
  }

  // Build the memory map for all in-scope binaries
  bool filter_system_sources = getenv("COZ_FILTER_SYSTEM");

  memory_map::get_instance().build(binary_scope, source_scope, !filter_system_sources);

  // Register any sampling progress points
  for(const string& line_name : progress_points) {
    /*shared_ptr<line> l = memory_map::get_instance().find_line(line_name);
    if(l) {
      progress_point* p = new sampling_progress_point(line_name, l);
      profiler::get_instance().sampling_progress_point(p);
    } else {
      WARNING << "Progress line \"" << line_name << "\" was not found.";
    }*/
    FATAL << "Sampling-based progress points are temporarily unsupported";
  }

  shared_ptr<line> fixed_line;
  if(fixed_line_name != "") {
    fixed_line = memory_map::get_instance().find_line(fixed_line_name);
    REQUIRE(fixed_line) << "Fixed line \"" << fixed_line_name << "\" was not found.";
  }

  // Create an end-to-end progress point and register it if running in
  // end-to-end mode
  if(end_to_end) {
    (void)profiler::get_instance().get_throughput_point("end-to-end");
  }

  // coz-mcp: runtime control socket ("%p" expands to the process id)
  string control_socket = getenv_safe("COZ_CONTROL_SOCKET", "");
  if(!control_socket.empty()) {
    string::size_type pos = control_socket.find("%p");
    if(pos != string::npos)
      control_socket.replace(pos, 2, to_string(getpid()));
    profiler::get_instance().set_control_socket(control_socket);
  }
  profiler::get_instance().set_sample_event(
      getenv_safe("COZ_SAMPLE_EVENT", control_socket.empty() ? "task-clock" : "ref-cycles"));
  // coz-mcp: "bank" keeps pause overshoot per thread instead of making every
  // other thread pause for it (upstream "propagate", the default). Opt-in:
  // on the demo it removes the delay inflation but predicts speedups 5-10
  // points below the real change measured under libcoz.
  profiler::get_instance().set_overshoot_mode(
      getenv_safe("COZ_PAUSE_OVERSHOOT", "propagate") == "bank"
          ? profiler::overshoot_mode::bank
          : profiler::overshoot_mode::propagate);
  profiler::get_instance().set_lock_aware_delays(
      getenv_safe("COZ_LOCK_AWARE_DELAYS", control_socket.empty() ? "0" : "1") == "1");

  // Start the profiler
  profiler::get_instance().startup(output_file,
                                   fixed_line.get(),
                                   fixed_speedup,
                                   end_to_end);

  // Synchronizations can be intercepted once the profiler has been initialized
  VERBOSE << "init_coz setting initialized=true";
  initialized = true;
  init_in_progress = false;
  VERBOSE << "init_coz complete, returning...";
}

/**
 * Pass the real __libc_start_main this main function, then run the real main
 * function. This allows Coz to shut down when the real main function returns.
 */
static int wrapped_main(int argc, char** argv, char** env) {
  if (!initialized)
    init_coz();

  // Run the real main function
  int result = real_main(argc, argv, env);

  // Increment the end-to-end progress point just before shutdown
  if(end_to_end) {
    throughput_point* end_point =
      profiler::get_instance().get_throughput_point("end-to-end");
    end_point->visit();
  }

  // Shut down the profiler
  profiler::get_instance().shutdown();

  return result;
}

#ifndef __APPLE__
/**
 * Interpose on the call to __libc_start_main to run before libc constructors.
 * This is Linux/glibc specific - macOS uses different initialization mechanisms.
 */
extern "C" int __libc_start_main(main_fn_t, int, char**, void (*)(), void (*)(), void (*)(), void*) __attribute__((weak, alias("coz_libc_start_main")));

extern "C" int coz_libc_start_main(main_fn_t main_fn, int argc, char** argv,
    void (*init)(), void (*fini)(), void (*rtld_fini)(), void* stack_end) {
  // Find the real __libc_start_main
  auto real_libc_start_main = (decltype(__libc_start_main)*)dlsym(RTLD_NEXT, "__libc_start_main");
  // Save the program's real main function
  real_main = main_fn;
  // Run the real __libc_start_main, but pass in the wrapped main function
  int result = real_libc_start_main(wrapped_main, argc, argv, init, fini, rtld_fini, stack_end);

  return result;
}
#else
/**
 * On macOS, use a constructor to initialize coz when the dylib is loaded.
 * This runs before main() is called.
 */
__attribute__((constructor))
static void coz_init_macos() {
  init_coz();
}

/**
 * On macOS, use a destructor to shutdown coz when the dylib is unloaded.
 */
__attribute__((destructor))
static void coz_shutdown_macos() {
  // Shutdown coz
  profiler::get_instance().shutdown();
}
#endif

/// Remove coz's required signals from a signal mask
static void remove_coz_signals(sigset_t* set) {
  if(sigismember(set, SampleSignal)) {
    sigdelset(set, SampleSignal);
  }
  if(sigismember(set, SIGSEGV)) {
    sigdelset(set, SIGSEGV);
  }
  if(sigismember(set, SIGABRT)) {
    sigdelset(set, SIGABRT);
  }
}

/// Check if a signal is required by coz
static bool is_coz_signal(int signum) {
  return signum == SampleSignal || signum == SIGSEGV || signum == SIGABRT;
}

#ifdef __APPLE__
/// Additional helpers called from mac_interpose.cpp
extern "C" void coz_shutdown() {
  profiler::get_instance().shutdown();
}

extern "C" bool coz_is_coz_signal(int signum) {
  return is_coz_signal(signum);
}

extern "C" void coz_remove_coz_signals(sigset_t* set) {
  remove_coz_signals(set);
}
#endif


#ifndef __APPLE__
/// The smallest alternate signal stack coz's SIGPROF handler can run on.
/// Matches Go's 32 KiB, so Go's own stack is never replaced.
static const size_t CozMinAltStackSize = 32 * 1024;

/// A per-thread replacement signal stack, allocated once and reused. It is
/// deliberately never unmapped: the kernel may still be using it as this
/// thread's alternate stack, and the thread is about to die anyway.
static stack_t* coz_alt_stack() {
  static thread_local stack_t storage;
  static thread_local bool ready = false;

  if(!ready) {
    void* mem = mmap(NULL, CozMinAltStackSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if(mem == MAP_FAILED) return NULL;
    storage.ss_sp = mem;
    storage.ss_size = CozMinAltStackSize;
    storage.ss_flags = 0;
    ready = true;
  }
  return &storage;
}
#endif

extern "C" {
// On macOS, all wrappers are in mac_interpose.cpp using DYLD interposition
#ifndef __APPLE__
  /// Pass pthread_create calls to coz so child threads can inherit the parent's delay count
  int pthread_create(pthread_t* thread,
                     const pthread_attr_t* attr,
                     thread_fn_t fn,
                     void* arg) {
    return profiler::get_instance().handle_pthread_create(thread, attr, fn, arg);
  }

  /// Catch up on delays before exiting, possibly unblocking a thread joining this one
  void __attribute__((noreturn)) pthread_exit(void* result) {
	  profiler::get_instance().handle_pthread_exit(result);
  }

  /// Skip any delays added while waiting to join a thread
  int pthread_join(pthread_t t, void** retval) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_join(t, retval);
    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  int pthread_tryjoin_np(pthread_t t, void** retval) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_tryjoin_np(t, retval);
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  int pthread_timedjoin_np(pthread_t t, void** ret, const struct timespec* abstime) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_timedjoin_np(t, ret, abstime);
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  /// Skip any global delays added while blocked on a mutex
  int pthread_mutex_lock(pthread_mutex_t* mutex) {
    lock_before_acquire();
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_mutex_lock(mutex);
    if(result == 0) lock_after_acquire();
    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  /// coz-mcp: track lock depth for lock-aware delays
  int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    int result = real::pthread_mutex_trylock(mutex);
    if(result == 0) lock_after_acquire();
    return result;
  }

  /// Catch up on delays before unblocking any threads waiting on a mutex
  int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    lock_before_release();
    int result = real::pthread_mutex_unlock(mutex);
    if(result == 0) lock_after_release(true);
    return result;
  }

  /**
   * POSIX semaphores.
   *
   * A thread blocked on a semaphore is not running, so it must not be charged
   * for virtual delays inserted while it slept -- otherwise it pays them all at
   * once on wake-up, and if it is the thread that visits the progress point,
   * every line in the profile acquires a negative slope.
   *
   * glibc implements sem_wait on futex(2) with an inline syscall, so the futex
   * cannot be interposed. sem_wait can: it is an ordinary exported libc symbol.
   * That is the seam, and it covers everything layered on POSIX semaphores,
   * including swift-corelibs-libdispatch's DispatchSemaphore.
   */
  int sem_wait(sem_t* sem) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::sem_wait(sem);
    // Woken by a sem_post from another thread, so skip the delays.
    if(initialized) profiler::get_instance().post_block(true);
    return result;
  }

  int sem_timedwait(sem_t* sem, const struct timespec* abstime) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::sem_timedwait(sem, abstime);
    // On timeout nobody handed us the semaphore, so we own our delays.
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  /// Never blocks, so there is nothing to skip.
  int sem_trywait(sem_t* sem) {
    return real::sem_trywait(sem);
  }

  /// May unblock another thread, so pay outstanding delays before it runs.
  int sem_post(sem_t* sem) {
    if(initialized) profiler::get_instance().catch_up();
    return real::sem_post(sem);
  }

  /**
   * Enforce a floor on the alternate signal stack.
   *
   * coz's SIGPROF handler runs with SA_ONSTACK, which Go requires: it creates
   * its Ms with pthread_create when cgo is in play, so coz samples threads that
   * are running goroutine stacks, and those are small and movable.
   *
   * But SA_ONSTACK means the handler runs on whatever alternate stack the
   * program installed, and process_samples() needs more room than some runtimes
   * provide. Rust's std gives each thread exactly SIGSTKSZ = 8192 bytes. On
   * x86_64 the signal frame and the dynamic linker's xsavec trampoline eat much
   * of that, and the handler overruns it: every Rust program under coz died with
   * SIGSEGV inside process_samples, and the crash reporter could not even print,
   * because fprintf's lazy PLT resolution faulted on the same exhausted stack.
   * aarch64 has a smaller signal frame and did not trip it.
   *
   * So when a program asks for a stack smaller than coz needs, give it a bigger
   * one. Go already asks for 32 KiB, at or above this floor, so its own stack is
   * left exactly as it set it up -- which matters, because the Go runtime checks
   * that a signal arrived on the gsignal stack it knows about.
   */
  int sigaltstack(const stack_t* ss, stack_t* old_ss) {
    if(ss != NULL && (ss->ss_flags & SS_DISABLE) == 0 && ss->ss_size < CozMinAltStackSize) {
      stack_t* mine = coz_alt_stack();
      if(mine != NULL) {
        stack_t enlarged = *ss;
        enlarged.ss_sp = mine->ss_sp;
        enlarged.ss_size = mine->ss_size;
        return real::sigaltstack(&enlarged, old_ss);
      }
    }
    return real::sigaltstack(ss, old_ss);
  }

  /// Skip any delays added while waiting on a condition variable
  int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_cond_wait(cond, mutex);
    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  /**
   * Wait on a condvar for a fixed timeout. If the wait does *not* time out, skip any global
   * delays added during the waiting period.
   */
  int pthread_cond_timedwait(pthread_cond_t* cond,
                             pthread_mutex_t* mutex,
                             const struct timespec* time) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_cond_timedwait(cond, mutex, time);

    // Skip delays only if the wait didn't time out
    if(initialized) profiler::get_instance().post_block(result == 0);

    return result;
  }

  /// Catchup on delays before waking a thread waiting on a condition variable
  int pthread_cond_signal(pthread_cond_t* cond) {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_cond_signal(cond);
  }

  /// Catch up on delays before waking any threads waiting on a condition variable
  int pthread_cond_broadcast(pthread_cond_t* cond) {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_cond_broadcast(cond);
  }

  /// Catch up before, and skip ahead after waking from a barrier
  int pthread_barrier_wait(pthread_barrier_t* barrier) {
    if(initialized) profiler::get_instance().catch_up();
    if(initialized) profiler::get_instance().pre_block();

    int result = real::pthread_barrier_wait(barrier);

    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  int pthread_rwlock_rdlock(pthread_rwlock_t* rwlock) {
    lock_before_acquire();
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_rdlock(rwlock);
    if(result == 0) lock_after_acquire();
    if(initialized) profiler::get_instance().post_block(true);
    return result;
  }

  int pthread_rwlock_timedrdlock(pthread_rwlock_t* rwlock, const struct timespec* abstime) {
    lock_before_acquire();
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_timedrdlock(rwlock, abstime);
    if(result == 0) lock_after_acquire();
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  int pthread_rwlock_wrlock(pthread_rwlock_t* rwlock) {
    lock_before_acquire();
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_wrlock(rwlock);
    if(result == 0) lock_after_acquire();
    if(initialized) profiler::get_instance().post_block(true);
    return result;
  }

  int pthread_rwlock_timedwrlock(pthread_rwlock_t* rwlock, const struct timespec* abstime) {
    lock_before_acquire();
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_timedwrlock(rwlock, abstime);
    if(result == 0) lock_after_acquire();
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  int pthread_rwlock_unlock(pthread_rwlock_t* rwlock) {
    lock_before_release();
    int result = real::pthread_rwlock_unlock(rwlock);
    if(result == 0) lock_after_release(true);
    return result;
  }
#endif // !__APPLE__

#ifndef __APPLE__
  /// Run shutdown before exiting
  void __attribute__((noreturn)) exit(int status) {
    profiler::get_instance().shutdown();
    real::exit(status);
    abort(); // Silence g++ warning about noreturn
  }

  /// Run shutdown before exiting
  void __attribute__((noreturn)) _exit(int status) {
    profiler::get_instance().shutdown();
  	real::_exit(status);
    abort(); // Silence g++ warning about noreturn
  }

  /// Run shutdown before exiting
  void __attribute__((noreturn)) _Exit(int status) {
    profiler::get_instance().shutdown();
    real::_Exit(status);
    abort(); // Silence g++ warning about noreturn
  }

  /// Don't allow programs to set signal handlers for coz's required signals
  sighandler_t signal(int signum, sighandler_t handler) {
    if(is_coz_signal(signum)) {
      return NULL;
    } else {
      return real::signal(signum, handler);
    }
  }

  /// Don't allow programs to set handlers or mask signals required for coz
  int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    if(is_coz_signal(signum)) {
      return 0;
    } else if(act != NULL) {
      struct sigaction my_act = *act;
      remove_coz_signals(&my_act.sa_mask);
      return real::sigaction(signum, &my_act, oldact);
    } else {
      return real::sigaction(signum, act, oldact);
    }
  }

  /// Ensure coz's signals remain unmasked
  int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    if(how == SIG_BLOCK || how == SIG_SETMASK) {
      if(set != NULL) {
        sigset_t myset = *set;
        remove_coz_signals(&myset);
        return real::sigprocmask(how, &myset, oldset);
      }
    }

    return real::sigprocmask(how, set, oldset);
  }

  /// Ensure coz's signals remain unmasked
  int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    if(how == SIG_BLOCK || how == SIG_SETMASK) {
      if(set != NULL) {
        sigset_t myset = *set;
        remove_coz_signals(&myset);

        return real::pthread_sigmask(how, &myset, oldset);
      }
    }

    return real::pthread_sigmask(how, set, oldset);
  }

  /// Catch up on delays before sending a signal to the current process
  int kill(pid_t pid, int sig) {
    if(pid == getpid())
      profiler::get_instance().catch_up();
    return real::kill(pid, sig);
  }

  /// Catch up on delays before sending a signal to another thread
  int pthread_kill(pthread_t thread, int sig) {
    // TODO: Don't allow threads to send coz's signals
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_kill(thread, sig);
  }

  int pthread_sigqueue(pthread_t thread, int sig, const union sigval val) {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_sigqueue(thread, sig, val);
  }

  /**
   * Ensure a thread cannot wait for coz's signals.
   * If the waking signal is delivered from the same process, skip any global delays added
   * while blocked.
   */
  int sigwait(const sigset_t* set, int* sig) {
    sigset_t myset = *set;
    remove_coz_signals(&myset);

    siginfo_t info;

    if(initialized) profiler::get_instance().pre_block();

    int result = real::sigwaitinfo(&myset, &info);

    // Woken up by another thread if the call did not fail, and the waking process is this one
    if(initialized) profiler::get_instance().post_block(result != -1 && info.si_pid == getpid());

    if(result == -1) {
      // If there was an error, return the error code
      return errno;
    } else {
      // No need to check if sig is null because it's declared as non-null
      *sig = result;
      return 0;
    }
  }

  /**
   * Ensure a thread cannot wait for coz's signals.
   * If the waking signal is delivered from the same process, skip any added global delays.
   */
  int sigwaitinfo(const sigset_t* set, siginfo_t* info) {
    sigset_t myset = *set;
    siginfo_t myinfo;
    remove_coz_signals(&myset);

    if(initialized) profiler::get_instance().pre_block();

    int result = real::sigwaitinfo(&myset, &myinfo);

    // Woken up by another thread if the call did not fail, and the waking process is this one
    if(initialized) profiler::get_instance().post_block(result > 0 && myinfo.si_pid == getpid());

    if(result > 0 && info)
      *info = myinfo;

    return result;
  }

  /**
   * Ensure a thread cannot wait for coz's signals.
   * If the waking signal is delivered from the same process, skip any global delays.
   */
  int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout) {
    sigset_t myset = *set;
    siginfo_t myinfo;
    remove_coz_signals(&myset);

    if(initialized) profiler::get_instance().pre_block();

    int result = real::sigtimedwait(&myset, &myinfo, timeout);

    // Woken up by another thread if the call did not fail, and the waking process is this one
    if(initialized) profiler::get_instance().post_block(result > 0 && myinfo.si_pid == getpid());

    if(result > 0 && info)
      *info = myinfo;

    return result;
  }

  /**
   * Set the process signal mask, suspend, then wake and restore the signal mask.
   * If the waking signal is delivered from within the process, skip any added global delays
   */
  int sigsuspend(const sigset_t* set) {
    sigset_t oldset;
    int sig;
    real::sigprocmask(SIG_SETMASK, set, &oldset);
    int rc = sigwait(set, &sig);
    real::sigprocmask(SIG_SETMASK, &oldset, nullptr);
    return rc;
  }
#endif // !__APPLE__
}
