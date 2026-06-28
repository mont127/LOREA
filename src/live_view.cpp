#include "live_view.hpp"
#include "pty_session.hpp"

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ocli {

namespace {

const char* GREEN_BG = "\033[48;2;104;215;138m";
const char* DARK_FG = "\033[38;2;9;28;16m";
const char* DARK_DIM_FG = "\033[38;2;36;92;52m";
const char* GREEN_DIM_FG = "\033[38;2;74;126;82m";
const char* NEON_FG = "\033[38;2;128;236;150m";
const char* CYAN_FG = "\033[38;2;56;220;200m";
const char* BOLD = "\033[1m";
const char* RESET = "\033[0m";

std::string lerp_color(int r0, int g0, int b0, int r1, int g1, int b1, int t, int span) {
  if (span < 1) span = 1;
  if (t < 0) t = 0;
  if (t > span) t = span;
  int r = r0 + (r1 - r0) * t / span;
  int g = g0 + (g1 - g0) * t / span;
  int b = b0 + (b1 - b0) * t / span;
  return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string pulse_on_green(unsigned long f) {
  int phase = static_cast<int>(f % 50);
  int t = phase < 25 ? phase : 50 - phase;
  return lerp_color(18, 44, 28, 232, 255, 238, t, 25);
}

std::string pulse_neon(unsigned long f) {
  int phase = static_cast<int>(f % 56);
  int t = phase < 28 ? phase : 56 - phase;
  return lerp_color(96, 226, 140, 56, 220, 220, t, 28);
}

bool blink_on(unsigned long f) { return (f % 18) < 11; }

std::mutex g_conv_mutex;
std::string g_conv;

std::mutex g_input_mutex;
std::string g_input;
std::size_t g_input_cursor = 0;
bool g_term_focus = false;
int g_scroll = 0;
int g_term_scroll = 0;
int g_term_x = 1000000;
int g_term_y = -1;

std::mutex g_status_mutex;
std::string g_status_line;

bool g_terminal_used = false;
std::size_t g_ctx_cursor = 0;

std::atomic<bool> g_active{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_mouse_want{false};
bool g_mouse_on = false;
bool g_suspended = false;
unsigned long g_frame = 0;

int g_real_fd = -1;
int g_saved_stdout = -1;
int g_pipe_r = -1;
int g_pipe_w = -1;

std::thread g_reader;
std::thread g_renderer;

struct termios g_saved_termios;
bool g_termios_saved = false;

std::function<std::string()> g_status_fn;

constexpr std::size_t kConvCap = 200 * 1024;

bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

void full_write(int fd, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, data + off, len - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
}

int vis_width(const std::string& s) {
  int w = 0;
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
    } else if (is_continuation(c)) {
      ++i;
    } else {
      ++w;
      ++i;
    }
  }
  return w;
}

std::string strip_ansi(const std::string& s) {
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
    } else if (c == '\t') {
      out += "  ";
      ++i;
    } else if (c < 0x20 && c != '\n') {
      ++i;
    } else {
      out += s[i++];
    }
  }
  return out;
}

std::string keep_sgr(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0x1b) {
      if (i + 1 < s.size() && s[i + 1] == '[') {
        std::size_t j = i + 2;
        while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
        if (j < s.size()) {
          if (s[j] == 'm') out.append(s, i, j - i + 1);
          i = j + 1;
        } else {
          i = s.size();
        }
      } else if (i + 1 < s.size() && s[i + 1] == ']') {
        std::size_t j = i + 2;
        while (j < s.size() && static_cast<unsigned char>(s[j]) != 0x07 &&
               static_cast<unsigned char>(s[j]) != 0x1b)
          ++j;
        if (j < s.size() && static_cast<unsigned char>(s[j]) == 0x1b) ++j;
        if (j < s.size()) ++j;
        i = j;
      } else {
        ++i;
      }
    } else if (c == '\t') {
      out += "  ";
      ++i;
    } else if (c < 0x20 && c != '\n' && c != '\r') {
      ++i;
    } else {
      out += s[i++];
    }
  }
  return out;
}

std::string fit_ansi(const std::string& s, int width) {
  std::string out;
  int w = 0;
  for (std::size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0x1b) {
      std::size_t start = i;
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
      out.append(s, start, i - start);
    } else if (is_continuation(c)) {
      out += s[i++];
    } else {
      if (w >= width) break;
      std::size_t start = i;
      ++i;
      while (i < s.size() && is_continuation(static_cast<unsigned char>(s[i]))) ++i;
      out.append(s, start, i - start);
      ++w;
    }
  }
  out += RESET;
  while (w < width) {
    out += ' ';
    ++w;
  }
  return out;
}

std::string fit_plain(const std::string& raw, int width) {
  std::string s = strip_ansi(raw);
  std::string out;
  int w = 0;
  for (std::size_t i = 0; i < s.size() && w < width;) {
    std::size_t start = i;
    ++i;
    while (i < s.size() && is_continuation(static_cast<unsigned char>(s[i]))) ++i;
    out.append(s, start, i - start);
    ++w;
  }
  while (w < width) {
    out += ' ';
    ++w;
  }
  return out;
}

void wrap_into(const std::string& line, int width, std::vector<std::string>& out) {
  if (width < 1) width = 1;
  if (vis_width(line) <= width) {
    out.push_back(line);
    return;
  }
  std::string cur;
  int w = 0;
  for (std::size_t i = 0; i < line.size();) {
    unsigned char c = static_cast<unsigned char>(line[i]);
    if (c == 0x1b) {
      std::size_t start = i;
      if (i + 1 < line.size() && line[i + 1] == '[') {
        i += 2;
        while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) ++i;
        if (i < line.size()) ++i;
      } else {
        ++i;
      }
      cur.append(line, start, i - start);
    } else if (is_continuation(c)) {
      cur += line[i++];
    } else {
      if (w >= width) {
        out.push_back(cur);
        cur.clear();
        w = 0;
      }
      std::size_t start = i;
      ++i;
      while (i < line.size() && is_continuation(static_cast<unsigned char>(line[i]))) ++i;
      cur.append(line, start, i - start);
      ++w;
    }
  }
  if (!cur.empty() || out.empty()) out.push_back(cur);
}

std::string collapse_cr(const std::string& logical) {
  std::size_t pos = logical.rfind('\r');
  if (pos == std::string::npos) return logical;
  if (pos + 1 >= logical.size()) return logical.substr(0, pos);
  return logical.substr(pos + 1);
}

std::vector<std::string> conv_display_lines(const std::string& raw, int width) {
  std::string conv = keep_sgr(raw);
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= conv.size()) {
    std::size_t nl = conv.find('\n', start);
    std::string seg =
        conv.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    wrap_into(collapse_cr(seg), width, out);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out;
}

struct PtyScreen {
  int rows = 0;
  int cols = 0;
  int cr = 0;
  int cc = 0;
  std::vector<std::vector<std::string>> cells;
  std::deque<std::vector<std::string>> history;
  static constexpr std::size_t kHistoryCap = 1000;

  void reset(int r, int c) {
    rows = std::max(1, r);
    cols = std::max(1, c);
    cr = 0;
    cc = 0;
    cells.assign(static_cast<std::size_t>(rows),
                 std::vector<std::string>(static_cast<std::size_t>(cols), " "));
    history.clear();
  }
  void scroll_up() {
    history.push_back(cells.front());
    if (history.size() > kHistoryCap) history.pop_front();
    cells.erase(cells.begin());
    cells.push_back(std::vector<std::string>(static_cast<std::size_t>(cols), " "));
  }
  int max_scroll() const { return static_cast<int>(history.size()); }
  std::vector<std::string> view(int scroll) {
    if (scroll < 0) scroll = 0;
    if (scroll > max_scroll()) scroll = max_scroll();
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(rows));
    int hist = static_cast<int>(history.size());
    for (int k = 0; k < rows; ++k) {
      int src = hist - scroll + k;
      std::string line;
      if (src < 0) {
        line = "";
      } else if (src < hist) {
        const auto& row = history[static_cast<std::size_t>(src)];
        for (const auto& ch : row) line += ch;
      } else {
        const auto& row = cells[static_cast<std::size_t>(src - hist)];
        for (const auto& ch : row) line += ch;
      }
      while (!line.empty() && line.back() == ' ') line.pop_back();
      out.push_back(line);
    }
    return out;
  }
  void newline() {
    ++cr;
    if (cr >= rows) {
      cr = rows - 1;
      scroll_up();
    }
  }
  void clamp() {
    if (cr < 0) cr = 0;
    if (cr >= rows) cr = rows - 1;
    if (cc < 0) cc = 0;
    if (cc > cols) cc = cols;
  }
  void put(const std::string& ch) {
    if (cc >= cols) {
      cc = 0;
      newline();
    }
    if (cr >= 0 && cr < rows && cc >= 0 && cc < cols)
      cells[static_cast<std::size_t>(cr)][static_cast<std::size_t>(cc)] = ch;
    ++cc;
  }
  void erase_line(int mode) {
    if (cr < 0 || cr >= rows) return;
    int a = (mode == 1) ? 0 : ((mode == 2) ? 0 : cc);
    int b = (mode == 1) ? std::min(cc, cols - 1) : (cols - 1);
    for (int x = a; x <= b && x >= 0 && x < cols; ++x)
      cells[static_cast<std::size_t>(cr)][static_cast<std::size_t>(x)] = " ";
  }
  void erase_display(int mode) {
    if (mode == 2 || mode == 3) {
      for (auto& row : cells) std::fill(row.begin(), row.end(), std::string(" "));
      cr = 0;
      cc = 0;
      return;
    }
    erase_line(0);
    for (int y = cr + 1; y < rows; ++y)
      std::fill(cells[static_cast<std::size_t>(y)].begin(),
                cells[static_cast<std::size_t>(y)].end(), std::string(" "));
  }
  void handle_csi(char fin, const std::string& params) {
    std::vector<int> nums;
    std::string curp;
    for (char c : params) {
      if (c == ';') {
        nums.push_back(curp.empty() ? -1 : std::atoi(curp.c_str()));
        curp.clear();
      } else if (c >= '0' && c <= '9') {
        curp += c;
      }
    }
    nums.push_back(curp.empty() ? -1 : std::atoi(curp.c_str()));
    auto n = [&](std::size_t idx, int def) {
      return (idx < nums.size() && nums[idx] > 0) ? nums[idx] : def;
    };
    switch (fin) {
      case 'H':
      case 'f':
        cr = n(0, 1) - 1;
        cc = n(1, 1) - 1;
        clamp();
        break;
      case 'A': cr -= n(0, 1); clamp(); break;
      case 'B': cr += n(0, 1); clamp(); break;
      case 'C': cc += n(0, 1); clamp(); break;
      case 'D': cc -= n(0, 1); clamp(); break;
      case 'G': cc = n(0, 1) - 1; clamp(); break;
      case 'd': cr = n(0, 1) - 1; clamp(); break;
      case 'K': erase_line(nums[0] < 0 ? 0 : nums[0]); break;
      case 'J': erase_display(nums[0] < 0 ? 0 : nums[0]); break;
      default: break;
    }
  }
  void feed(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
      unsigned char ch = static_cast<unsigned char>(s[i]);
      if (ch == 0x1b) {
        if (i + 1 < s.size() && s[i + 1] == '[') {
          std::size_t j = i + 2;
          while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
          if (j >= s.size()) break;
          handle_csi(s[j], s.substr(i + 2, j - (i + 2)));
          i = j + 1;
        } else if (i + 1 < s.size() && s[i + 1] == ']') {
          std::size_t j = i + 2;
          while (j < s.size() && static_cast<unsigned char>(s[j]) != 0x07 &&
                 static_cast<unsigned char>(s[j]) != 0x1b)
            ++j;
          if (j < s.size() && static_cast<unsigned char>(s[j]) == 0x1b) ++j;
          if (j < s.size()) ++j;
          i = j;
        } else {
          i += 2;
        }
      } else if (ch == '\r') {
        cc = 0;
        ++i;
      } else if (ch == '\n') {
        newline();
        ++i;
      } else if (ch == '\b') {
        if (cc > 0) --cc;
        ++i;
      } else if (ch == '\t') {
        cc = std::min(cols, ((cc / 8) + 1) * 8);
        ++i;
      } else if (ch < 0x20) {
        ++i;
      } else {
        std::size_t j = i + 1;
        while (j < s.size() && is_continuation(static_cast<unsigned char>(s[j]))) ++j;
        put(s.substr(i, j - i));
        i = j;
      }
    }
  }
  std::vector<std::string> render() {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(rows));
    for (int r = 0; r < rows; ++r) {
      std::string line;
      for (int c = 0; c < cols; ++c) line += cells[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
      while (!line.empty() && line.back() == ' ') line.pop_back();
      out.push_back(line);
    }
    return out;
  }
};

PtyScreen g_emu;
std::size_t g_emu_cursor = 0;
int g_emu_cols = -1;
int g_emu_h = -1;
int g_term_max_scroll = 0;

std::vector<std::string> pty_pane_lines(int cols, int height, int term_scroll) {
  if (cols < 1) cols = 1;
  if (height < 1) height = 1;
  if (cols != g_emu_cols || height != g_emu_h) {
    g_emu_cols = cols;
    g_emu_h = height;
    g_emu.reset(height, cols);
    terminal_session().resize(height, cols);
    g_emu_cursor = 0;
    std::string all = terminal_session().read_available(g_emu_cursor);
    if (all.size() > 16384) all = all.substr(all.size() - 16384);
    g_emu.feed(all);
  } else {
    std::string chunk = terminal_session().read_available(g_emu_cursor);
    if (!chunk.empty()) g_emu.feed(chunk);
  }
  g_term_max_scroll = g_emu.max_scroll();
  return g_emu.view(term_scroll);
}

void reader_loop() {
  std::vector<char> buf(8192);
  while (g_running.load()) {
    ssize_t n = ::read(g_pipe_r, buf.data(), buf.size());
    if (n > 0) {
      std::lock_guard<std::mutex> lk(g_conv_mutex);
      g_conv.append(buf.data(), static_cast<std::size_t>(n));
      if (g_conv.size() > kConvCap) g_conv.erase(0, g_conv.size() - kConvCap);
    } else if (n == 0) {
      break;
    } else {
      if (errno == EINTR) continue;
      break;
    }
  }
}

std::string green_cell(const std::string& text, int width, bool dim) {
  return std::string(GREEN_BG) + (dim ? DARK_DIM_FG : DARK_FG) + fit_plain(text, width) +
         RESET;
}

std::string clip_visible(const std::string& s, int width) {
  if (width < 1) return std::string();
  std::vector<std::size_t> starts;
  for (std::size_t i = 0; i < s.size();) {
    starts.push_back(i);
    ++i;
    while (i < s.size() && is_continuation(static_cast<unsigned char>(s[i]))) ++i;
  }
  if (static_cast<int>(starts.size()) <= width) return s;
  return s.substr(starts[starts.size() - static_cast<std::size_t>(width)]);
}

std::string green_cell_rich(const std::string& content, int width) {
  std::string out = std::string(GREEN_BG) + DARK_FG;
  int w = 0;
  for (std::size_t i = 0; i < content.size();) {
    unsigned char c = static_cast<unsigned char>(content[i]);
    if (c == 0x1b) {
      std::size_t start = i;
      if (i + 1 < content.size() && content[i + 1] == '[') {
        i += 2;
        while (i < content.size() && !(content[i] >= '@' && content[i] <= '~')) ++i;
        if (i < content.size()) ++i;
      } else {
        ++i;
      }
      out.append(content, start, i - start);
    } else if (is_continuation(c)) {
      out += content[i++];
    } else {
      if (w >= width) break;
      std::size_t start = i;
      ++i;
      while (i < content.size() && is_continuation(static_cast<unsigned char>(content[i]))) ++i;
      out.append(content, start, i - start);
      ++w;
    }
  }
  out += std::string(GREEN_BG) + DARK_FG;
  for (; w < width; ++w) out += ' ';
  out += RESET;
  return out;
}

std::string pill_rows(int row, int width) {
  if (row == 1) {
    std::string content = "  " + pulse_on_green(g_frame) + "\xE2\x97\x86" + std::string(DARK_FG) +
                          BOLD + " LOREA";
    return green_cell_rich(content, width);
  }
  if (row == 2) {
    std::string sub = g_status_fn ? strip_ansi(g_status_fn()) : std::string();
    if (static_cast<int>(sub.size()) > width - 4 && width > 5)
      sub = sub.substr(0, static_cast<std::size_t>(width - 5)) + "\xE2\x80\xA6";
    return green_cell_rich(std::string("  ") + DARK_DIM_FG + sub, width);
  }
  return green_cell("", width, false);
}

void compose(std::vector<std::string>& rows, int R, int C) {
  std::string conv;
  {
    std::lock_guard<std::mutex> lk(g_conv_mutex);
    conv = g_conv;
  }
  std::string input;
  std::size_t icur = 0;
  bool tfocus = false;
  int scroll = 0;
  int term_scroll = 0;
  {
    std::lock_guard<std::mutex> lk(g_input_mutex);
    input = g_input;
    icur = g_input_cursor;
    tfocus = g_term_focus;
    scroll = g_scroll;
    term_scroll = g_term_scroll;
  }
  std::string status;
  {
    std::lock_guard<std::mutex> lk(g_status_mutex);
    status = g_status_line;
  }

  bool wide = (C >= 96 && R >= 16);
  int left_x = 2;
  int pill_h = 3;

  rows.assign(static_cast<std::size_t>(R) + 1, std::string());
  auto emit = [&](int r, const std::string& body) {
    rows[static_cast<std::size_t>(r)] = body;
  };

  if (wide) {
    int right_w = C * 42 / 100;
    if (right_w < 32) right_w = 32;
    if (right_w > 80) right_w = 80;
    int gap = 2;
    int left_w = C - left_x - right_w - gap - 1;
    if (left_w < 20) {
      left_w = 20;
      right_w = C - left_x - left_w - gap - 1;
    }
    int main_bottom = R - 1;
    int term_top = 1;
    int term_h = main_bottom;
    int conv_top = 1 + pill_h;
    int input_h = 3;
    int input_top = R - 1 - input_h + 1;
    int conv_bottom = input_top - 2;
    int conv_h = conv_bottom - conv_top + 1;
    if (conv_h < 1) conv_h = 1;

    int right_pane_x = left_x + left_w + gap;
    g_term_x = right_pane_x;
    g_term_y = -1;
    std::vector<std::string> cl = conv_display_lines(conv, left_w);
    std::vector<std::string> tl = pty_pane_lines(right_w - 2, term_h - 2, term_scroll);

    int cl_total = static_cast<int>(cl.size());
    int cl_start = cl_total - conv_h - scroll;
    if (cl_start < 0) cl_start = 0;
    int tl_total = static_cast<int>(tl.size());
    int tl_start = 0;

    std::string pad_left(static_cast<std::size_t>(left_x - 1), ' ');
    std::string gap_str(static_cast<std::size_t>(gap), ' ');

    for (int r = 1; r <= main_bottom; ++r) {
      std::string left_cell;
      if (r <= pill_h) {
        left_cell = pill_rows(r - 1, left_w);
      } else if (r == conv_top && scroll > 0) {
        std::string hint = std::string(CYAN_FG) + BOLD + "\xE2\x8C\x83 " + std::to_string(scroll) +
                           " up" + std::string(NEON_FG) + "  \xC2\xB7 scroll down for live";
        left_cell = fit_ansi(hint, left_w);
      } else if (r >= conv_top && r <= conv_bottom) {
        int idx = cl_start + (r - conv_top);
        if (idx >= 0 && idx < cl_total)
          left_cell = fit_ansi(cl[static_cast<std::size_t>(idx)], left_w);
        else
          left_cell = std::string(static_cast<std::size_t>(left_w), ' ');
      } else if (r == conv_bottom + 1 && !status.empty()) {
        left_cell = fit_ansi(status, left_w);
      } else {
        left_cell = std::string(static_cast<std::size_t>(left_w), ' ');
      }

      std::string right_cell;
      int right_x = left_x + left_w + gap;
      (void)right_x;
      if (r == term_top) {
        std::string head = "  " + pulse_on_green(g_frame) + "\xE2\x96\xB8" + std::string(DARK_FG) +
                           BOLD + " AI TERMINAL";
        if (term_scroll > 0)
          head += std::string(DARK_DIM_FG) + "   \xE2\x86\x91" + std::to_string(term_scroll);
        else
          head += std::string(DARK_DIM_FG) + "   live";
        right_cell = green_cell_rich(head, right_w);
      } else if (r == term_top + 1) {
        right_cell = green_cell("", right_w, false);
      } else if (r >= term_top + 2 && r <= term_top + term_h - 1) {
        int idx = tl_start + (r - (term_top + 2));
        std::string content = (idx >= 0 && idx < tl_total) ? tl[static_cast<std::size_t>(idx)]
                                                           : std::string();
        right_cell = green_cell(" " + content, right_w, false);
      } else {
        right_cell = green_cell("", right_w, false);
      }

      emit(r, pad_left + left_cell + gap_str + right_cell);
    }

    for (int r = input_top; r <= input_top + input_h - 1; ++r) {
      std::string body;
      if (r == input_top || r == input_top + input_h - 1) {
        body = pad_left + green_cell("", left_w, false);
      } else {
        std::string caret = blink_on(g_frame) ? "\xE2\x96\x88" : " ";
        if (tfocus) {
          body = pad_left + green_cell_rich(std::string("  ") + DARK_DIM_FG + BOLD +
                     "\xE2\x96\xB8 shell" + std::string(DARK_DIM_FG) +
                     "  typing into the terminal \xC2\xB7 Ctrl-T to return", left_w);
        } else if (input.empty()) {
          body = pad_left + green_cell_rich(std::string("  ") + DARK_FG + BOLD + "\xE2\x9D\xAF " +
                     std::string(DARK_DIM_FG) + "Ask anything \xC2\xB7 type to chat  " +
                     std::string(DARK_FG) + caret, left_w);
        } else {
          std::string shown = clip_visible(strip_ansi(input), left_w - 5);
          body = pad_left + green_cell_rich(std::string("  ") + DARK_FG + BOLD + "\xE2\x9D\xAF " +
                     std::string(DARK_FG) + shown + std::string(DARK_FG) + caret, left_w);
        }
      }
      emit(r, body);
    }
    emit(R, "");
    (void)icur;
  } else {
    int width = C - left_x - 1;
    if (width < 10) width = C - 1;
    int pill_bottom = pill_h;
    int input_h = 3;
    int input_top = R - input_h;
    int body_top = pill_bottom + 1;
    int body_bottom = input_top - 1;
    int body_h = body_bottom - body_top + 1;
    if (body_h < 2) body_h = 2;
    int conv_h = body_h * 55 / 100;
    if (conv_h < 1) conv_h = 1;
    int term_label = body_top + conv_h;
    int term_top = term_label + 1;
    int term_bottom = body_bottom;

    int term_h = term_bottom - term_top + 1;
    g_term_x = 0;
    g_term_y = term_top;
    std::vector<std::string> cl = conv_display_lines(conv, width);
    std::vector<std::string> tl = pty_pane_lines(width - 1, term_h, term_scroll);
    int cl_total = static_cast<int>(cl.size());
    int cl_start = cl_total - conv_h - scroll;
    if (cl_start < 0) cl_start = 0;
    int tl_total = static_cast<int>(tl.size());
    int tl_start = 0;

    std::string pad_left(static_cast<std::size_t>(left_x - 1), ' ');

    for (int r = 1; r <= pill_h; ++r) emit(r, pad_left + pill_rows(r - 1, width));

    for (int r = body_top; r <= body_bottom; ++r) {
      if (r == body_bottom && !status.empty()) {
        emit(r, pad_left + fit_ansi(status, width));
      } else if (r <= body_top + conv_h - 1) {
        int idx = cl_start + (r - body_top);
        std::string cell = (idx >= 0 && idx < cl_total)
                               ? fit_ansi(cl[static_cast<std::size_t>(idx)], width)
                               : std::string(static_cast<std::size_t>(width), ' ');
        emit(r, pad_left + cell);
      } else if (r == term_label) {
        std::string lbl = std::string(pulse_neon(g_frame)) + BOLD + "\xE2\x96\xB8 AI TERMINAL" +
                          RESET + GREEN_DIM_FG + (term_scroll > 0 ? "  \xE2\x86\x91" + std::to_string(term_scroll) : std::string("  live"));
        emit(r, pad_left + std::string(GREEN_DIM_FG) + fit_ansi(lbl, width) + RESET);
      } else {
        int idx = tl_start + (r - term_top);
        std::string content =
            (idx >= 0 && idx < tl_total) ? tl[static_cast<std::size_t>(idx)] : std::string();
        emit(r, pad_left + green_cell(" " + content, width, false));
      }
    }

    for (int r = input_top; r <= R; ++r) {
      std::string body;
      if (r == input_top || r == R) {
        body = pad_left + green_cell("", width, false);
      } else {
        std::string caret = blink_on(g_frame) ? "\xE2\x96\x88" : " ";
        if (tfocus)
          body = pad_left + green_cell_rich(std::string("  ") + DARK_DIM_FG + BOLD +
                     "\xE2\x96\xB8 shell \xC2\xB7 Ctrl-T to return", width);
        else if (input.empty())
          body = pad_left + green_cell_rich(std::string("  ") + DARK_FG + BOLD + "\xE2\x9D\xAF " +
                     std::string(DARK_DIM_FG) + "Ask anything  " + std::string(DARK_FG) + caret, width);
        else
          body = pad_left + green_cell_rich(std::string("  ") + DARK_FG + BOLD + "\xE2\x9D\xAF " +
                     std::string(DARK_FG) + clip_visible(strip_ansi(input), width - 5) +
                     std::string(DARK_FG) + caret, width);
      }
      emit(r, body);
    }
  }

}

void render_loop() {
  std::vector<std::string> cur;
  std::vector<std::string> last;
  int last_R = -1, last_C = -1;
  while (g_running.load()) {
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    int R = 24, C = 80;
    if (::ioctl(g_real_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
      R = ws.ws_row;
      C = ws.ws_col;
    }
    bool resized = (R != last_R || C != last_C);
    if (resized) {
      last_R = R;
      last_C = C;
      std::string clr = "\033[2J";
      full_write(g_real_fd, clr.data(), clr.size());
      last.clear();
    }

    compose(cur, R, C);

    std::string out = "\033[?2026h";
    bool any = false;
    bool want_mouse = g_mouse_want.load();
    if (want_mouse != g_mouse_on) {
      out += want_mouse ? "\033[?1000h\033[?1006h" : "\033[?1000l\033[?1006l";
      g_mouse_on = want_mouse;
      any = true;
    }
    for (int r = 1; r <= R; ++r) {
      const std::string& body = cur[static_cast<std::size_t>(r)];
      if (static_cast<int>(last.size()) <= r || last[static_cast<std::size_t>(r)] != body) {
        out += "\033[" + std::to_string(r) + ";1H\033[2K" + body;
        any = true;
      }
    }
    out += "\033[?2026l";
    if (any) full_write(g_real_fd, out.data(), out.size());
    last = cur;
    ++g_frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
  }
}

}  // namespace

bool live_active() { return g_active.load(); }

void live_set_status_line(const std::string& line) {
  std::lock_guard<std::mutex> lk(g_status_mutex);
  g_status_line = line;
}

std::string live_take_terminal_context() {
  bool used;
  {
    std::lock_guard<std::mutex> lk(g_input_mutex);
    used = g_terminal_used;
    g_terminal_used = false;
  }
  std::string delta = terminal_session().read_available(g_ctx_cursor);
  if (!used) return std::string();
  std::string plain = strip_ansi(delta);
  std::size_t a = 0;
  std::size_t b = plain.size();
  while (a < b && (plain[a] == '\n' || plain[a] == ' ' || plain[a] == '\t' || plain[a] == '\r'))
    ++a;
  while (b > a && (plain[b - 1] == '\n' || plain[b - 1] == ' ' || plain[b - 1] == '\t' ||
                   plain[b - 1] == '\r'))
    --b;
  plain = plain.substr(a, b - a);
  if (plain.size() > 6000) plain = "...\n" + plain.substr(plain.size() - 6000);
  return plain;
}

void live_note_activity() {}

bool live_begin(std::function<std::string()> status_fn) {
  if (g_active.load()) return true;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

  g_status_fn = std::move(status_fn);

  if (::tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
    g_termios_saved = true;
    struct termios raw = g_saved_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  }

  g_real_fd = ::dup(STDOUT_FILENO);

  int fds[2];
  if (::pipe(fds) != 0) {
    if (g_termios_saved) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    if (g_real_fd >= 0) ::close(g_real_fd);
    g_real_fd = -1;
    return false;
  }
  g_pipe_r = fds[0];
  g_pipe_w = fds[1];
  g_saved_stdout = ::dup(STDOUT_FILENO);
  ::dup2(g_pipe_w, STDOUT_FILENO);
  ::close(g_pipe_w);
  g_pipe_w = -1;

  std::cout << std::unitbuf;
  ::setvbuf(stdout, nullptr, _IONBF, 0);

  terminal_session();
  g_shared_terminal_active = true;
  g_emu_cols = -1;
  g_emu_h = -1;
  g_emu_cursor = 0;

  const char* enter = "\033[?1049h\033[2J\033[H\033[?25l";
  full_write(g_real_fd, enter, std::strlen(enter));

  g_active.store(true);
  g_running.store(true);
  g_reader = std::thread(reader_loop);
  g_renderer = std::thread(render_loop);
  return true;
}

std::string live_read_line(const std::vector<std::string>* history) {
  if (!g_active.load()) return std::string();

  std::size_t hist_idx = history ? history->size() : 0;
  std::string stash;
  bool cc_armed = false;
  std::chrono::steady_clock::time_point cc_at{};

  struct MouseScope {
    MouseScope() { g_mouse_want.store(true); }
    ~MouseScope() { g_mouse_want.store(false); }
  } mouse_scope;

  char buf[256];
  for (;;) {
    ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return std::string("/exit");
    }

    bool focus_terminal;
    {
      std::lock_guard<std::mutex> lk(g_input_mutex);
      focus_terminal = g_term_focus;
    }

    if (focus_terminal) {
      std::string passthrough;
      for (ssize_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == 0x14) {
          std::lock_guard<std::mutex> lk(g_input_mutex);
          g_term_focus = false;
          passthrough.clear();
          break;
        }
        passthrough += static_cast<char>(c);
      }
      if (!passthrough.empty()) {
        terminal_session().write_input(passthrough);
        std::lock_guard<std::mutex> lk(g_input_mutex);
        g_terminal_used = true;
      }
      continue;
    }

    for (ssize_t i = 0; i < n; ++i) {
      unsigned char c = static_cast<unsigned char>(buf[i]);

      if (cc_armed && c != 0x03) {
        cc_armed = false;
        live_set_status_line("");
      }

      if (c == '\r' || c == '\n') {
        std::string line;
        {
          std::lock_guard<std::mutex> lk(g_input_mutex);
          line = g_input;
          g_input.clear();
          g_input_cursor = 0;
          g_scroll = 0;
          g_term_scroll = 0;
        }
        return line;
      } else if (c == 0x14) {
        std::lock_guard<std::mutex> lk(g_input_mutex);
        g_term_focus = true;
      } else if (c == 0x7f || c == 0x08) {
        std::lock_guard<std::mutex> lk(g_input_mutex);
        if (g_input_cursor > 0) {
          std::size_t prev = g_input_cursor - 1;
          while (prev > 0 && is_continuation(static_cast<unsigned char>(g_input[prev]))) --prev;
          g_input.erase(prev, g_input_cursor - prev);
          g_input_cursor = prev;
        }
      } else if (c == 0x03) {
        auto now = std::chrono::steady_clock::now();
        if (cc_armed && now - cc_at < std::chrono::milliseconds(1500)) {
          live_set_status_line("");
          return std::string("/exit");
        }
        cc_armed = true;
        cc_at = now;
        {
          std::lock_guard<std::mutex> lk(g_input_mutex);
          g_input.clear();
          g_input_cursor = 0;
        }
        live_set_status_line(std::string(GREEN_DIM_FG) + "  Press Ctrl-C again to quit" +
                             RESET);
      } else if (c == 0x04) {
        std::lock_guard<std::mutex> lk(g_input_mutex);
        if (g_input.empty()) return std::string("/exit");
      } else if (c == 0x15) {
        std::lock_guard<std::mutex> lk(g_input_mutex);
        g_input.erase(0, g_input_cursor);
        g_input_cursor = 0;
      } else if (c == 0x1b) {
        if (i + 2 < n && buf[i + 1] == '[') {
          char code = buf[i + 2];
          if (code == '<') {
            std::size_t j = i + 3;
            int vals[3] = {0, 0, 0};
            int vi = 0;
            int acc = 0;
            while (j < static_cast<std::size_t>(n) && buf[j] != 'M' && buf[j] != 'm') {
              char d = buf[j];
              if (d >= '0' && d <= '9')
                acc = acc * 10 + (d - '0');
              else if (d == ';') {
                if (vi < 3) vals[vi] = acc;
                ++vi;
                acc = 0;
              }
              ++j;
            }
            if (j < static_cast<std::size_t>(n)) {
              if (vi < 3) vals[vi] = acc;
              int btn = vals[0], col = vals[1], row = vals[2];
              if (btn == 64 || btn == 65) {
                bool up = (btn == 64);
                bool term_pane = (g_term_y >= 0) ? (row >= g_term_y) : (col >= g_term_x);
                std::lock_guard<std::mutex> lk(g_input_mutex);
                if (term_pane) {
                  g_term_scroll += up ? 3 : -3;
                  if (g_term_scroll < 0) g_term_scroll = 0;
                  if (g_term_scroll > g_term_max_scroll) g_term_scroll = g_term_max_scroll;
                } else {
                  g_scroll += up ? 3 : -3;
                  if (g_scroll < 0) g_scroll = 0;
                }
              }
              i = j;
            } else {
              i = static_cast<ssize_t>(n);
            }
          } else if (code == 'C') {
            std::lock_guard<std::mutex> lk(g_input_mutex);
            if (g_input_cursor < g_input.size()) {
              ++g_input_cursor;
              while (g_input_cursor < g_input.size() &&
                     is_continuation(static_cast<unsigned char>(g_input[g_input_cursor])))
                ++g_input_cursor;
            }
            i += 2;
          } else if (code == 'D') {
            std::lock_guard<std::mutex> lk(g_input_mutex);
            if (g_input_cursor > 0) {
              --g_input_cursor;
              while (g_input_cursor > 0 &&
                     is_continuation(static_cast<unsigned char>(g_input[g_input_cursor])))
                --g_input_cursor;
            }
            i += 2;
          } else if (code == 'A') {
            if (history && !history->empty() && hist_idx > 0) {
              if (hist_idx == history->size()) {
                std::lock_guard<std::mutex> lk(g_input_mutex);
                stash = g_input;
              }
              --hist_idx;
              std::lock_guard<std::mutex> lk(g_input_mutex);
              g_input = (*history)[hist_idx];
              g_input_cursor = g_input.size();
            }
            i += 2;
          } else if (code == 'B') {
            if (history && hist_idx < history->size()) {
              ++hist_idx;
              std::lock_guard<std::mutex> lk(g_input_mutex);
              if (hist_idx == history->size()) {
                g_input = stash;
              } else {
                g_input = (*history)[hist_idx];
              }
              g_input_cursor = g_input.size();
            }
            i += 2;
          } else if (code == '5') {
            std::lock_guard<std::mutex> lk(g_input_mutex);
            g_scroll += 5;
            if (i + 3 < n && buf[i + 3] == '~') i += 3; else i += 2;
          } else if (code == '6') {
            std::lock_guard<std::mutex> lk(g_input_mutex);
            g_scroll -= 5;
            if (g_scroll < 0) g_scroll = 0;
            if (i + 3 < n && buf[i + 3] == '~') i += 3; else i += 2;
          } else {
            i += 2;
          }
        }
      } else if (c >= 0x20) {
        std::size_t start = static_cast<std::size_t>(i);
        std::size_t len = 1;
        while (i + 1 < n && is_continuation(static_cast<unsigned char>(buf[i + 1]))) {
          ++i;
          ++len;
        }
        std::lock_guard<std::mutex> lk(g_input_mutex);
        g_input.insert(g_input_cursor, std::string(buf + start, len));
        g_input_cursor += len;
      }
    }
  }
}

bool live_suspend() {
  if (!g_active.load()) return false;
  g_active.store(false);
  g_running.store(false);
  if (g_renderer.joinable()) g_renderer.join();

  if (g_saved_stdout >= 0) {
    std::cout.flush();
    ::fflush(stdout);
    ::dup2(g_saved_stdout, STDOUT_FILENO);
    ::close(g_saved_stdout);
    g_saved_stdout = -1;
  }
  if (g_pipe_r >= 0) {
    ::close(g_pipe_r);
    g_pipe_r = -1;
  }
  if (g_reader.joinable()) g_reader.join();

  std::cout << std::nounitbuf;
  ::setvbuf(stdout, nullptr, _IOLBF, 0);

  if (g_termios_saved) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);

  const char* reset = "\033[?1000l\033[?1006l\033[?25h\033[2J\033[H";
  full_write(g_real_fd, reset, std::strlen(reset));
  g_mouse_on = false;
  g_mouse_want.store(false);

  g_suspended = true;
  return true;
}

void live_resume() {
  if (!g_suspended) return;

  if (g_termios_saved) {
    struct termios raw = g_saved_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  }

  g_saved_stdout = ::dup(STDOUT_FILENO);
  int fds[2];
  if (::pipe(fds) == 0) {
    g_pipe_r = fds[0];
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[1]);
  }
  std::cout << std::unitbuf;
  ::setvbuf(stdout, nullptr, _IONBF, 0);

  const char* hide = "\033[?25l\033[2J\033[H";
  full_write(g_real_fd, hide, std::strlen(hide));

  g_suspended = false;
  g_active.store(true);
  g_running.store(true);
  g_reader = std::thread(reader_loop);
  g_renderer = std::thread(render_loop);
}

void live_end() {
  if (!g_active.load()) return;
  g_running.store(false);

  if (g_renderer.joinable()) g_renderer.join();

  if (g_saved_stdout >= 0) {
    std::cout.flush();
    ::fflush(stdout);
    ::dup2(g_saved_stdout, STDOUT_FILENO);
    ::close(g_saved_stdout);
    g_saved_stdout = -1;
  }

  if (g_pipe_r >= 0) {
    ::close(g_pipe_r);
    g_pipe_r = -1;
  }
  if (g_reader.joinable()) g_reader.join();

  std::cout << std::nounitbuf;
  ::setvbuf(stdout, nullptr, _IOLBF, 0);

  const char* leave = "\033[?1000l\033[?1006l\033[?25h\033[?1049l";
  full_write(g_real_fd, leave, std::strlen(leave));
  g_mouse_on = false;
  g_mouse_want.store(false);

  if (g_termios_saved) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    g_termios_saved = false;
  }

  {
    std::lock_guard<std::mutex> lk(g_conv_mutex);
    if (!g_conv.empty()) {
      std::string tail = g_conv.size() > 8000 ? g_conv.substr(g_conv.size() - 8000) : g_conv;
      full_write(g_real_fd, tail.data(), tail.size());
    }
    g_conv.clear();
  }

  if (g_real_fd >= 0) {
    ::close(g_real_fd);
    g_real_fd = -1;
  }

  {
    std::lock_guard<std::mutex> lk(g_input_mutex);
    g_input.clear();
    g_input_cursor = 0;
    g_term_focus = false;
    g_scroll = 0;
  }

  g_active.store(false);
  g_status_fn = nullptr;
}

}  // namespace ocli
