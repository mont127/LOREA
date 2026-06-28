#include "widgets.hpp"
#include "ansi.hpp"
#include "terminal.hpp"
#include "render.hpp"
#include "interrupt.hpp"
#include "live_view.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <termios.h>
#include <unistd.h>

namespace ocli {

namespace {

struct ScopeGuard {
    std::function<void()> fn;
    explicit ScopeGuard(std::function<void()> f) : fn(std::move(f)) {}
    ~ScopeGuard() { if (fn) fn(); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
};

void set_raw(int fd) {
    struct termios mode;
    tcgetattr(fd, &mode);
    mode.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    mode.c_oflag &= ~(OPOST);
    mode.c_cflag &= ~(CSIZE | PARENB);
    mode.c_cflag |= CS8;
    mode.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSAFLUSH, &mode);
}

int pymod(int a, int b) { int r = a % b; if (r < 0) r += b; return r; }

long py_round_half_even(double x) {
    double fl = std::floor(x);
    double diff = x - fl;
    long f = (long)fl;
    if (diff < 0.5) return f;
    if (diff > 0.5) return f + 1;

    return (f % 2 == 0) ? f : f + 1;
}

double py_float(const std::string& s, bool& ok) {
    ok = false;
    if (s.empty()) return 0.0;
    try {
        std::size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos != s.size()) return 0.0;
        ok = true;
        return v;
    } catch (...) {
        return 0.0;
    }
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
    return out;
}

std::string ljust(const std::string& s, int w) {
    int n = (int)utf8_len(s);
    if (n >= w) return s;
    return s + std::string(w - n, ' ');
}

std::string rjust(const std::string& s, int w) {
    int n = (int)utf8_len(s);
    if (n >= w) return s;
    return std::string(w - n, ' ') + s;
}

std::string strip(const std::string& s) {
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t a = 0, b = s.size();
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool is_digit_str(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::string lstrip_char(const std::string& s, char ch) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ch) ++i;
    return s.substr(i);
}

std::vector<std::string> py_split_newline(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string last_line_after_newline(const std::string& s) {
    auto pos = s.rfind('\n');
    if (pos == std::string::npos) return s;
    return s.substr(pos + 1);
}

std::size_t count_splitlines(const std::string& s) {
    std::u32string u = utf8_to_u32(s);
    std::size_t n = u.size(), i = 0, count = 0;
    auto is_break = [](char32_t c) {
        return c == U'\n' || c == U'\r' || c == 0x0b || c == 0x0c ||
               c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85 ||
               c == 0x2028 || c == 0x2029;
    };
    while (i < n) {
        while (i < n && !is_break(u[i])) ++i;
        ++count;
        if (i < n) {
            if (u[i] == U'\r' && i + 1 < n && u[i + 1] == U'\n') i += 2;
            else ++i;
        }
    }
    return count;
}

bool cp_is_printable(uint32_t cp) {
    if (cp == 0x20) return true;
    if (cp < 0x20) return false;
    if (cp >= 0x7f && cp <= 0xa0) return false;
    if (cp == 0x1680) return false;
    if (cp >= 0x2000 && cp <= 0x200a) return false;
    if (cp == 0x2028 || cp == 0x2029) return false;
    if (cp == 0x202f || cp == 0x205f || cp == 0x3000) return false;
    if (cp >= 0x200b && cp <= 0x200f) return false;
    if (cp >= 0x202a && cp <= 0x202e) return false;
    if (cp >= 0x2060 && cp <= 0x2064) return false;
    if (cp >= 0x2066 && cp <= 0x206f) return false;
    if (cp == 0xfeff) return false;
    if (cp >= 0xfff9 && cp <= 0xfffb) return false;
    return true;
}

bool str_is_printable(const std::string& s) {
    std::u32string u = utf8_to_u32(s);
    for (char32_t c : u) if (!cp_is_printable((uint32_t)c)) return false;
    return true;
}

std::string py_input(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("EOFError");
    return line;
}

}

std::pair<std::vector<std::string>, std::vector<std::pair<std::string, std::string>>>
slash_palette_lines(const std::string& query, int selected, const char* style, int limit) {
    std::string q = to_lower_ascii(lstrip_char(query.empty() ? std::string() : query, '/'));

    std::vector<std::pair<std::string, std::string>> prefix;
    for (const auto& c : SLASH_COMMANDS) {
        std::string nm = to_lower_ascii(lstrip_char(c.first, '/'));
        if (starts_with(nm, q)) prefix.push_back(c);
    }
    std::vector<std::pair<std::string, std::string>> matches;
    if (!prefix.empty()) {
        matches = prefix;
    } else {
        for (const auto& c : SLASH_COMMANDS) {
            std::string nm = to_lower_ascii(lstrip_char(c.first, '/'));
            if (contains(nm, q)) matches.push_back(c);
        }
    }

    int width = term_width();
    int inner = width - 4;
    std::string indent = left_indent();

    if (matches.empty()) {
        std::string msg = truncate_visible(
            std::string(Colors::DIM) + Colors::GRAY + "no match for '/" + q + "'" + Colors::RESET,
            inner);
        std::string pad(std::max(0, inner - (int)clean_len(msg)), ' ');
        std::vector<std::string> out;
        out.push_back(indent + frame_title("commands", style));
        out.push_back(indent + std::string(style) + Colors::BOLD + "│" + Colors::RESET + " " +
                      msg + pad + " " + style + Colors::BOLD + "│" + Colors::RESET);
        out.push_back(indent + frame_bottom(style, "esc close"));
        return {out, matches};
    }

    int sel = std::max(0, std::min(selected, (int)matches.size() - 1));
    std::vector<std::pair<std::string, std::string>> view;
    {
        int e = std::min((int)matches.size(), limit);
        for (int i = 0; i < e; ++i) view.push_back(matches[i]);
    }
    int base;

    if (sel >= limit) {
        view.clear();
        int s = sel - limit + 1, e = sel + 1;
        s = std::max(0, std::min(s, (int)matches.size()));
        e = std::max(0, std::min(e, (int)matches.size()));
        for (int i = s; i < e; ++i) view.push_back(matches[i]);
        base = sel - limit + 1;
    } else {
        base = 0;
    }

    std::vector<std::string> out;
    {
        std::string sub = std::to_string(matches.size());
        out.push_back(indent + frame_title("commands", style, sub.c_str()));
    }
    int name_w = 0;
    for (const auto& c : matches) name_w = std::max(name_w, (int)utf8_len(c.first));

    for (int i = 0; i < (int)view.size(); ++i) {
        const std::string& name = view[i].first;
        const std::string& desc = view[i].second;
        int idx = base + i;
        std::string row = ljust(name, name_w) + "  " + desc;
        row = truncate_visible(row, inner - 2);
        if (idx == sel) {
            std::string pad(std::max(0, inner - (int)clean_len(row) - 2), ' ');
            std::string entry = std::string(Colors::BG_DARK) + ACCENT + Colors::BOLD + "❯ " +
                                Colors::WHITE + row + pad + Colors::RESET;
            out.push_back(indent + std::string(style) + Colors::BOLD + "│" + Colors::RESET +
                          Colors::BG_DARK + " " + entry + " " + Colors::RESET + style +
                          Colors::BOLD + "│" + Colors::RESET);
        } else {
            std::string entry = std::string(Colors::TEAL) + name + Colors::RESET + Colors::DIM +
                                Colors::GRAY +
                                std::string(std::max(0, name_w - (int)utf8_len(name)), ' ') +
                                "  " + desc + Colors::RESET;
            entry = truncate_visible(entry, inner - 2);
            std::string pad(std::max(0, inner - (int)clean_len(entry) - 2), ' ');
            out.push_back(indent + std::string(style) + Colors::BOLD + "│" + Colors::RESET +
                          "   " + entry + pad + " " + style + Colors::BOLD + "│" + Colors::RESET);
        }
    }
    std::string hint = (width >= 44) ? "↑↓ · ⇥/⏎ complete · esc close" : "⇥ complete";
    out.push_back(indent + frame_bottom(style, hint.c_str()));
    return {out, matches};
}

namespace {

struct StyledInput {
    std::string prompt;
    std::function<std::string()> header_fn;
    std::function<std::string()> prompt_fn;

    std::vector<Unit> units;
    std::vector<std::string> hist;
    int hist_index = 0;
    std::optional<std::string> hist_stash;
    int cursor = 0;
    std::string last_prompt;
    std::vector<int> header_lines_vis;
    int prev_input_rows = 1;
    bool palette_open = false;
    int palette_sel = 0;
    int prev_palette_rows = 0;

    struct termios old_settings{};

    StyledInput(const std::string& prompt_, const std::string& default_value,
                std::function<std::string()> hf, std::function<std::string()> pf,
                const std::vector<std::string>* history)
        : prompt(prompt_), header_fn(std::move(hf)), prompt_fn(std::move(pf)) {
        for (const auto& ch : utf8_chars(default_value)) units.push_back(Unit{ch, ch});
        if (history) hist = *history;
        hist_index = (int)hist.size();
        cursor = (int)units.size();
        last_prompt = last_line_after_newline(prompt);
        if (header_fn) {
            for (const auto& line : py_split_newline(header_fn()))
                header_lines_vis.push_back((int)clean_len(line));
        }
    }

    std::string display_text() {
        std::string s;
        for (const auto& u : units) s += u.display;
        return s;
    }
    std::string actual_text() {
        std::string s;
        for (const auto& u : units) s += u.actual;
        return s;
    }
    int display_len(int start, int end) {
        std::string s;
        for (int i = start; i < end; ++i) s += units[i].display;
        return (int)clean_len(s);
    }

    int row_count(int visible_cols, int cols) {
        cols = std::max(1, cols);
        return visible_cols > 0 ? ((visible_cols - 1) / cols + 1) : 1;
    }
    int header_rows(int cols) {
        int total = 0;
        for (int vis : header_lines_vis) total += row_count(vis, cols);
        return total;
    }
    int input_rows(int cols) {
        int total = (int)clean_len(last_prompt) + display_len(0, (int)units.size());
        return row_count(total, cols);
    }

    void clear_input_block() {
        int rows = prev_input_rows;
        if (rows > 1) std::cout << "\033[" << (rows - 1) << "A";
        std::cout << "\r\033[J";
    }

    void place_cursor(int cols) {
        int before = (int)clean_len(last_prompt) + display_len(0, cursor);
        int after = (int)clean_len(last_prompt) + display_len(0, (int)units.size());
        if (after == before) return;
        int end_row = after / cols, end_col = after % cols;
        int cur_row = before / cols, cur_col = before % cols;

        if (end_col == 0 && after > 0) { end_row -= 1; end_col = cols; }
        if (cur_col == 0 && before > 0) { cur_row -= 1; cur_col = cols; }
        int up = end_row - cur_row;
        if (up > 0) std::cout << "\033[" << up << "A";
        if (cur_col < end_col) std::cout << "\033[" << (end_col - cur_col) << "D";
        else if (cur_col > end_col) std::cout << "\033[" << (cur_col - end_col) << "C";
    }

    std::pair<int, int> caret_rowcol(int cols) {
        int before = (int)clean_len(last_prompt) + display_len(0, cursor);
        int cur_row = before / cols, cur_col = before % cols;
        if (cur_col == 0 && before > 0) { cur_row -= 1; cur_col = cols; }
        return {cur_row, cur_col};
    }

    std::optional<std::vector<std::string>> current_palette() {
        std::string text = actual_text();
        if (!palette_open || !starts_with(text, "/") || contains(text, " ") || contains(text, "\n"))
            return std::nullopt;
        auto res = slash_palette_lines(text, palette_sel);
        return res.first;
    }

    void redraw(bool repaint_header = false) {
        int cols = std::max(1, term_cols());
        if (prompt_fn) last_prompt = last_line_after_newline(prompt_fn());
        std::string rendered = display_text();
        if (repaint_header && header_fn) {

            std::string header = header_fn();
            std::cout << "\033[2J\033[H";
            std::cout << replace_all(header, "\n", "\r\n") << "\r\n";
            header_lines_vis.clear();
            for (const auto& line : py_split_newline(header))
                header_lines_vis.push_back((int)clean_len(line));
        } else {
            clear_input_block();
        }
        std::cout << "\r" << last_prompt << rendered;
        int in_rows = input_rows(cols);
        prev_input_rows = in_rows;

        auto palette = current_palette();
        if (palette.has_value()) {
            const std::vector<std::string>& pal_lines = *palette;
            std::cout << "\r\n" << join(pal_lines, "\r\n");
            prev_palette_rows = (int)pal_lines.size();
            int total = (int)clean_len(last_prompt) + display_len(0, (int)units.size());
            int input_end_row = total / cols;
            if (total && total % cols == 0) input_end_row -= 1;
            auto [cur_row, cur_col] = caret_rowcol(cols);
            int up = (input_end_row + (int)pal_lines.size()) - cur_row;
            std::cout << "\r";
            if (up > 0) std::cout << "\033[" << up << "A";
            if (cur_col > 0) std::cout << "\033[" << cur_col << "C";
        } else {
            prev_palette_rows = 0;
            place_cursor(cols);
        }
        std::cout.flush();
    }

    void insert_unit(const std::string& display, const std::string& actual) {
        units.insert(units.begin() + cursor, Unit{display, actual});
        ++cursor;
        sync_palette();
        redraw();
    }

    void insert_text(const std::string& value) {
        for (const auto& ch : utf8_chars(value)) {
            std::string display, actual;
            if (ch == "\r" || ch == "\n") {
                display = std::string(Colors::GRAY) + "↵" + Colors::RESET;
                actual = "\n";
            } else if (str_is_printable(ch) || ch == "\t") {
                display = (ch == "\t") ? std::string("    ") : ch;
                actual = ch;
            } else {
                continue;
            }
            units.insert(units.begin() + cursor, Unit{display, actual});
            ++cursor;
        }
        sync_palette();
        redraw();
    }

    void insert_paste(const std::string& value) {
        std::string normalized = replace_all(replace_all(value, "\r\n", "\n"), "\r", "\n");
        int lines = (int)count_splitlines(normalized);
        if (lines == 0) lines = 1;
        if (lines > 2) {
            insert_unit(std::string(Colors::ORANGE) + "[PASTED " + std::to_string(lines) +
                            " LINES]" + Colors::RESET,
                        normalized);
        } else {
            insert_text(normalized);
        }
    }

    void sync_palette() {
        std::string text = actual_text();
        bool should = starts_with(text, "/") && !contains(text, " ") && !contains(text, "\n");
        if (should) {
            palette_open = true;
            auto res = slash_palette_lines(text, palette_sel);
            if (palette_sel >= (int)res.second.size()) palette_sel = 0;
        } else {
            palette_open = false;
            palette_sel = 0;
        }
    }

    bool accept_palette() {
        std::string text = actual_text();
        auto res = slash_palette_lines(text, palette_sel);
        auto& matches = res.second;
        if (matches.empty()) return false;
        int idx = std::max(0, std::min(palette_sel, (int)matches.size() - 1));
        std::string name = matches[idx].first;
        std::string value = name + " ";
        units.clear();
        for (const auto& c : utf8_chars(value)) units.push_back(Unit{c, c});
        cursor = (int)units.size();
        palette_open = false;
        palette_sel = 0;
        redraw();
        return true;
    }

    void set_units_history(const std::string& s) {
        units.clear();
        for (const auto& ch : utf8_chars(s)) {
            std::string d;
            if (ch == "\t") d = "    ";
            else if (ch == "\n") d = std::string(Colors::GRAY) + "↵" + Colors::RESET;
            else d = ch;
            units.push_back(Unit{d, ch});
        }
    }

    std::string run() {
        int fd = STDIN_FILENO;
        tcgetattr(fd, &old_settings);
        ScopeGuard g([&] {
            std::cout << "\033[?2004l";
            std::cout.flush();
            tcsetattr(fd, TCSADRAIN, &old_settings);
        });
        set_raw(fd);
        std::cout << "\033[?2004h";
        std::cout << raw_text(prompt) << display_text();
        prev_input_rows = input_rows(std::max(1, term_cols()));
        place_cursor(std::max(1, term_cols()));
        std::cout.flush();

        while (true) {
            wait_for_key_or_resize(fd, [&] { redraw(true); });
            std::string key = read_key();
            if (key == "\x1b[200~") {
                insert_paste(read_bracketed_paste(fd));
                sync_palette();
                redraw();
                continue;
            }

            if (palette_open) {
                auto pres = slash_palette_lines(actual_text(), palette_sel);
                auto& pmatches = pres.second;
                if (key == "\x1b[A" || key == "\x10") {
                    if (!pmatches.empty()) {
                        palette_sel = pymod(palette_sel - 1, (int)pmatches.size());
                        redraw();
                    }
                    continue;
                }
                if (key == "\x1b[B" || key == "\x0e") {
                    if (!pmatches.empty()) {
                        palette_sel = pymod(palette_sel + 1, (int)pmatches.size());
                        redraw();
                    }
                    continue;
                }
                if (key == "\t") {
                    accept_palette();
                    continue;
                }
                if (key == "\r" || key == "\n") {

                    if (!pmatches.empty() && accept_palette()) continue;
                }
                if (key == "\x1b") {
                    palette_open = false;
                    palette_sel = 0;
                    redraw();
                    continue;
                }
            }
            if (key == "\r" || key == "\n") {

                int cols = std::max(1, term_cols());
                int end_total = (int)clean_len(last_prompt) + display_len(0, (int)units.size());
                int cur_total = (int)clean_len(last_prompt) + display_len(0, cursor);
                int end_row = end_total / cols - ((end_total && end_total % cols == 0) ? 1 : 0);
                int cur_row = cur_total / cols - ((cur_total && cur_total % cols == 0) ? 1 : 0);
                int down = end_row - cur_row;
                if (down > 0) std::cout << "\033[" << down << "B";
                std::cout << "\r\n";
                std::cout.flush();
                return actual_text();
            }
            if (key == "\x03") {
                throw std::runtime_error("KeyboardInterrupt");
            }
            if (key == "\x04") {
                if (units.empty()) throw std::runtime_error("EOFError");
                continue;
            }
            if (key == "\x7f" || key == "\b") {
                if (cursor > 0) {
                    units.erase(units.begin() + (cursor - 1));
                    --cursor;
                    sync_palette();
                    redraw();
                }
                continue;
            }
            if (key == "\x1b[A" || key == "\x10") {
                if (!hist.empty() && hist_index > 0) {
                    if (hist_index == (int)hist.size()) hist_stash = actual_text();
                    --hist_index;
                    set_units_history(hist[hist_index]);
                    cursor = (int)units.size();
                    sync_palette();
                    redraw();
                }
                continue;
            }
            if (key == "\x1b[B" || key == "\x0e") {
                if (!hist.empty() && hist_index < (int)hist.size()) {
                    ++hist_index;
                    std::string restore;
                    if (hist_index == (int)hist.size()) restore = hist_stash.value_or("");
                    else restore = hist[hist_index];
                    set_units_history(restore);
                    cursor = (int)units.size();
                    sync_palette();
                    redraw();
                }
                continue;
            }
            if (key == "\x1b[D" || key == "\x02") {
                if (cursor > 0) { --cursor; redraw(); }
                continue;
            }
            if (key == "\x1b[C" || key == "\x06") {
                if (cursor < (int)units.size()) { ++cursor; redraw(); }
                continue;
            }
            if (key == "\x1b[H" || key == "\x1b[1~" || key == "\x01") {
                cursor = 0;
                redraw();
                continue;
            }
            if (key == "\x1b[F" || key == "\x1b[4~" || key == "\x05") {
                cursor = (int)units.size();
                redraw();
                continue;
            }
            if (starts_with(key, "\x1b[3") && cursor < (int)units.size()) {
                units.erase(units.begin() + cursor);
                sync_palette();
                redraw();
                continue;
            }
            if (utf8_len(key) == 1 && str_is_printable(key)) {
                insert_unit(key, key);
            }
        }
    }
};

}

std::string styled_input(const std::string& prompt, const std::string& default_value,
                         std::function<std::string()> header_fn,
                         std::function<std::string()> prompt_fn,
                         const std::vector<std::string>* history) {
    LiveSuspendGuard live_guard;
    if (!can_use_terminal_keys()) {
        return py_input(prompt);
    }
    StyledInput ed(prompt, default_value, std::move(header_fn), std::move(prompt_fn), history);
    return ed.run();
}

std::vector<std::string> menu_lines(const std::string& title,
                                    const std::vector<std::string>& options, int selected,
                                    const char* style, int offset, int limit) {
    int width = term_width();
    int inner_width = width - 4;
    std::vector<std::string> visible;
    if (limit) {
        int s = std::max(0, std::min(offset, (int)options.size()));
        int e = std::max(0, std::min(offset + limit, (int)options.size()));
        for (int i = s; i < e; ++i) visible.push_back(options[i]);
    } else {
        visible = options;
    }
    std::string indent = left_indent();
    std::vector<std::string> lines;
    lines.push_back(indent + frame_title(title, style));
    for (int visible_index = 0; visible_index < (int)visible.size(); ++visible_index) {
        const std::string& option = visible[visible_index];
        int actual_index = offset + visible_index;
        if (actual_index == selected) {

            std::string text = clean_ansi(option);
            if ((int)clean_len(text) > inner_width - 2) {
                text = utf8_substr(text, 0, (std::size_t)std::max(0, inner_width - 3)) + "…";
            }
            std::string core = std::string("❯ ") + text;
            std::string pad(std::max(0, inner_width - (int)clean_len(core)), ' ');
            std::string entry = std::string(Colors::BG_DARK) + ACCENT + Colors::BOLD + "❯ " +
                                Colors::WHITE + text + pad + Colors::RESET;
            lines.push_back(indent + std::string(style) + Colors::BOLD + "│" + Colors::RESET +
                            Colors::BG_DARK + " " + entry + " " + Colors::RESET + style +
                            Colors::BOLD + "│" + Colors::RESET);
            continue;
        }
        std::string entry = truncate_visible(
            std::string(Colors::DIM) + Colors::GRAY + "  " + option + Colors::RESET, inner_width);
        std::string pad(std::max(0, inner_width - (int)clean_len(entry)), ' ');
        lines.push_back(indent + std::string(style) + Colors::BOLD + "│" + Colors::RESET + " " +
                        entry + pad + " " + style + Colors::BOLD + "│" + Colors::RESET);
    }
    lines.push_back(indent + frame_bottom(style));

    std::string hint_base;
    if (width < 44) {
        hint_base = std::string(Colors::DIM) + Colors::GRAY + "↑↓ · ⏎ · esc" + Colors::RESET;
    } else {
        hint_base = std::string(Colors::DIM) + Colors::GRAY +
                    "↑↓ move · ⏎ select · 0/Esc cancel" + Colors::RESET;
    }
    if (limit && (int)options.size() > limit) {
        lines.push_back(indent + hint_base + "  " + Colors::DIM + Colors::GRAY + "· " +
                        std::to_string(offset + 1) + "-" +
                        std::to_string(offset + (int)visible.size()) + "/" +
                        std::to_string(options.size()) + Colors::RESET);
    } else {
        lines.push_back(indent + hint_base);
    }
    return lines;
}

std::optional<int> interactive_menu(const std::string& title,
                                    const std::vector<std::string>& options, const char* style) {
    LiveSuspendGuard live_guard;
    if (!can_use_terminal_keys()) {
        std::vector<std::string> panel = options;
        panel.push_back(std::string(Colors::GRAY) + "0. Cancel" + Colors::RESET);
        print_panel(title, panel, style);
        while (true) {
            std::string choice = strip(py_input(std::string("  ") + style + Colors::BOLD +
                                                 "Select:" + Colors::RESET + " "));
            if (choice == "" || choice == "0" || choice == "q" || choice == "quit" ||
                choice == "cancel")
                return std::nullopt;
            if (is_digit_str(choice)) {
                int v = std::stoi(choice);
                if (v >= 1 && v <= (int)options.size()) return v - 1;
            }
            log_info("Pick a listed number or 0 to cancel.");
        }
    }

    int fd = STDIN_FILENO;
    struct termios old{};
    tcgetattr(fd, &old);
    std::vector<std::string> entries = options;
    entries.push_back(std::string(Colors::GRAY) + "0. Cancel" + Colors::RESET);
    int selected = 0;
    int offset = 0;
    std::vector<std::string> last_lines;

    auto phys_rows = [&](const std::vector<std::string>& lines) -> int {
        int cols = std::max(1, term_cols());
        int total = 0;
        for (const auto& line : lines) {
            int vis = (int)clean_len(line);
            total += vis > 0 ? ((vis - 1) / cols + 1) : 1;
        }
        return total;
    };
    auto clear_menu = [&]() {
        if (last_lines.empty()) return;
        std::cout << "\033[" << phys_rows(last_lines) << "F\033[J";
        std::cout.flush();
        last_lines.clear();
    };
    auto finish = [&](std::optional<int> v) -> std::optional<int> {
        clear_menu();
        std::cout << "\033[?25h";
        std::cout.flush();
        return v;
    };
    auto draw = [&]() {
        if (!last_lines.empty()) std::cout << "\033[" << phys_rows(last_lines) << "F\033[J";
        int rows = term_rows();
        int visible_limit = std::max(6, std::min(16, rows - 8));
        if (selected < offset) offset = selected;
        else if (selected >= offset + visible_limit) offset = selected - visible_limit + 1;
        std::vector<std::string> lines = menu_lines(title, entries, selected, style, offset,
                                                    visible_limit);
        std::cout << "\r" << join(lines, "\r\n") << "\r\n";
        last_lines = lines;
        std::cout.flush();
    };

    ScopeGuard g([&] {
        clear_menu();
        std::cout << "\033[?25h";
        std::cout.flush();
        tcsetattr(fd, TCSADRAIN, &old);
    });

    set_raw(fd);
    std::cout << "\033[?25l";
    draw();
    while (true) {
        wait_for_key_or_resize(fd, draw);
        std::string key = read_key();
        if (key == "\x03" || key == "\x04") throw std::runtime_error("KeyboardInterrupt");
        if (key == "\r" || key == "\n")
            return finish(selected == (int)options.size() ? std::optional<int>{}
                                                          : std::optional<int>{selected});
        if (key == "\x1b" || key == "q" || key == "Q" || key == "0") return finish(std::nullopt);
        if (key == "\x1b[A" || key == "k" || key == "K") {
            selected = pymod(selected - 1, (int)entries.size());
            draw();
            continue;
        }
        if (key == "\x1b[B" || key == "j" || key == "J") {
            selected = pymod(selected + 1, (int)entries.size());
            draw();
            continue;
        }
        if (is_digit_str(key)) {
            int v = std::stoi(key);
            if (v >= 1 && v <= (int)options.size()) return finish(v - 1);
        }
    }
}

std::optional<double> interactive_slider(const std::string& title, double value, double minimum,
                                         double maximum, double step, const std::string& unit,
                                         const char* style, const std::string& hint,
                                         std::function<std::string(double)> fmt,
                                         const std::vector<std::pair<double, std::string>>* marks) {
    LiveSuspendGuard live_guard;
    if (maximum <= minimum) return value;
    if (!fmt) {
        fmt = [unit](double v) { return std::to_string(py_round_half_even(v)) + unit; };
    }

    auto clamp = [&](double v) -> double {
        v = std::max(minimum, std::min(maximum, v));
        if (step != 0.0) v = minimum + (double)py_round_half_even((v - minimum) / step) * step;
        return std::max(minimum, std::min(maximum, v));
    };

    value = clamp(value);

    if (!can_use_terminal_keys()) {
        print_panel(title,
                    {std::string("Range ") + fmt(minimum) + " – " + fmt(maximum) +
                         "  ·  current " + fmt(value),
                     std::string(Colors::DIM) + Colors::GRAY +
                         "Enter a value, or blank to keep current." + Colors::RESET},
                    style);
        std::string raw = strip(py_input(std::string("  ") + style + Colors::BOLD + "Value:" +
                                         Colors::RESET + " "));
        if (raw.empty()) return value;
        std::string filtered;
        for (char c : raw)
            if ((c >= '0' && c <= '9') || c == '.' || c == '-') filtered += c;
        bool ok = false;
        double f = py_float(filtered, ok);
        if (!ok) return value;
        return clamp(f);
    }

    int fd = STDIN_FILENO;
    struct termios old{};
    tcgetattr(fd, &old);
    std::string indent = left_indent();
    double big = std::max(step, (maximum - minimum) / 10.0);

    int track_w = std::max(20, std::min(60, term_width() - 24));
    int last_lines = 0;
    int track_col0 = 0;

    auto bar = [&](double v) -> std::string {
        int filled = (int)py_round_half_even((v - minimum) / (maximum - minimum) * (track_w - 1));
        std::string s;
        for (int i = 0; i < track_w; ++i) {
            if (i == filled) s += std::string(style) + Colors::BOLD + "●" + Colors::RESET;
            else if (i < filled) s += std::string(style) + "━" + Colors::RESET;
            else s += std::string(Colors::DIM) + Colors::GRAY + "─" + Colors::RESET;
        }
        return s;
    };

    auto render = [&]() -> std::vector<std::string> {
        int pct = (int)py_round_half_even((value - minimum) / (maximum - minimum) * 100.0);
        std::vector<std::string> lines;
        lines.push_back(indent + frame_title(title, style));
        std::string track_line = indent + "  " + bar(value) + "  " + Colors::WHITE + Colors::BOLD +
                                 fmt(value) + Colors::RESET + " " + Colors::DIM + Colors::GRAY +
                                 "(" + std::to_string(pct) + "%)" + Colors::RESET;
        lines.push_back(track_line);
        std::string ends = indent + "  " + Colors::DIM + Colors::GRAY + ljust(fmt(minimum), 8) +
                           std::string(std::max(0, track_w - 16), ' ') + rjust(fmt(maximum), 8) +
                           Colors::RESET;
        lines.push_back(ends);
        if (marks && !marks->empty()) {
            std::vector<std::string> seg;
            for (const auto& m : *marks)
                seg.push_back(std::string(Colors::DIM) + Colors::GRAY + fmt(m.first) + "=" +
                              m.second + Colors::RESET);
            lines.push_back(indent + "  " + join(seg, "  "));
        }
        std::string default_hint =
            "drag/click track · ←/→ adjust · ⇧ big step · Home/End · ⏎ apply · Esc cancel";
        lines.push_back(indent + frame_bottom(style, (hint.empty() ? default_hint : hint).c_str()));
        return lines;
    };

    auto value_from_col = [&](int col) -> double {
        int rel = col - track_col0;
        rel = std::max(0, std::min(track_w - 1, rel));
        return clamp(minimum + ((double)rel / (double)(track_w - 1)) * (maximum - minimum));
    };

    auto draw = [&](bool first) {
        std::vector<std::string> lines = render();
        if (!first) std::cout << "\033[" << last_lines << "F\033[J";
        std::cout << "\r" << join(lines, "\r\n") << "\r\n";
        last_lines = (int)lines.size();
        std::cout.flush();
    };

    ScopeGuard g([&] {
        std::cout << "\033[?1000l\033[?1002l\033[?1006l";
        std::cout << "\033[?25h";
        std::cout.flush();
        tcsetattr(fd, TCSADRAIN, &old);
    });

    set_raw(fd);
    std::cout << "\033[?25l";

    std::cout << "\033[?1000h\033[?1002h\033[?1006h";
    draw(true);

    track_col0 = LEFT_MARGIN + 2 + 1;
    bool dragging = false;
    while (true) {
        wait_for_key_or_resize(fd, [&] { draw(false); });
        std::string key = read_key();
        auto mouse = parse_mouse(key);
        if (mouse.has_value()) {
            int btn = mouse->button;
            if (btn == 64) { value = clamp(value + step); draw(false); continue; }
            if (btn == 65) { value = clamp(value - step); draw(false); continue; }
            if (btn == 0 && mouse->pressed) {
                dragging = true;
                value = value_from_col(mouse->col);
                draw(false);
                continue;
            }
            if (btn == 0 && !mouse->pressed) { dragging = false; continue; }
            if (dragging) { value = value_from_col(mouse->col); draw(false); continue; }
            continue;
        }
        if (key == "\r" || key == "\n") return clamp(value);
        if (key == "\x1b" || key == "q" || key == "Q") return std::nullopt;
        if (key == "\x03" || key == "\x04") throw std::runtime_error("KeyboardInterrupt");
        if (key == "\x1b[D" || key == "h" || key == "H") {
            value = clamp(value - step); draw(false); continue;
        }
        if (key == "\x1b[C" || key == "l" || key == "L") {
            value = clamp(value + step); draw(false); continue;
        }
        if (key == "\x1b[5~") { value = clamp(value + big); draw(false); continue; }
        if (key == "\x1b[6~") { value = clamp(value - big); draw(false); continue; }
        if (key == "\x1b[H" || key == "\x1b[1~") { value = minimum; draw(false); continue; }
        if (key == "\x1b[F" || key == "\x1b[4~") { value = maximum; draw(false); continue; }
    }
}

}
