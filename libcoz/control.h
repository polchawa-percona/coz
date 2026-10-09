/*
 * Runtime control of Coz over a unix socket (coz-mcp).
 *
 * Protocol: one JSON object per line from the client, one JSON object per line
 * back. Every request has a "cmd" field. Errors are {"error": "..."} and leave
 * the connection usable.
 *
 * This header has no dependency on the profiler so that the protocol can be
 * unit tested with a fake backend.
 */

#ifndef COZ_CONTROL_H
#define COZ_CONTROL_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace coz_control {

/// A scalar JSON value (requests are flat objects).
struct value {
  enum kind_t { null_v, bool_v, number_v, string_v } kind = null_v;
  bool b = false;
  double num = 0;
  std::string str;
};

using object = std::map<std::string, value>;

/// Parse a flat JSON object. Returns false and sets err on malformed input.
bool parse_object(const std::string& text, object& out, std::string& err);

/// Escape a string for embedding in JSON (without the surrounding quotes).
std::string escape(const std::string& s);

/// Result of a backend operation: a JSON object body, or an error message.
struct reply {
  bool ok;
  std::string body;  // JSON object when ok, error message otherwise
  static reply success(std::string json) { return {true, std::move(json)}; }
  static reply failure(std::string msg) { return {false, std::move(msg)}; }
};

enum class experiment_kind { speedup, slowdown };

/// Operations the control protocol can invoke. Implemented by the profiler.
class backend {
 public:
  virtual ~backend() = default;
  virtual reply status() = 0;
  virtual reply hot_lines(size_t min_samples) = 0;
  virtual reply start(const std::string& line, experiment_kind kind,
                      unsigned percent, double max_duration_s) = 0;
  virtual reply stop() = 0;
  virtual reply latch_stats(bool enable) = 0;
  virtual reply latch_stats_reset() = 0;
  virtual reply latch_stats_snapshot() = 0;
};

/// Default auto-stop for experiments, seconds.
constexpr double default_max_duration_s = 120.0;

/// Handle one request line, return one response line (without newline).
std::string dispatch(const std::string& request, backend& b);

/// Unix socket server; single client at a time, line-oriented.
class server {
 public:
  server() = default;
  ~server();
  server(const server&) = delete;
  server& operator=(const server&) = delete;

  /// Bind and listen. Removes a stale socket file first. Mode 0600.
  bool open(const std::string& path, std::string& err);
  /// Wait up to timeout_ms for activity, then handle complete request lines.
  void poll_once(int timeout_ms, backend& b);
  void close();

 private:
  void accept_client();
  void drop_client();
  bool read_client(backend& b);

  std::string _path;
  int _listen_fd = -1;
  int _client_fd = -1;
  std::string _inbuf;
};

}  // namespace coz_control

#endif
