/**
 * Unit tests for the coz-mcp control protocol (libcoz/control.h): JSON
 * parsing, request dispatch to a backend, and the unix socket server.
 */

#include "control.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace coz_control;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                       \
  static void test_##name();             \
  static struct Register_##name {        \
    Register_##name() { test_##name(); } \
  } register_##name;                     \
  static void test_##name()

#define ASSERT_TRUE(expr)                                              \
  do {                                                                 \
    tests_run++;                                                       \
    if (!(expr)) {                                                     \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } else {                                                           \
      tests_passed++;                                                  \
    }                                                                  \
  } while (0)

#define ASSERT_EQ(a, b)                                               \
  do {                                                                \
    tests_run++;                                                      \
    if (!((a) == (b))) {                                              \
      fprintf(stderr, "FAIL: %s:%d: %s == %s\n  got: %s\n", __FILE__, \
              __LINE__, #a, #b, std::string(a).c_str());              \
    } else {                                                          \
      tests_passed++;                                                 \
    }                                                                 \
  } while (0)

/// Records calls and returns canned replies.
struct fake_backend : backend {
  std::string last;
  std::string line;
  experiment_kind kind = experiment_kind::speedup;
  unsigned percent = 0;
  double max_duration = 0;
  size_t min_samples = 0;
  bool enable = false;

  reply status() override {
    last = "status";
    return reply::success("{\"mode\":\"manual\"}");
  }
  reply hot_lines(size_t m) override {
    last = "hot_lines";
    min_samples = m;
    return reply::success("{\"lines\":[]}");
  }
  reply start(const std::string& l, experiment_kind k, unsigned p,
              double d) override {
    last = "start";
    line = l;
    kind = k;
    percent = p;
    max_duration = d;
    if (l == "missing.cc:1") return reply::failure("line not found");
    return reply::success("{\"start_ns\":1}");
  }
  reply stop() override {
    last = "stop";
    return reply::success("{\"end_ns\":2}");
  }
  reply latch_stats(bool e) override {
    last = "latch_stats";
    enable = e;
    return reply::success("{\"enabled\":true}");
  }
  reply latch_stats_reset() override {
    last = "latch_stats_reset";
    return reply::success("{}");
  }
  reply latch_stats_snapshot() override {
    last = "latch_stats_snapshot";
    return reply::success("{\"latches\":[]}");
  }
};

TEST(parse_flat_object) {
  object o;
  std::string err;
  ASSERT_TRUE(parse_object(
      " {\"cmd\": \"start\", \"percent\": 25, \"x\": -1.5e1, \"on\": true,"
      " \"off\": false, \"n\": null, \"s\": \"a\\\"b\\\\c\\n\\u0041\"} ",
      o, err));
  ASSERT_EQ(o["cmd"].str, std::string("start"));
  ASSERT_TRUE(o["percent"].kind == value::number_v && o["percent"].num == 25);
  ASSERT_TRUE(o["x"].num == -15.0);
  ASSERT_TRUE(o["on"].kind == value::bool_v && o["on"].b);
  ASSERT_TRUE(o["off"].kind == value::bool_v && !o["off"].b);
  ASSERT_TRUE(o["n"].kind == value::null_v);
  ASSERT_EQ(o["s"].str, std::string("a\"b\\c\nA"));
}

TEST(parse_rejects_malformed) {
  object o;
  std::string err;
  ASSERT_TRUE(!parse_object("", o, err));
  ASSERT_TRUE(!parse_object("{\"cmd\":", o, err));
  ASSERT_TRUE(!parse_object("[1,2]", o, err));
  ASSERT_TRUE(!parse_object("{\"a\":1} trailing", o, err));
  ASSERT_TRUE(!parse_object("{\"a\":{\"nested\":1}}", o, err));
}

TEST(escape_round_trip) {
  ASSERT_EQ(escape("a\"b\\c\n\t"), std::string("a\\\"b\\\\c\\n\\t"));
}

TEST(dispatch_status) {
  fake_backend b;
  ASSERT_EQ(dispatch("{\"cmd\":\"status\"}", b),
            std::string("{\"mode\":\"manual\"}"));
}

TEST(dispatch_start_with_defaults) {
  fake_backend b;
  std::string r = dispatch(
      "{\"cmd\":\"start\",\"line\":\"demo.cc:55\",\"kind\":\"slowdown\","
      "\"percent\":30}",
      b);
  ASSERT_EQ(r, std::string("{\"start_ns\":1}"));
  ASSERT_EQ(b.line, std::string("demo.cc:55"));
  ASSERT_TRUE(b.kind == experiment_kind::slowdown);
  ASSERT_TRUE(b.percent == 30);
  ASSERT_TRUE(b.max_duration == default_max_duration_s);
}

TEST(dispatch_start_validates_arguments) {
  fake_backend b;
  ASSERT_EQ(
      dispatch("{\"cmd\":\"start\",\"kind\":\"speedup\",\"percent\":5}", b),
      std::string("{\"error\":\"start needs \\\"line\\\" (file:line)\"}"));
  ASSERT_EQ(
      dispatch("{\"cmd\":\"start\",\"line\":\"a.cc:1\",\"kind\":\"faster\","
               "\"percent\":5}",
               b),
      std::string("{\"error\":\"kind must be speedup or slowdown\"}"));
  ASSERT_EQ(
      dispatch("{\"cmd\":\"start\",\"line\":\"a.cc:1\",\"kind\":\"speedup\","
               "\"percent\":101}",
               b),
      std::string("{\"error\":\"percent must be 0..100\"}"));
  ASSERT_EQ(
      dispatch("{\"cmd\":\"start\",\"line\":\"a.cc:1\",\"kind\":\"speedup\","
               "\"percent\":5,\"max_duration_s\":0}",
               b),
      std::string("{\"error\":\"max_duration_s must be > 0\"}"));
  ASSERT_TRUE(b.last != "start");
}

TEST(dispatch_backend_error) {
  fake_backend b;
  ASSERT_EQ(dispatch("{\"cmd\":\"start\",\"line\":\"missing.cc:1\","
                     "\"kind\":\"speedup\",\"percent\":5}",
                     b),
            std::string("{\"error\":\"line not found\"}"));
}

TEST(dispatch_hot_lines_and_latch) {
  fake_backend b;
  dispatch("{\"cmd\":\"hot_lines\",\"min_samples\":7}", b);
  ASSERT_TRUE(b.last == "hot_lines" && b.min_samples == 7);
  dispatch("{\"cmd\":\"hot_lines\"}", b);
  ASSERT_TRUE(b.min_samples == 1);
  dispatch("{\"cmd\":\"latch_stats\",\"enable\":true}", b);
  ASSERT_TRUE(b.last == "latch_stats" && b.enable);
  dispatch("{\"cmd\":\"latch_stats_reset\"}", b);
  ASSERT_TRUE(b.last == "latch_stats_reset");
  dispatch("{\"cmd\":\"latch_stats_snapshot\"}", b);
  ASSERT_TRUE(b.last == "latch_stats_snapshot");
  dispatch("{\"cmd\":\"stop\"}", b);
  ASSERT_TRUE(b.last == "stop");
}

TEST(dispatch_unknown_and_malformed) {
  fake_backend b;
  ASSERT_EQ(dispatch("{\"cmd\":\"fly\"}", b),
            std::string("{\"error\":\"unknown cmd: fly\"}"));
  ASSERT_EQ(dispatch("{\"x\":1}", b),
            std::string("{\"error\":\"missing \\\"cmd\\\"\"}"));
  ASSERT_TRUE(dispatch("not json", b).find("\"error\"") == 1);
}

static int connect_to(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

static std::string read_line(int fd) {
  std::string s;
  char c;
  while (read(fd, &c, 1) == 1 && c != '\n') s += c;
  return s;
}

TEST(server_round_trip) {
  char tmpl[] = "/tmp/coz_control_testXXXXXX";
  char* dir = mkdtemp(tmpl);
  ASSERT_TRUE(dir != nullptr);
  std::string path = std::string(dir) + "/ctl.sock";

  fake_backend b;
  server srv;
  std::string err;
  ASSERT_TRUE(srv.open(path, err));
  struct stat st;
  ASSERT_TRUE(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);

  int fd = connect_to(path);
  ASSERT_TRUE(fd >= 0);
  // Two requests in one write, the second split across writes.
  std::string req = "{\"cmd\":\"status\"}\n{\"cmd\":\"st";
  ASSERT_TRUE(write(fd, req.data(), req.size()) == (ssize_t)req.size());
  std::thread t([&] {
    for (int i = 0; i < 20; i++) srv.poll_once(10, b);
  });
  usleep(30000);
  std::string rest = "op\"}\ngarbage\n";
  ASSERT_TRUE(write(fd, rest.data(), rest.size()) == (ssize_t)rest.size());
  t.join();
  ASSERT_EQ(read_line(fd), std::string("{\"mode\":\"manual\"}"));
  ASSERT_EQ(read_line(fd), std::string("{\"end_ns\":2}"));
  ASSERT_TRUE(read_line(fd).find("\"error\"") == 1);
  ::close(fd);

  // A new client can connect after the first one left.
  srv.poll_once(10, b);
  fd = connect_to(path);
  ASSERT_TRUE(fd >= 0);
  std::string r2 = "{\"cmd\":\"status\"}\n";
  ASSERT_TRUE(write(fd, r2.data(), r2.size()) == (ssize_t)r2.size());
  for (int i = 0; i < 5; i++) srv.poll_once(10, b);
  ASSERT_EQ(read_line(fd), std::string("{\"mode\":\"manual\"}"));
  ::close(fd);

  srv.close();
  ASSERT_TRUE(access(path.c_str(), F_OK) != 0);
  rmdir(dir);
}

int main() {
  printf("%d/%d control tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
