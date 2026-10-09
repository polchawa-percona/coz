/*
 * Runtime control of Coz over a unix socket (coz-mcp). See control.h.
 */

#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace coz_control {

namespace {

class parser {
public:
  explicit parser(const std::string& t)
      : _t(t) {}

  bool parse(object& out, std::string& err) {
    skip_ws();
    if(!expect('{')) return fail(err, "expected '{'");
    skip_ws();
    if(peek() == '}') {
      _p++;
    } else {
      for(;;) {
        skip_ws();
        std::string key;
        if(!parse_string(key)) return fail(err, "expected string key");
        skip_ws();
        if(!expect(':')) return fail(err, "expected ':'");
        skip_ws();
        value v;
        if(!parse_value(v)) return fail(err, "expected scalar value");
        out[key] = v;
        skip_ws();
        if(peek() == ',') {
          _p++;
          continue;
        }
        if(peek() == '}') {
          _p++;
          break;
        }
        return fail(err, "expected ',' or '}'");
      }
    }
    skip_ws();
    if(_p != _t.size()) return fail(err, "trailing characters");
    return true;
  }

private:
  char peek() const { return _p < _t.size() ? _t[_p] : '\0'; }

  bool expect(char c) {
    if(peek() != c) return false;
    _p++;
    return true;
  }

  void skip_ws() {
    while(_p < _t.size() && strchr(" \t\r\n", _t[_p])) _p++;
  }

  bool fail(std::string& err, const char* msg) {
    err = std::string(msg) + " at offset " + std::to_string(_p);
    return false;
  }

  bool literal(const char* word) {
    size_t n = strlen(word);
    if(_t.compare(_p, n, word) != 0) return false;
    _p += n;
    return true;
  }

  bool parse_string(std::string& s) {
    if(!expect('"')) return false;
    while(_p < _t.size()) {
      char c = _t[_p++];
      if(c == '"') return true;
      if(c != '\\') {
        s += c;
        continue;
      }
      if(_p >= _t.size()) return false;
      char e = _t[_p++];
      switch(e) {
        case '"':
          s += '"';
          break;
        case '\\':
          s += '\\';
          break;
        case '/':
          s += '/';
          break;
        case 'b':
          s += '\b';
          break;
        case 'f':
          s += '\f';
          break;
        case 'n':
          s += '\n';
          break;
        case 'r':
          s += '\r';
          break;
        case 't':
          s += '\t';
          break;
        case 'u': {
          if(_p + 4 > _t.size()) return false;
          unsigned cp = strtoul(_t.substr(_p, 4).c_str(), nullptr, 16);
          _p += 4;
          // Requests only carry ASCII (paths, names); keep it simple.
          s += cp < 0x80 ? (char)cp : '?';
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool parse_value(value& v) {
    char c = peek();
    if(c == '"') {
      v.kind = value::string_v;
      return parse_string(v.str);
    }
    if(literal("true")) {
      v.kind = value::bool_v;
      v.b = true;
      return true;
    }
    if(literal("false")) {
      v.kind = value::bool_v;
      v.b = false;
      return true;
    }
    if(literal("null")) {
      v.kind = value::null_v;
      return true;
    }
    if(c == '-' || (c >= '0' && c <= '9')) {
      const char* begin = _t.c_str() + _p;
      char* end = nullptr;
      v.num = strtod(begin, &end);
      if(end == begin) return false;
      _p += end - begin;
      v.kind = value::number_v;
      return true;
    }
    return false;
  }

  const std::string& _t;
  size_t _p = 0;
};

std::string error_json(const std::string& msg) {
  return "{\"error\":\"" + escape(msg) + "\"}";
}

const value* find(const object& o, const char* key) {
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

std::string finish(const reply& r) {
  return r.ok ? r.body : error_json(r.body);
}

std::string dispatch_start(const object& req, backend& b) {
  const value* line = find(req, "line");
  if(!line || line->kind != value::string_v || line->str.empty())
    return error_json("start needs \"line\" (file:line)");

  const value* kind = find(req, "kind");
  experiment_kind k;
  if(kind && kind->kind == value::string_v && kind->str == "speedup") {
    k = experiment_kind::speedup;
  } else if(kind && kind->kind == value::string_v && kind->str == "slowdown") {
    k = experiment_kind::slowdown;
  } else {
    return error_json("kind must be speedup or slowdown");
  }

  const value* pct = find(req, "percent");
  if(!pct || pct->kind != value::number_v || pct->num < 0 || pct->num > 100 ||
     pct->num != std::floor(pct->num))
    return error_json("percent must be 0..100");

  double max_duration = default_max_duration_s;
  if(const value* d = find(req, "max_duration_s")) {
    if(d->kind != value::number_v || !(d->num > 0))
      return error_json("max_duration_s must be > 0");
    max_duration = d->num;
  }
  return finish(b.start(line->str, k, (unsigned)pct->num, max_duration));
}

}  // namespace

bool parse_object(const std::string& text, object& out, std::string& err) {
  out.clear();
  return parser(text).parse(out, err);
}

std::string escape(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for(char c : s) {
    switch(c) {
      case '"':
        r += "\\\"";
        break;
      case '\\':
        r += "\\\\";
        break;
      case '\n':
        r += "\\n";
        break;
      case '\r':
        r += "\\r";
        break;
      case '\t':
        r += "\\t";
        break;
      default:
        if((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          r += buf;
        } else {
          r += c;
        }
    }
  }
  return r;
}

std::string dispatch(const std::string& request, backend& b) {
  object req;
  std::string err;
  if(!parse_object(request, req, err))
    return error_json("bad request: " + err);

  const value* cmd = find(req, "cmd");
  if(!cmd || cmd->kind != value::string_v)
    return error_json("missing \"cmd\"");
  const std::string& c = cmd->str;

  if(c == "status") return finish(b.status());
  if(c == "hot_lines") {
    size_t min_samples = 1;
    if(const value* m = find(req, "min_samples")) {
      if(m->kind != value::number_v || m->num < 0)
        return error_json("min_samples must be >= 0");
      min_samples = (size_t)m->num;
    }
    return finish(b.hot_lines(min_samples));
  }
  if(c == "start") return dispatch_start(req, b);
  if(c == "stop") return finish(b.stop());
  if(c == "latch_stats") {
    const value* e = find(req, "enable");
    if(!e || e->kind != value::bool_v)
      return error_json("latch_stats needs \"enable\" (bool)");
    return finish(b.latch_stats(e->b));
  }
  if(c == "latch_stats_reset") return finish(b.latch_stats_reset());
  if(c == "latch_stats_snapshot") return finish(b.latch_stats_snapshot());
  return error_json("unknown cmd: " + c);
}

server::~server() {
  close();
}

bool server::open(const std::string& path, std::string& err) {
  sockaddr_un addr;
  if(path.size() >= sizeof(addr.sun_path)) {
    err = "socket path too long";
    return false;
  }
  _listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if(_listen_fd < 0) {
    err = std::string("socket: ") + strerror(errno);
    return false;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  unlink(path.c_str());
  // Only the owner may control (and slow down) the profiled process.
  mode_t old_mask = umask(0177);
  int rc = bind(_listen_fd, (sockaddr*)&addr, sizeof(addr));
  umask(old_mask);
  if(rc != 0 || listen(_listen_fd, 4) != 0) {
    err = std::string("bind/listen ") + path + ": " + strerror(errno);
    ::close(_listen_fd);
    _listen_fd = -1;
    return false;
  }
  _path = path;
  return true;
}

void server::close() {
  drop_client();
  if(_listen_fd >= 0) {
    ::close(_listen_fd);
    _listen_fd = -1;
    unlink(_path.c_str());
  }
}

void server::drop_client() {
  if(_client_fd >= 0) {
    ::close(_client_fd);
    _client_fd = -1;
  }
  _inbuf.clear();
}

void server::accept_client() {
  int fd = accept4(_listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if(fd < 0) return;
  if(_client_fd >= 0) {
    // One client at a time: refuse the newcomer with an error line.
    static const char busy[] = "{\"error\":\"another client is connected\"}\n";
    (void)!write(fd, busy, sizeof(busy) - 1);
    ::close(fd);
    return;
  }
  _client_fd = fd;
}

bool server::read_client(backend& b) {
  char buf[4096];
  ssize_t n = read(_client_fd, buf, sizeof(buf));
  if(n <= 0) {
    if(n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
    return false;
  }
  _inbuf.append(buf, n);
  size_t pos;
  while((pos = _inbuf.find('\n')) != std::string::npos) {
    std::string line = _inbuf.substr(0, pos);
    _inbuf.erase(0, pos + 1);
    if(!line.empty() && line.back() == '\r') line.pop_back();
    if(line.empty()) continue;
    std::string resp = dispatch(line, b) + "\n";
    const char* p = resp.data();
    size_t left = resp.size();
    while(left > 0) {
      ssize_t w = write(_client_fd, p, left);
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) return false;
      p += w;
      left -= w;
    }
  }
  if(_inbuf.size() > (1 << 20)) return false;  // runaway client
  return true;
}

void server::poll_once(int timeout_ms, backend& b) {
  if(_listen_fd < 0) return;
  pollfd fds[2];
  int n = 0;
  fds[n++] = {_listen_fd, POLLIN, 0};
  if(_client_fd >= 0) fds[n++] = {_client_fd, POLLIN, 0};
  int rc = poll(fds, n, timeout_ms);
  if(rc <= 0) return;
  if(n > 1 && fds[1].revents) {
    if(!read_client(b)) drop_client();
  }
  if(fds[0].revents & POLLIN) accept_client();
}

}  // namespace coz_control
