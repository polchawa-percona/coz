/*
 * coz-mcp latch statistics. See latch_stats.h.
 */

#include "latch_stats.h"

#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>

namespace coz_latch {

namespace {

/// Single-writer update: only the owning thread writes, so load+store is enough.
inline void add(std::atomic<uint64_t>& a, uint64_t v) {
  a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

inline void max_of(std::atomic<uint64_t>& a, uint64_t v) {
  if(v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}

const char* mode_name(int mode) {
  switch(mode) {
    case 0:
      return "mutex";
    case 1:
      return "s";
    case 2:
      return "x";
    case 3:
      return "sx";
    default:
      return "unknown";
  }
}

std::string json_escape(const char* s) {
  std::string r;
  for(; *s; s++) {
    if(*s == '"' || *s == '\\') r += '\\';
    if((unsigned char)*s < 0x20) continue;
    r += *s;
  }
  return r;
}

struct site_key {
  const char* file;
  int line;
  const char* name;
  int mode;
  bool operator==(const site_key& o) const {
    return file == o.file && line == o.line && name == o.name && mode == o.mode;
  }
};

struct site_key_hash {
  size_t operator()(const site_key& k) const {
    size_t h = std::hash<const void*>()(k.file);
    h = h * 31 + (size_t)k.line;
    h = h * 31 + std::hash<const void*>()(k.name);
    return h * 31 + (size_t)k.mode;
  }
};

struct held_latch {
  const void* latch;
  site* s;
  uint64_t since;
};

}  // namespace

struct thread_data {
  // Owner thread only.
  std::unordered_map<site_key, site*, site_key_hash> index;
  // Pending waits (nested waits happen, e.g. a mutex taken while waiting for
  // another latch); acquired() pops its wait and abandoned waits above it.
  struct pending_wait {
    const void* latch;
    site* s;
    uint64_t since;
  } pending[8];
  int npending = 0;
  held_latch held[max_held];
  int depth = 0;
  uint64_t enable_epoch = 0;

  // Read by snapshots.
  std::mutex sites_mutex;    // guards the vector, not the counters
  std::vector<site*> sites;  // never freed
  std::atomic<uint64_t> unmatched{0};
  std::atomic<uint64_t> overflows{0};

  site* lookup(const char* file, int line, const char* name, int mode) {
    site_key k{file, line, name, mode};
    auto it = index.find(k);
    if(it != index.end()) return it->second;
    site* s = new site{file, line, name, mode, {}};
    index.emplace(k, s);
    std::lock_guard<std::mutex> g(sites_mutex);
    sites.push_back(s);
    return s;
  }
};

int bucket_of(uint64_t ns) {
  if(ns == 0) return 0;
  int b = 64 - __builtin_clzll(ns);
  return b < hist_buckets ? b : hist_buckets - 1;
}

void counters::reset() {
  for(auto* a : {&count, &wait_ns, &wait_max_ns, &hold_count, &hold_ns, &hold_max_ns})
    a->store(0, std::memory_order_relaxed);
  for(int i = 0; i < hist_buckets; i++) {
    wait_hist[i].store(0, std::memory_order_relaxed);
    hold_hist[i].store(0, std::memory_order_relaxed);
  }
}

registry& registry::instance() {
  // Never destroyed: hooks may run during static destruction.
  static registry* r = new registry();
  return *r;
}

thread_data* registry::local() {
  thread_local thread_data* td = nullptr;
  thread_local uint64_t gen = 0;
  uint64_t g = _generation.load(std::memory_order_relaxed);
  if(td == nullptr || gen != g) {
    td = new thread_data();
    gen = g;
    std::lock_guard<std::mutex> lock(_threads_mutex);
    _threads.push_back(td);
  }
  uint64_t epoch = _enable_epoch.load(std::memory_order_relaxed);
  if(td->enable_epoch != epoch) {
    td->enable_epoch = epoch;
    td->depth = 0;
    td->npending = 0;
  }
  return td;
}

void registry::wait_begin(const void* latch, const char* file, int line,
                          const char* name, int mode, uint64_t now_ns) {
  if(!enabled()) return;
  thread_data* td = local();
  if(td->npending == 8) {
    // Too deep: drop the oldest wait.
    for(int i = 1; i < 8; i++) td->pending[i - 1] = td->pending[i];
    td->npending--;
  }
  td->pending[td->npending++] = {latch, td->lookup(file, line, name, mode), now_ns};
}

void registry::acquired(const void* latch, uint64_t now_ns) {
  if(!enabled()) return;
  thread_data* td = local();
  int i = td->npending - 1;
  while(i >= 0 && td->pending[i].latch != latch) i--;
  if(i < 0) return;
  site* s = td->pending[i].s;
  uint64_t since = td->pending[i].since;
  td->npending = i;

  uint64_t wait = now_ns >= since ? now_ns - since : 0;
  add(s->c.count, 1);
  add(s->c.wait_ns, wait);
  max_of(s->c.wait_max_ns, wait);
  add(s->c.wait_hist[bucket_of(wait)], 1);

  if(td->depth == max_held) {
    add(td->overflows, 1);
    return;
  }
  td->held[td->depth++] = {latch, s, now_ns};
}

void registry::release(const void* latch, uint64_t now_ns) {
  if(!enabled()) return;
  thread_data* td = local();
  for(int i = td->depth - 1; i >= 0; i--) {
    if(td->held[i].latch != latch) continue;
    held_latch h = td->held[i];
    for(int j = i; j + 1 < td->depth; j++) td->held[j] = td->held[j + 1];
    td->depth--;
    uint64_t hold = now_ns >= h.since ? now_ns - h.since : 0;
    add(h.s->c.hold_count, 1);
    add(h.s->c.hold_ns, hold);
    max_of(h.s->c.hold_max_ns, hold);
    add(h.s->c.hold_hist[bucket_of(hold)], 1);
    return;
  }
  add(td->unmatched, 1);
}

void registry::reset() {
  std::lock_guard<std::mutex> lock(_threads_mutex);
  for(thread_data* td : _threads) {
    td->unmatched.store(0, std::memory_order_relaxed);
    td->overflows.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(td->sites_mutex);
    for(site* s : td->sites) s->c.reset();
  }
}

std::string registry::snapshot_json(uint64_t now_ns) {
  struct merged {
    uint64_t v[6] = {};
    uint64_t wait_hist[hist_buckets] = {};
    uint64_t hold_hist[hist_buckets] = {};
  };
  // Sites with the same file:line/name/mode from different threads (or with
  // different string pointers) are merged by their text.
  std::map<std::tuple<std::string, int, std::string, int>, merged> all;
  uint64_t unmatched = 0, overflows = 0;
  {
    std::lock_guard<std::mutex> lock(_threads_mutex);
    for(thread_data* td : _threads) {
      unmatched += td->unmatched.load(std::memory_order_relaxed);
      overflows += td->overflows.load(std::memory_order_relaxed);
      std::lock_guard<std::mutex> g(td->sites_mutex);
      for(site* s : td->sites) {
        merged& m = all[std::make_tuple(std::string(s->file), s->line,
                                        std::string(s->name), s->mode)];
        const counters& c = s->c;
        m.v[0] += c.count.load(std::memory_order_relaxed);
        m.v[1] += c.wait_ns.load(std::memory_order_relaxed);
        uint64_t wmax = c.wait_max_ns.load(std::memory_order_relaxed);
        if(wmax > m.v[2]) m.v[2] = wmax;
        m.v[3] += c.hold_count.load(std::memory_order_relaxed);
        m.v[4] += c.hold_ns.load(std::memory_order_relaxed);
        uint64_t hmax = c.hold_max_ns.load(std::memory_order_relaxed);
        if(hmax > m.v[5]) m.v[5] = hmax;
        for(int i = 0; i < hist_buckets; i++) {
          m.wait_hist[i] += c.wait_hist[i].load(std::memory_order_relaxed);
          m.hold_hist[i] += c.hold_hist[i].load(std::memory_order_relaxed);
        }
      }
    }
  }

  std::string r = "{\"enabled\":";
  r += enabled() ? "true" : "false";
  r += ",\"now_ns\":" + std::to_string(now_ns);
  r += ",\"unmatched_releases\":" + std::to_string(unmatched);
  r += ",\"stack_overflows\":" + std::to_string(overflows);
  r += ",\"latches\":[";
  bool first = true;
  for(const auto& e : all) {
    const merged& m = e.second;
    if(m.v[0] == 0 && m.v[3] == 0) continue;
    if(!first) r += ",";
    first = false;
    r += "{\"site\":\"" + json_escape(std::get<0>(e.first).c_str()) + ":" +
         std::to_string(std::get<1>(e.first)) + "\"";
    r += ",\"name\":\"" + json_escape(std::get<2>(e.first).c_str()) + "\"";
    r += std::string(",\"mode\":\"") + mode_name(std::get<3>(e.first)) + "\"";
    static const char* names[6] = {"count", "wait_ns", "wait_max_ns",
                                   "hold_count", "hold_ns", "hold_max_ns"};
    for(int i = 0; i < 6; i++)
      r += std::string(",\"") + names[i] + "\":" + std::to_string(m.v[i]);
    for(int h = 0; h < 2; h++) {
      r += h == 0 ? ",\"wait_hist\":[" : ",\"hold_hist\":[";
      const uint64_t* hist = h == 0 ? m.wait_hist : m.hold_hist;
      for(int i = 0; i < hist_buckets; i++) {
        if(i > 0) r += ",";
        r += std::to_string(hist[i]);
      }
      r += "]";
    }
    r += "}";
  }
  r += "]}";
  return r;
}

void registry::clear_for_test() {
  std::lock_guard<std::mutex> lock(_threads_mutex);
  _threads.clear();  // leaks on purpose: tests only
  _generation.fetch_add(1, std::memory_order_relaxed);
  _enabled.store(false, std::memory_order_relaxed);
}

}  // namespace coz_latch
