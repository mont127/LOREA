#include "pty_session.hpp"

#include <util.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ocli {

bool g_shared_terminal_active = false;

namespace {

constexpr std::size_t kRingCap = 256 * 1024;

std::mutex g_mutex;
std::condition_variable g_cv;
std::string g_ring;
std::size_t g_total = 0;
int g_master_fd = -1;
pid_t g_child_pid = -1;
bool g_started = false;
std::atomic<bool> g_alive{false};
std::thread g_reader;
std::atomic<unsigned long> g_counter{0};

void reader_loop() {
  std::vector<char> buf(8192);
  for (;;) {
    int fd;
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      fd = g_master_fd;
    }
    if (fd < 0) break;
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n > 0) {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_ring.append(buf.data(), static_cast<std::size_t>(n));
      g_total += static_cast<std::size_t>(n);
      if (g_ring.size() > kRingCap) {
        g_ring.erase(0, g_ring.size() - kRingCap);
      }
      g_cv.notify_all();
    } else if (n == 0) {
      break;
    } else {
      if (errno == EINTR) continue;
      break;
    }
  }
  g_alive.store(false);
  std::lock_guard<std::mutex> lk(g_mutex);
  g_cv.notify_all();
}

bool full_write(int fd, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, data + off, len - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

}  // namespace

PtySession& PtySession::instance() {
  static PtySession s;
  return s;
}

PtySession::~PtySession() { shutdown(); }

bool PtySession::start() {
  std::unique_lock<std::mutex> lk(g_mutex);
  if (g_started) return g_alive.load();

  struct winsize ws;
  std::memset(&ws, 0, sizeof(ws));
  ws.ws_row = 24;
  ws.ws_col = 80;

  int master = -1;
  pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    const char* shell = ::getenv("SHELL");
    if (shell == nullptr || *shell == '\0') shell = "/bin/zsh";
    ::setenv("TERM", "xterm-256color", 1);
    ::execl(shell, shell, "-l", static_cast<char*>(nullptr));
    ::_exit(127);
  }

  g_master_fd = master;
  g_child_pid = pid;
  g_started = true;
  g_alive.store(true);
  g_reader = std::thread(reader_loop);
  g_cv.wait_for(lk, std::chrono::milliseconds(1500), [] { return g_total > 0; });
  for (int i = 0; i < 8; ++i) {
    std::size_t before = g_total;
    g_cv.wait_for(lk, std::chrono::milliseconds(250), [before] { return g_total > before; });
    if (g_total == before) break;
  }
  return true;
}

bool PtySession::alive() { return g_alive.load(); }

void PtySession::write_input(const std::string& bytes) {
  int fd;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    fd = g_master_fd;
  }
  if (fd < 0) return;
  full_write(fd, bytes.data(), bytes.size());
}

std::string PtySession::read_since(std::size_t& cursor) {
  std::unique_lock<std::mutex> lk(g_mutex);
  g_cv.wait_for(lk, std::chrono::milliseconds(200),
                [&] { return g_total > cursor || !g_alive.load(); });
  std::size_t ring_start = g_total - g_ring.size();
  if (cursor < ring_start) cursor = ring_start;
  if (cursor >= g_total) {
    cursor = g_total;
    return std::string();
  }
  std::size_t off = cursor - ring_start;
  std::string out = g_ring.substr(off);
  cursor = g_total;
  return out;
}

std::string PtySession::read_available(std::size_t& cursor) {
  std::lock_guard<std::mutex> lk(g_mutex);
  std::size_t ring_start = g_total - g_ring.size();
  if (cursor < ring_start) cursor = ring_start;
  if (cursor >= g_total) {
    cursor = g_total;
    return std::string();
  }
  std::size_t off = cursor - ring_start;
  std::string out = g_ring.substr(off);
  cursor = g_total;
  return out;
}

std::string PtySession::snapshot() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_ring;
}

void PtySession::resize(int rows, int cols) {
  int fd;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    fd = g_master_fd;
  }
  if (fd < 0) return;
  struct winsize ws;
  std::memset(&ws, 0, sizeof(ws));
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  ::ioctl(fd, TIOCSWINSZ, &ws);
}

std::string PtySession::run_and_capture(const std::string& command, double timeout_s) {
  if (!alive()) {
    if (!start()) return std::string();
  }

  auto strip_ansi = [](const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == 0x1b) {
        if (i + 1 < s.size() && s[i + 1] == '[') {
          i += 2;
          while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
          if (i < s.size()) ++i;
        } else if (i + 1 < s.size() && s[i + 1] == ']') {
          i += 2;
          while (i < s.size() && static_cast<unsigned char>(s[i]) != 0x07 &&
                 static_cast<unsigned char>(s[i]) != 0x1b)
            ++i;
          if (i < s.size() && static_cast<unsigned char>(s[i]) == 0x1b) ++i;
          if (i < s.size()) ++i;
        } else {
          ++i;
        }
      } else if (c == '\r') {
        ++i;
      } else {
        out += s[i++];
      }
    }
    return out;
  };

  unsigned long id = g_counter.fetch_add(1);
  std::string B = "__OCLI_B_" + std::to_string(id) + "__";
  std::string E = "__OCLI_E_" + std::to_string(id) + "__";
  std::string bp = "\n" + B + "\n";
  std::string ep = "\n" + E + "\n";

  std::size_t cursor;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    cursor = g_total;
  }

  write_input("printf '%s\\n' " + B + "; " + command + "; printf '%s\\n' " + E + "\n");

  std::string acc;
  std::string clean;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(timeout_s));

  while (std::chrono::steady_clock::now() < deadline) {
    std::string chunk = read_since(cursor);
    if (!chunk.empty()) {
      acc += chunk;
      clean = strip_ansi(acc);
      if (clean.find(ep) != std::string::npos) break;
    }
    if (!alive()) break;
  }

  std::size_t bpos = clean.find(bp);
  if (bpos == std::string::npos) return std::string();
  std::size_t ostart = bpos + bp.size();
  if (clean.compare(ostart, E.size() + 1, E + "\n") == 0) return std::string();
  std::size_t epos = clean.find(ep, ostart);
  if (epos == std::string::npos) return std::string();
  return clean.substr(ostart, epos - ostart);
}

void PtySession::shutdown() {
  pid_t pid = -1;
  int fd = -1;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_started) return;
    pid = g_child_pid;
    fd = g_master_fd;
    g_master_fd = -1;
    g_child_pid = -1;
    g_started = false;
  }
  g_alive.store(false);
  if (pid > 0) ::kill(pid, SIGHUP);
  if (fd >= 0) ::close(fd);
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_cv.notify_all();
  }
  if (g_reader.joinable()) g_reader.join();
  if (pid > 0) {
    int status = 0;
    ::waitpid(pid, &status, 0);
  }
}

PtySession& terminal_session() {
  PtySession& s = PtySession::instance();
  s.start();
  return s;
}

}  // namespace ocli
