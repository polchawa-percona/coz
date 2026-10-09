/*
 * Copyright (c) 2015, Charlie Curtsinger and Emery Berger,
 *                     University of Massachusetts Amherst
 * This file is part of the Coz project. See LICENSE.md file at the top-level
 * directory of this distribution and at http://github.com/plasma-umass/coz.
 */

#if !defined(CAUSAL_RUNTIME_PROFILER_H)
#define CAUSAL_RUNTIME_PROFILER_H

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "coz.h"

#include "control.h"

#include "inspect.h"
#include "progress_point.h"
#include "thread_state.h"
#include "util.h"

#include "ccutil/spinlock.h"
#include "ccutil/static_map.h"

/// coz-mcp: number of locks (pthread or COZ_LATCH_*) the current thread holds.
/// initial-exec TLS so that the sampling signal handler can read it safely.
extern __thread int coz_lock_depth __attribute__((tls_model("initial-exec")));

/// Type of a thread entry function
typedef void* (*thread_fn_t)(void*);

#ifdef __APPLE__
// Original pthread_create that bypasses DYLD_INTERPOSE (defined in mac_interpose.cpp)
extern "C" int coz_orig_pthread_create(pthread_t*, const pthread_attr_t*,
                                        void*(*)(void*), void*);
#endif

/// The type of a main function
typedef int (*main_fn_t)(int, char**, char**);

enum {
  SampleSignal = SIGPROF,                                   //< Signal to generate when samples are ready
  SamplePeriod = 1000000,                                   //< Time between samples (1ms)
  SampleBatchSize = 10,                                     //< Samples to batch together for one processing run
  SpeedupDivisions = 20,                                    //< How many different speedups to try (20 = 5% increments)
  ZeroSpeedupWeight = 7,                                    //< Weight of speedup=0 versus other speedup values (7 = ~25% of experiments run with zero speedup)
  ExperimentMinTime = SamplePeriod * SampleBatchSize * 50,  //< Minimum experiment length (500ms)
  ExperimentCoolOffTime = SamplePeriod * SampleBatchSize,   //< Time to wait after an experiment
  ExperimentTargetDelta = 5,                                //< Target minimum number of visits to a progress point during an experiment
  MaxDeferredDelay = 20000000,                              //< coz-mcp: longest virtual delay deferred while a lock is held (20ms)
  SlowdownBacklog = 2000000                                 //< coz-mcp: above this debt, pay 1/64 of it per transition
};

/**
 * Argument type passed to wrapped threads
 */
struct thread_start_arg {
  thread_fn_t _fn;
  void* _arg;
  size_t _parent_delay_time;

  thread_start_arg(thread_fn_t fn, void* arg, size_t t) :
      _fn(fn), _arg(arg), _parent_delay_time(t) {}
};

void init_coz(void);
class profiler {
public:
  /// coz-mcp: catch up on delays only when an experiment runs (avoids gettid())
  void catch_up_if_active() {
    if(_experiment_active.load(std::memory_order_relaxed)) catch_up();
  }

  /// coz-mcp: pay part of this thread's slowdown debt by spinning. inside:
  /// debt for samples taken while holding a lock (call before a release),
  /// otherwise debt for samples taken outside locks (call before an acquire).
  void pay_slowdown_slice(bool inside, size_t segment_ns);

  /// coz-mcp: drop slowdown debt left over from an earlier experiment
  void check_slowdown_epoch(thread_state* state) {
    size_t epoch = _experiment_epoch.load(std::memory_order_relaxed);
    if(state->slowdown_epoch != epoch) {
      state->slowdown_epoch = epoch;
      state->slowdown_debt_in = 0;
      state->slowdown_debt_out = 0;
    }
  }

  /// coz-mcp: CLOCK_MONOTONIC time at which the current experiment started
  size_t experiment_start_mono() const { return _experiment_start_mono.load(std::memory_order_relaxed); }

  /// coz-mcp: is a slowdown experiment running?
  bool slowdown_active() const { return _slowdown_size.load(std::memory_order_relaxed) > 0; }

  /// coz-mcp: perf event that drives sampling ("task-clock", "ref-cycles" or
  /// "cycles"). Call before startup().
  void set_sample_event(const std::string& name);

  /// coz-mcp: defer virtual delay payments while the thread holds a lock
  void set_lock_aware_delays(bool on) { _lock_aware_delays = on; }
  bool lock_aware_delays() const { return _lock_aware_delays; }

  /// coz-mcp: is an experiment running? (cheap check for latch hooks)
  bool experiment_active() const { return _experiment_active.load(std::memory_order_relaxed); }

  /// coz-mcp: enable runtime control over this unix socket (manual mode).
  /// Must be called before startup().
  void set_control_socket(const std::string& path) { _control_socket = path; }

  /// Start the profiler
  void startup(const std::string& outfile,
               line* fixed_line,
               int fixed_speedup,
               bool end_to_end);

  /// Shut down the profiler
  void shutdown();

  /// Get or create a progress point to measure throughput
  throughput_point* get_throughput_point(const std::string& name) {
    // Lock the map of throughput points
    _throughput_points_lock.lock();
  
    // Search for a matching point
    auto search = _throughput_points.find(name);
  
    // If there is no match, add a new throughput point
    if(search == _throughput_points.end()) {
      search = _throughput_points.emplace_hint(search, name, new throughput_point(name));
    }
  
    // Get the matching or inserted value
    throughput_point* result = search->second;
  
    // Unlock the map and return the result
    _throughput_points_lock.unlock();
    return result;
  }
  
  /// Get or create a progress point to measure latency
  latency_point* get_latency_point(const std::string& name) {
    // Lock the map of latency points
    _latency_points_lock.lock();
  
    // Search for a matching point
    auto search = _latency_points.find(name);
  
    // If there is no match, add a new latency point
    if(search == _latency_points.end()) {
      search = _latency_points.emplace_hint(search, name, new latency_point(name));
    }
  
    // Get the matching or inserted value
    latency_point* result = search->second;
  
    // Unlock the map and return the result
    _latency_points_lock.unlock();
    return result;
  }

  /// Pass local delay counts and excess delay time to the child thread
  int handle_pthread_create(pthread_t* thread,
                            const pthread_attr_t* attr,
                            thread_fn_t fn,
                            void* arg) {
    thread_start_arg* new_arg;

    thread_state* state = get_thread_state();
    if (NULL == state) {
      init_coz();
      state = get_thread_state();
    }
    REQUIRE(state) << "Thread state not found";

    // Allocate a struct to pass as an argument to the new thread.
    // On macOS, cap to _global_delay to prevent children from inheriting
    // a stale-high local_delay that would cause them to skip delays.
    size_t parent_delay = state->local_delay.load();
#ifdef __APPLE__
    size_t global = _global_delay.load();
    if(parent_delay > global) parent_delay = global;
#endif
    new_arg = new thread_start_arg(fn, arg, parent_delay);

    // Create a wrapped thread and pass in the wrapped argument
#ifdef __APPLE__
    // On macOS, use the original pthread_create to avoid recursion through
    // DYLD_INTERPOSE (real::pthread_create resolves to the interposed version)
    return coz_orig_pthread_create(thread, attr, profiler::start_thread, new_arg);
#else
    return real::pthread_create(thread, attr, profiler::start_thread, new_arg);
#endif
  }

  /// Force threads to catch up on delays, and stop sampling before the thread exits
  void handle_pthread_exit(void* result) __attribute__((noreturn)) {
    end_sampling();
    // If no more threads being sampled, shut down the profiler
    if (_num_threads_running == 0) {
      shutdown();
    }
    real::pthread_exit(result);
    abort(); // Silence g++ warning about noreturn
  }

  /// Ensure a thread has executed all the required delays before possibly unblocking another thread
  void catch_up() {
    thread_state* state = get_thread_state();

    if(!state)
      return;

    // Handle all samples and add delays as required
    if(_experiment_active) {
      state->set_in_use(true);
#ifndef __APPLE__
      // On Linux, samples accumulate in the per-thread perf_event buffer between
      // timer signals (every 10ms).  Process pending samples so the delay counters
      // are up-to-date before we potentially unblock another thread.
      // process_samples() calls add_delays() at the end.
      process_samples(state);
#else
      // On macOS, samples are processed centrally by the profiler thread.
      add_delays(state);
#endif
      state->set_in_use(false);
    }
  }

  /// Call before (possibly) blocking
  void pre_block() {
    thread_state* state = get_thread_state();
    if(!state)
      return;

    // coz-mcp: only the outermost pair counts (a latch wait may contain an
    // interposed condition variable wait).
    if(state->block_depth++ > 0) return;

    state->is_blocked.store(true);
    state->pre_block_time = _global_delay.load();
  }

  /// Call after unblocking. If by_thread is true, delays will be skipped
  void post_block(bool skip_delays) {
    thread_state* state = get_thread_state();
    if(!state)
      return;

    if(state->block_depth == 0 || --state->block_depth > 0) return;

    state->set_in_use(true);

    if(skip_delays) {
      // Skip all delays that were inserted during the blocked period
      state->local_delay.fetch_add(_global_delay.load() - state->pre_block_time);
    }

    // Must clear is_blocked before process_samples() because add_delays()
    // (called at the end of process_samples()) returns early if is_blocked is true.
    state->is_blocked.store(false);

#ifndef __APPLE__
    // On Linux, process any samples that accumulated while this thread was
    // blocked to bring its delay counters up to date (BCOZ fix).
    if(_experiment_active) {
      process_samples(state);
    }
#endif

    state->set_in_use(false);
  }

  /// Only allow one instance of the profiler, and never run the destructor
  static profiler& get_instance() {
    // alignas is required, not decorative: a bare `char buf[]` has alignment 1,
    // so placement-newing a profiler into it can leave the std::atomic members
    // under-aligned. x86 tolerates that; aarch64 raises SIGBUS on the first
    // atomic store in the constructor.
    alignas(profiler) static char buf[sizeof(profiler)];
    static profiler* p = new(buf) profiler();
    return *p;
  }

private:
  profiler()  {
    _experiment_active.store(false);
    _global_delay.store(0);
    _delay_size.store(0);
    _selected_line.store(nullptr);
    _next_line.store(nullptr);
    _running.store(true);
  }

  // Disallow copy and assignment
  profiler(const profiler&) = delete;
  void operator=(const profiler&) = delete;

  void profiler_thread(spinlock& l);          //< Body of the main profiler thread
  void begin_sampling(thread_state* state);   //< Start sampling in the current thread
  void end_sampling();                        //< Stop sampling in the current thread
  void add_delays(thread_state* state);       //< Add any required delays
  void process_samples(thread_state* state);  //< Process all available samples and insert delays
  void process_all_samples();                 //< Process samples from all threads (for macOS profiler thread)
  void apply_pending_delays();                //< Apply pending delays using Mach thread suspension (macOS)
  std::pair<line*,bool> match_line(perf_event::record&);       //< Map a sample to its source line and matches with selected_line
  void log_samples(std::ofstream&, size_t);   //< Log runtime and sample counts for all identified regions
  void control_loop(std::ofstream& output);   //< coz-mcp: serve the control socket instead of running experiments

  thread_state* add_thread(); //< Add a thread state entry for this thread
  thread_state* get_thread_state(); //< Get a reference to the thread state object for this thread
  void remove_thread(); //< Remove the thread state structure for the current thread

  static void* start_profiler_thread(void*);          //< Entry point for the profiler thread
  static void* start_thread(void* arg);               //< Entry point for wrapped threads
  static void samples_ready(int, siginfo_t*, void*);  //< Signal handler for sample processing
  static void on_error(int, siginfo_t*, void*);       //< Handle errors

  /// A map from name to throughput monitoring progress points
  std::unordered_map<std::string, throughput_point*> _throughput_points;
  spinlock _throughput_points_lock; //< Spinlock that protects the throughput points map
  
  /// A map from name to latency monitoring progress points
  std::unordered_map<std::string, latency_point*> _latency_points;
  spinlock _latency_points_lock;  //< Spinlock that protects the latency points map

  static_map<pid_t, thread_state> _thread_states;   //< Map from thread IDs to thread-local state
  std::atomic<size_t> _num_threads_running;         //< Number of threads that are currently being sampled

  std::atomic<bool> _experiment_active; //< Is an experiment running?
  std::atomic<size_t> _global_delay;    //< The global delay time required
  std::atomic<size_t> _delay_size;      //< The current delay size
  std::atomic<line*> _selected_line;    //< The line to speed up
  std::atomic<line*> _next_line;        //< The next line to speed up

  pthread_t _profiler_thread;     //< Handle for the profiler thread
  std::atomic<bool> _running;     //< Clear to signal the profiler thread to quit
  std::string _output_filename;   //< File for profiler output
  line* _fixed_line;              //< The only line that should be sped up, if set
  int _fixed_delay_size = -1;     //< The only delay size that should be used, if set
  bool _json_output = true;       //< Output in JSON Lines format (default)

  /// Should coz run in end-to-end mode?
  bool _enable_end_to_end;

  /// Atomic flag to guarantee shutdown procedures run exactly one time
  std::atomic_flag _shutdown_run = ATOMIC_FLAG_INIT;

  // coz-mcp runtime control (manual mode).
  friend class control_backend;
  std::string _control_socket;             //< Control socket path; empty = upstream behaviour
  std::atomic<size_t> _slowdown_size{0};   //< Sleep per selected-line sample in a slowdown experiment
  std::atomic<size_t> _slowdown_total{0};  //< Total time slept for slowdown experiments
  bool _lock_aware_delays = false;         //< Defer virtual delays while holding locks
  std::atomic<size_t> _experiment_epoch{0};       //< Incremented at every manual experiment start
  std::atomic<size_t> _experiment_start_mono{0};  //< CLOCK_MONOTONIC at experiment start
  uint32_t _sample_event_type = 1;         //< PERF_TYPE_SOFTWARE
  uint64_t _sample_event_config = 1;       //< PERF_COUNT_SW_TASK_CLOCK
  uint64_t _sample_event_period = SamplePeriod;
  std::string _sample_event_name = "task-clock";
};

#endif
