#include "ansi.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <regex>
#include <thread>

#include <unistd.h>
#include <sys/ioctl.h>

namespace ocli {

const char* ACCENT = Colors::TEAL;
const char* MUTED  = Colors::SLATE;

const std::vector<int> FLAIR_RAMP = {51, 50, 44, 45, 39, 38, 75, 69, 105, 141};

const std::vector<std::string> CELEBRATION_ICONS = {
    "✦", "✧", "⋆", "✶", "✷", "✸", "❉", "✺"};

namespace {

inline const char* or_default(const char* s, const char* d) {
    return (s && s[0]) ? s : d;
}

std::string repeat_str(const std::string& unit, int n) {
    std::string r;
    if (n <= 0) return r;
    r.reserve(unit.size() * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) r += unit;
    return r;
}

long py_round(double x) {
    return static_cast<long>(std::nearbyint(x));
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
double rand01() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng());
}

std::vector<std::string> split_newline(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        std::size_t next = s.find(from, pos);
        if (next == std::string::npos) {
            out += s.substr(pos);
            break;
        }
        out += s.substr(pos, next - pos);
        out += to;
        pos = next + from.size();
    }
    return out;
}

std::vector<std::string> py_splitlines(const std::string& s) {
    std::u32string u = utf8_to_u32(s);
    std::vector<std::string> out;
    std::u32string cur;
    auto is_break = [](char32_t c) {
        return c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x1C ||
               c == 0x1D || c == 0x1E || c == 0x85 || c == 0x2028 || c == 0x2029;
    };
    std::size_t i = 0, n = u.size();
    while (i < n) {
        char32_t c = u[i];
        if (is_break(c)) {
            out.push_back(u32_to_utf8(cur));
            cur.clear();
            if (c == 0x0D && i + 1 < n && u[i + 1] == 0x0A) ++i;
            ++i;
        } else {
            cur.push_back(c);
            ++i;
        }
    }
    if (!cur.empty()) out.push_back(u32_to_utf8(cur));
    return out;
}

}

std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len;
        if (c < 0x80)            len = 1;
        else if ((c >> 5) == 0x6) len = 2;
        else if ((c >> 4) == 0xE) len = 3;
        else if ((c >> 3) == 0x1E) len = 4;
        else                      len = 1;
        if (i + len > n) {
            len = 1;
        } else {
            for (std::size_t k = 1; k < len; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                    len = 1;
                    break;
                }
            }
        }
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

std::size_t utf8_len(const std::string& s) {
    std::size_t count = 0, i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len;
        if (c < 0x80)            len = 1;
        else if ((c >> 5) == 0x6) len = 2;
        else if ((c >> 4) == 0xE) len = 3;
        else if ((c >> 3) == 0x1E) len = 4;
        else                      len = 1;
        if (i + len > n) {
            len = 1;
        } else {
            for (std::size_t k = 1; k < len; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                    len = 1;
                    break;
                }
            }
        }
        ++count;
        i += len;
    }
    return count;
}

std::u32string utf8_to_u32(const std::string& s) {
    std::u32string out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        std::size_t len;
        if (c < 0x80)            { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else                      { cp = c;        len = 1; }
        if (i + len > n) {
            cp = c;
            len = 1;
        } else {
            bool ok = true;
            for (std::size_t k = 1; k < len; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                cp = c;
                len = 1;
            } else {
                for (std::size_t k = 1; k < len; ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            }
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string u32_to_utf8(const std::u32string& s) {
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string utf8_substr(const std::string& s, std::size_t start, std::size_t count) {
    std::vector<std::string> chars = utf8_chars(s);
    if (start >= chars.size()) return "";
    std::size_t end;
    if (count == std::string::npos || start + count > chars.size())
        end = chars.size();
    else
        end = start + count;
    std::string out;
    for (std::size_t i = start; i < end; ++i) out += chars[i];
    return out;
}

std::string clean_ansi(const std::string& text) {
    static const std::regex ansi_re("\033\\[[0-9;]*m");
    return std::regex_replace(text, ansi_re, "");
}

std::size_t clean_len(const std::string& text) {
    return utf8_len(clean_ansi(text));
}

int term_cols(int def) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return def;
}

int term_width(int def) {
    int cols = term_cols(def + LEFT_MARGIN * 2);

    int usable = cols - LEFT_MARGIN * 2;
    if (usable < 60)
        return std::max(20, usable);
    return usable;
}

std::string left_indent() {
    return std::string(LEFT_MARGIN, ' ');
}

std::string center_pad(int content_len) {
    int cols = term_cols();
    int half = (cols - content_len) / 2;
    return std::string(static_cast<std::size_t>(std::max(LEFT_MARGIN, half)), ' ');
}

std::string badge(const std::string& text, const char* fg, const char* bg) {
    return std::string(bg) + fg + Colors::BOLD + " " + text + " " + Colors::RESET;
}

std::string frame_title(const std::string& title_in, const char* style, const char* subtitle) {
    const char* st = or_default(style, MUTED);
    int width = term_width();
    std::string title = title_in;

    std::string sub;
    int sub_len = 0;
    if (subtitle && subtitle[0] &&
        width - static_cast<int>(clean_len(std::string(" ") + title + " ")) -
                static_cast<int>(clean_len(std::string(subtitle) + " ")) - 4 >= 2) {
        sub = std::string(Colors::RESET) + Colors::DIM + Colors::GRAY + subtitle +
              Colors::RESET + st + " ";
        sub_len = static_cast<int>(clean_len(std::string(subtitle) + " "));
    }

    int label_len = static_cast<int>(clean_len(std::string(" ") + title + " "));
    if (width - label_len - sub_len - 4 < 2) {

        int keep = std::max(1, width - sub_len - 4 - 2 - 2);
        if (keep < static_cast<int>(utf8_len(title)))
            title = utf8_substr(title, 0, static_cast<std::size_t>(keep)) + "…";
        label_len = static_cast<int>(clean_len(std::string(" ") + title + " "));
    }

    std::string label = std::string(" ") + Colors::RESET + Colors::BOLD + Colors::WHITE +
                        title + Colors::RESET + st + " ";
    std::string line = repeat_str("─", std::max(2, width - label_len - sub_len - 4));
    return std::string(st) + "╭─" + label + sub + line + "─╮" + Colors::RESET;
}

std::string frame_bottom(const char* style, const char* hint) {
    const char* st = or_default(style, MUTED);
    int width = term_width();
    if (hint && hint[0] &&
        width - static_cast<int>(clean_len(std::string(" ") + hint + " ")) - 4 >= 2) {
        std::string hint_text = std::string(" ") + Colors::RESET + Colors::DIM + Colors::GRAY +
                                hint + Colors::RESET + st + " ";
        int hint_len = static_cast<int>(clean_len(std::string(" ") + hint + " "));
        std::string line = repeat_str("─", width - hint_len - 4);
        return std::string(st) + "╰─" + line + hint_text + "─╯" + Colors::RESET;
    }
    return std::string(st) + "╰" + repeat_str("─", width - 2) + "╯" + Colors::RESET;
}

std::string status_label(const std::string& text, const char* style) {
    const char* st = or_default(style, ACCENT);
    return std::string(st) + Colors::BOLD + "▎" + Colors::RESET + Colors::BOLD +
           Colors::WHITE + " " + text + " " + Colors::RESET;
}

std::string gradient_text(const std::string& text, const std::vector<int>* ramp, bool bold) {
    const std::vector<int>& r = (ramp && !ramp->empty()) ? *ramp : FLAIR_RAMP;
    std::vector<std::string> chars = utf8_chars(text);
    if (chars.empty()) return "";
    std::string out;
    std::string b = bold ? Colors::BOLD : "";
    int n = static_cast<int>(chars.size());
    int span = std::max(1, n - 1);
    int rn = static_cast<int>(r.size());
    for (int i = 0; i < n; ++i) {
        int shade = r[std::min(rn - 1, i * rn / (span + 1))];
        out += "\033[38;5;" + std::to_string(shade) + "m" + b + chars[i];
    }
    out += Colors::RESET;
    return out;
}

std::string progress_bar(double done, double total, int slots, bool animate) {
    double total_d = std::max(1.0, total);
    int filled = static_cast<int>(py_round(done * slots / total_d));
    int pct = static_cast<int>(py_round(done * 100.0 / total_d));

    int rn = static_cast<int>(FLAIR_RAMP.size());
    auto render = [&](int n) -> std::string {
        std::string out;
        for (int i = 0; i < slots; ++i) {
            if (i < n) {
                int shade = FLAIR_RAMP[std::min(rn - 1, i * rn / slots)];
                out += "\033[38;5;" + std::to_string(shade) + "m" + Colors::BOLD + "━";
            } else {
                out += std::string(Colors::DIM) + Colors::GRAY + "━";
            }
        }
        return out + Colors::RESET;
    };

    if (animate && can_use_terminal_keys() && filled > 0) {
        std::string indent = left_indent();
        for (int step = 0; step <= filled; ++step) {
            std::cout << "\r" << indent << "  " << render(step) << "  " << Colors::WHITE
                      << Colors::BOLD << static_cast<int>(static_cast<double>(step) * 100.0 / slots)
                      << "%" << Colors::RESET << "\033[K";
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::duration<double>(0.018));
        }
        std::cout << "\r\033[K";
        std::cout.flush();
    }
    return render(filled) + "  " + Colors::WHITE + Colors::BOLD + std::to_string(pct) + "%" +
           Colors::RESET;
}

std::string sparkle_line(int width) {
    int w = width;
    if (w <= 0) w = term_width();
    std::string out;
    int rn = static_cast<int>(FLAIR_RAMP.size());
    int icn = static_cast<int>(CELEBRATION_ICONS.size());
    for (int i = 0; i < w; ++i) {
        if (rand01() < 0.10) {
            int shade = FLAIR_RAMP[std::uniform_int_distribution<int>(0, rn - 1)(rng())];
            const std::string& icon =
                CELEBRATION_ICONS[std::uniform_int_distribution<int>(0, icn - 1)(rng())];
            out += "\033[38;5;" + std::to_string(shade) + "m" + icon;
        } else {
            out += " ";
        }
    }
    return out + Colors::RESET;
}

void celebrate(const std::string& title, const char* subtitle, const char* style) {
    std::string indent = left_indent();
    int width = term_width();
    const char* st = or_default(style, Colors::GREEN);
    std::string bar = std::string(st) + Colors::BOLD + repeat_str("━", width) + Colors::RESET;
    auto centered = [&](const std::string& s) -> std::string {
        int half = (width - static_cast<int>(clean_len(s))) / 2;
        if (half < 0) half = 0;
        return std::string(static_cast<std::size_t>(half), ' ') + s;
    };
    std::cout << "\n";
    if (can_use_terminal_keys()) {
        for (int i = 0; i < 3; ++i) {
            std::cout << "\r" << indent << sparkle_line(width);
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::duration<double>(0.09));
        }
        std::cout << "\r\033[K";
    }
    std::cout << indent << bar << "\n";
    std::cout << indent << centered(gradient_text(std::string("  ") + title + "  ")) << "\n";
    if (subtitle && subtitle[0]) {
        std::cout << indent
                  << centered(std::string(Colors::DIM) + Colors::GRAY + subtitle + Colors::RESET)
                  << "\n";
    }
    std::cout << indent << bar << "\n";
    std::cout << "\n";
}

std::string mode_value(bool enabled, const std::string& on, const std::string& off) {
    if (enabled)
        return std::string(ACCENT) + "●" + Colors::RESET + " " + Colors::WHITE + on +
               Colors::RESET;
    return std::string(Colors::DIM) + Colors::GRAY + "○ " + off + Colors::RESET;
}

std::string soft_rule(const char* style) {
    const char* st = or_default(style, Colors::GRAY);
    return left_indent() + st + Colors::DIM + repeat_str("─", term_width()) + Colors::RESET;
}

std::string kv_row(const std::string& label, const std::string& value, int label_width) {
    int pad = std::max(0, label_width - static_cast<int>(clean_len(label)));
    return std::string(Colors::DIM) + Colors::GRAY + label + Colors::RESET +
           std::string(static_cast<std::size_t>(pad), ' ') + "  " + value;
}

std::string truncate_visible(const std::string& text_in, int limit) {
    if (static_cast<int>(clean_len(text_in)) <= limit)
        return text_in;
    std::vector<std::string> chars = utf8_chars(text_in);
    std::string out;
    int shown = 0;
    std::size_t i = 0, n = chars.size();
    while (i < n && shown < limit - 1) {
        if (chars[i] == "\033") {

            std::size_t j = std::string::npos;
            for (std::size_t k = i; k < n; ++k) {
                if (chars[k] == "m") {
                    j = k;
                    break;
                }
            }
            if (j == std::string::npos) break;
            for (std::size_t k = i; k <= j; ++k) out += chars[k];
            i = j + 1;
            continue;
        }
        out += chars[i];
        ++shown;
        ++i;
    }
    out += std::string("…") + Colors::RESET;
    return out;
}

std::vector<std::string> panel_lines(const std::string& title,
                                     const std::vector<std::string>& lines,
                                     const char* style) {
    int width = term_width();
    int inner_width = width - 4;
    std::string indent = left_indent();
    std::vector<std::string> out;
    out.push_back(indent + frame_title(title, style));
    for (const std::string& line : lines) {
        std::vector<std::string> chunks = py_splitlines(line);
        if (chunks.empty()) chunks = {""};
        for (std::string chunk : chunks) {
            chunk = truncate_visible(chunk, inner_width);
            int padn = std::max(0, inner_width - static_cast<int>(clean_len(chunk)));
            out.push_back(indent + style + Colors::BOLD + "│" + Colors::RESET + " " + chunk +
                          std::string(static_cast<std::size_t>(padn), ' ') + " " + style +
                          Colors::BOLD + "│" + Colors::RESET);
        }
    }
    out.push_back(indent + frame_bottom(style));
    return out;
}

void print_panel(const std::string& title, const std::vector<std::string>& lines,
                 const char* style) {
    for (const std::string& line : panel_lines(title, lines, style))
        std::cout << line << "\n";
}

void print_frame_line(const std::string& text, const char* style) {
    int inner_width = term_width() - 4;
    std::string indent = left_indent();
    std::string cleaned = clean_ansi(text);
    cleaned = replace_all(cleaned, "\r", "\n");
    cleaned = replace_all(cleaned, "\t", "    ");

    std::u32string u = utf8_to_u32(cleaned);
    std::u32string filtered;
    filtered.reserve(u.size());
    for (char32_t ch : u)
        filtered.push_back((ch == U'\n' || ch >= 32) ? ch : U' ');
    std::string safe = u32_to_utf8(filtered);

    std::vector<std::string> lines = split_newline(safe);
    if (lines.empty()) lines = {""};
    for (const std::string& ln : lines) {
        if (ln.empty()) {
            std::cout << indent << style << Colors::BOLD << "│" << Colors::RESET << " "
                      << std::string(static_cast<std::size_t>(std::max(0, inner_width)), ' ')
                      << " " << style << Colors::BOLD << "│" << Colors::RESET << "\n";
            continue;
        }
        std::string line = ln;
        while (!line.empty()) {
            std::string chunk = utf8_substr(line, 0, static_cast<std::size_t>(inner_width));
            line = utf8_substr(line, static_cast<std::size_t>(inner_width));
            int padn = std::max(0, inner_width - static_cast<int>(clean_len(chunk)));
            std::cout << indent << style << Colors::BOLD << "│" << Colors::RESET << " "
                      << chunk << std::string(static_cast<std::size_t>(padn), ' ') << " " << style
                      << Colors::BOLD << "│" << Colors::RESET << "\n";
        }
    }
}

void print_frame_text(const std::string& text, const char* style) {
    std::string normalized = replace_all(text, "\r", "\n");
    std::vector<std::string> parts = split_newline(normalized);
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index == parts.size() - 1 && parts[index].empty())
            continue;
        print_frame_line(parts[index], style);
    }
}

bool can_use_terminal_keys() {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

std::string raw_text(const std::string& text) {
    return replace_all(text, "\n", "\r\n");
}

}
