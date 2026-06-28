#include "lorea.hpp"
#include "dashboard.hpp"
#include "pty_session.hpp"
#include "terminal.hpp"
#include "live_view.hpp"

#include <string>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <random>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>

namespace ocli {

namespace {

inline bool is_py_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string strip_py(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && is_py_space(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && is_py_space(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string rstrip_py(const std::string& s) {
    std::size_t b = s.size();
    while (b > 0 && is_py_space(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(0, b);
}

std::string lower_ascii(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

std::string upper_ascii(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains_sub(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

std::string repeat_str(const std::string& s, int n) {
    if (n <= 0) return std::string();
    std::string out;
    out.reserve(s.size() * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

std::string ljust(const std::string& s, int width) {
    int n = static_cast<int>(utf8_len(s));
    if (n >= width) return s;
    return s + std::string(static_cast<std::size_t>(width - n), ' ');
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (is_py_space(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string join_str(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::vector<std::string> tail_from(const std::vector<std::string>& v, std::size_t start) {
    if (start >= v.size()) return std::vector<std::string>();
    return std::vector<std::string>(v.begin() + static_cast<long>(start), v.end());
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

std::vector<std::string> py_splitlines_keepends(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        cur.push_back(c);
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') { cur.push_back('\n'); ++i; }
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

double now_seconds() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

std::string random_choice_local(const std::vector<std::string>& v) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    static std::mt19937 g(std::random_device{}());
    std::uniform_int_distribution<std::size_t> d(0, v.size() - 1);
    return v[d(g)];
}

std::string dashboard_lan_ipv4() {
    struct ifaddrs* ifap = nullptr;
    std::string result = "127.0.0.1";
    if (getifaddrs(&ifap) == 0) {
        for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) != nullptr) {
                std::string s(ip);
                if (!s.empty() && s != "127.0.0.1") {
                    result = s;
                    break;
                }
            }
        }
        freeifaddrs(ifap);
    }
    return result;
}

void attach_shared_terminal() {
    PtySession& term = terminal_session();
    if (!term.alive()) {
        log_warn("Could not start the shared terminal.");
        return;
    }
    g_shared_terminal_active = true;

    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        term.resize(ws.ws_row, ws.ws_col);
    }

    std::cout << "\r\n"
              << Colors::DIM << Colors::GRAY
              << "── shared terminal · you and the AI share this shell · Ctrl-] to detach ──"
              << Colors::RESET << "\r\n"
              << std::flush;

    struct termios saved;
    bool raw_set = false;
    if (::tcgetattr(STDIN_FILENO, &saved) == 0) {
        struct termios raw = saved;
        ::cfmakeraw(&raw);
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) raw_set = true;
    }

    std::size_t cursor = 0;
    {
        std::string history = term.read_since(cursor);
        if (!history.empty()) {
            ssize_t w = ::write(STDOUT_FILENO, history.data(), history.size());
            (void)w;
        }
    }

    bool done = false;
    while (!done && term.alive()) {
        if (RESIZE_FLAG) {
            RESIZE_FLAG = 0;
            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
                term.resize(ws.ws_row, ws.ws_col);
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 30000;
        int rv = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (rv > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[4096];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                std::size_t send_len = static_cast<std::size_t>(n);
                for (ssize_t i = 0; i < n; ++i) {
                    if (buf[i] == 0x1d) {
                        send_len = static_cast<std::size_t>(i);
                        done = true;
                        break;
                    }
                }
                if (send_len > 0) term.write_input(std::string(buf, send_len));
            } else if (n == 0) {
                done = true;
            }
        }

        std::string out = term.read_since(cursor);
        if (!out.empty()) {
            ssize_t w = ::write(STDOUT_FILENO, out.data(), out.size());
            (void)w;
        }
    }

    if (raw_set) ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    std::cout << "\r\n"
              << Colors::DIM << Colors::GRAY << "── detached from shared terminal ──"
              << Colors::RESET << "\r\n"
              << std::flush;
}

const std::string LOREA_CYBER_HF_REPO = "Soaperloafidksum/LOREA-cyber-coder-30B-A3B-v5.1";

std::string fmt_duration(int elapsed) {
    int mins = elapsed / 60, secs = elapsed % 60;
    if (mins) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dm %02ds", mins, secs);
        return std::string(buf);
    }
    return std::to_string(secs) + "s";
}

}

void LOREA::copy_last_answer() {
    std::string text = last_assistant_text();
    if (text.empty()) {
        log_info("No assistant answer to copy yet.");
        return;
    }

    const std::vector<std::vector<std::string>> cmds = {
        {"pbcopy"},
        {"xclip", "-selection", "clipboard"},
        {"wl-copy"},
    };
    for (const auto& cmd : cmds) {
        if (command_on_path(cmd[0])) {
            try {
                ProcResult res = run_subprocess(cmd, text);

                if (!res.started || res.exit_code != 0) continue;
                log_ok("Copied last answer to clipboard (" + std::to_string(utf8_len(text)) + " chars).");
                return;
            } catch (...) {
                continue;
            }
        }
    }
    log_warn("No clipboard tool found (pbcopy/xclip/wl-copy).");
}

bool LOREA::retry_last_turn() {
    if (last_user_goal.empty()) {
        log_info("Nothing to retry yet.");
        return false;
    }

    int cut = -1;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        const Message& msg = messages[static_cast<std::size_t>(i)];
        if (msg_role(msg) == "user" && msg_content(msg) == last_user_goal) {
            cut = i;
            break;
        }
    }
    if (cut < 0) {
        log_info("Could not locate the last turn to retry.");
        return false;
    }
    int removed = static_cast<int>(messages.size()) - cut;
    messages.resize(static_cast<std::size_t>(cut));
    log_ok("Rewound " + std::to_string(removed) + " message(s); retrying last prompt.");
    return true;
}

void LOREA::undo_last_write() {
    if (undo_stack.empty()) {
        log_info("No file writes to undo this session.");
        return;
    }
    UndoEntry entry = undo_stack.back();
    undo_stack.pop_back();
    const std::string& path = entry.path;
    try {
        if (entry.existed) {
            std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!f) throw std::runtime_error("could not open file for writing");
            f << entry.old_content;
            f.flush();
            if (!f) throw std::runtime_error("write failed");
            log_ok("Reverted " + path + " to its previous contents.");
        } else {
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                std::filesystem::remove(path, ec);
                if (ec) throw std::runtime_error(ec.message());
            }
            log_ok("Removed " + path + " (it did not exist before this session).");
        }
        session_files_touched.erase(path);
    } catch (const std::exception& e) {
        log_warn("Undo failed for " + path + ": " + e.what());
    }
}

void LOREA::run_oneoff_shell(const std::string& command_in) {
    std::string command = strip_py(command_in);
    if (command.empty()) {
        log_info("Usage: /cmd <shell command>");
        return;
    }
    if (classify_command(command) == "catastrophic") {
        log_warn("That command looks destructive: " + command);
        std::string confirm = strip_py(styled_input(
            std::string("  ") + Colors::BOLD + "Type " + Colors::RED + "RUN" + Colors::RESET +
            Colors::BOLD + " to execute: " + Colors::RESET));
        if (confirm != "RUN") {
            log_info("Aborted.");
            return;
        }
    }
    std::string indent = left_indent();
    std::string sub = utf8_substr(command, 0, 60);
    std::cout << "\n" << indent << frame_title("SHELL", Colors::MAGENTA, sub.c_str()) << "\n";
    try {
        ProcResult result = run_shell(command, 60.0);
        if (result.timed_out) {
            print_frame_text("(timed out after 60s)", Colors::MAGENTA);
            std::cout << indent << frame_bottom(Colors::MAGENTA) << "\n\n";
        } else {

            std::string body = rstrip_py(result.out);
            if (body.empty()) body = "(no output)";
            std::string hint = "exit " + std::to_string(result.exit_code);
            print_frame_text(truncate_output(body, 8000), Colors::MAGENTA);
            std::cout << indent << frame_bottom(Colors::MAGENTA, hint.c_str()) << "\n\n";
        }
    } catch (const std::exception& e) {
        print_frame_text(std::string("error: ") + e.what(), Colors::MAGENTA);
        std::cout << indent << frame_bottom(Colors::MAGENTA) << "\n\n";
    }
}

void LOREA::show_working_diff(const std::string& path) {
    std::vector<std::string> cmd = {"git", "diff", "--no-color"};
    if (!path.empty()) cmd.push_back(path);
    ProcResult result;
    try {
        result = run_subprocess(cmd, "", 15.0, false);
    } catch (const std::exception& e) {
        log_warn(std::string("git diff failed: ") + e.what());
        return;
    }
    if (!result.started || result.timed_out) {
        log_warn(std::string("git diff failed: ") +
                 (result.timed_out ? "timed out after 15s" : "could not run git"));
        return;
    }
    if (result.exit_code != 0) {
        std::string base = result.err.empty() ? std::string("Not a git repository.") : result.err;
        base = strip_py(base);
        std::size_t nl = base.find('\n');
        std::string first = (nl == std::string::npos) ? base : base.substr(0, nl);
        log_warn(first);
        return;
    }
    std::vector<std::string> diff_lines = py_splitlines_keepends(result.out);
    if (diff_lines.empty()) {
        log_info("Working tree is clean — no changes.");
        return;
    }
    print_diff(diff_lines);
}

void LOREA::print_usage() {
    int elapsed = std::max(0, static_cast<int>(now_seconds() - session_started_at));
    std::string dur = fmt_duration(elapsed);
    int approx_tokens = estimate_context_tokens();
    print_panel("usage", {
        kv_row("session",   std::string(Colors::WHITE) + dur + Colors::RESET),
        kv_row("turns",     std::string(Colors::WHITE) + std::to_string(session_turns) + Colors::RESET),
        kv_row("tool runs", std::string(Colors::WHITE) + std::to_string(session_tools_run) + Colors::RESET),
        kv_row("files",     std::string(Colors::WHITE) + std::to_string(session_files_touched.size()) + Colors::RESET +
                            " " + Colors::DIM + Colors::GRAY + "touched · " +
                            std::to_string(undo_stack.size()) + " undoable" + Colors::RESET),
        kv_row("context",   std::string(Colors::WHITE) + "~" + std::to_string(approx_tokens) + Colors::RESET +
                            " " + Colors::DIM + Colors::GRAY + "tokens · " +
                            std::to_string(messages.size()) + " messages" + Colors::RESET),
        kv_row("compacted", std::string(Colors::WHITE) + std::to_string(compaction_count) + Colors::RESET +
                            " " + Colors::DIM + Colors::GRAY + "times" + Colors::RESET),
    }, MUTED);
}

void LOREA::goodbye() {
    int elapsed = std::max(0, static_cast<int>(now_seconds() - session_started_at));
    std::string dur = fmt_duration(elapsed);
    std::size_t files = session_files_touched.size();
    std::vector<std::string> bits = {
        std::string(Colors::WHITE) + Colors::BOLD + std::to_string(session_turns) + Colors::RESET +
            " " + Colors::DIM + Colors::GRAY + "turns" + Colors::RESET,
        std::string(Colors::WHITE) + Colors::BOLD + std::to_string(session_tools_run) + Colors::RESET +
            " " + Colors::DIM + Colors::GRAY + "tool calls" + Colors::RESET,
        std::string(Colors::WHITE) + Colors::BOLD + std::to_string(files) + Colors::RESET +
            " " + Colors::DIM + Colors::GRAY + "files touched" + Colors::RESET,
        std::string(Colors::WHITE) + Colors::BOLD + dur + Colors::RESET +
            " " + Colors::DIM + Colors::GRAY + "elapsed" + Colors::RESET,
    };
    std::string sep = std::string(" ") + Colors::DIM + Colors::GRAY + "·" + Colors::RESET + " ";
    std::string indent = left_indent();

    std::string title;
    if (session_tools_run == 0 && session_turns == 0) {
        title = "SEE YOU SOON";
    } else if (session_tools_run >= 10 || files >= 3) {
        title = "GREAT SESSION";
    } else {
        title = "NICE WORK";
    }
    celebrate(title, nullptr, ACCENT);
    std::cout << indent << join_str(bits, sep) << "\n";
    auto p = pick_logo_phrase();
    std::string phrase = (p && !p->empty()) ? *p : std::string("until next time");
    std::cout << indent << Colors::DIM << Colors::ITALIC << ACCENT << phrase << Colors::RESET << "\n\n";
}

std::vector<std::string> LOREA::welcome_lines(const std::string& phrase) {
    std::vector<std::string> lines = logo_lines(phrase);
    lines.push_back("");
    std::vector<std::string> panel = panel_lines("session", {
        kv_row("model",   std::string(Colors::WHITE) + model_name + Colors::RESET),
        kv_row("backend", std::string(Colors::WHITE) + backend + Colors::RESET),
        kv_row("mpc",     (mpc_url && !mpc_url->empty())
                              ? (std::string(Colors::WHITE) + *mpc_url + Colors::RESET)
                              : (std::string(Colors::GRAY) + "not connected" + Colors::RESET)),
        kv_row("auto",    mode_value(auto_mode)),
        std::string(""),
        std::string(Colors::DIM) + Colors::GRAY + "/help for commands · exit to quit" + Colors::RESET,
    }, MUTED);
    lines.insert(lines.end(), panel.begin(), panel.end());
    return lines;
}

void LOREA::run() {
    std::optional<std::string> welcome_phrase = pick_logo_phrase();
    bool first_prompt = true;

    auto live_status = [this]() -> std::string {
        std::string m = model_name;
        std::size_t slash = m.find_last_of('/');
        if (slash != std::string::npos) m = m.substr(slash + 1);
        if (m.size() > 32) m = m.substr(0, 31) + "\xE2\x80\xA6";
        std::string s = backend + " · " + m;
        s += std::string("   auto ") + (auto_mode ? "on" : "off");
        s += std::string("   plan ") + (planning_enabled ? "on" : "off");
        return s;
    };
    bool want_live = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO) &&
                     std::getenv("LOREA_CLASSIC") == nullptr;
    if (want_live) {
        live_begin(live_status);
        if (live_active()) {
            std::cout << ACCENT << Colors::BOLD << "LOREA" << Colors::RESET
                      << Colors::DIM << Colors::GRAY
                      << "  ready. Type a prompt below. The panel on the right is the live "
                         "terminal the AI works in."
                      << Colors::RESET << "\n"
                      << Colors::DIM << Colors::GRAY
                      << "  Ctrl-T types into that terminal yourself, PgUp/PgDn scrolls, "
                         "/exit quits."
                      << Colors::RESET << "\n\n";
        }
    }

    while (true) {
        try {
            auto render_bar = [this]() -> std::string {
                int width = term_width();
                std::string indent = left_indent();
                std::string meta_left = " LOREA ";
                std::string mpc_flag = (mpc_url && !mpc_url->empty()) ? " · mpc" : "";
                std::string meta_right = std::string(" ") + backend + ":" + model_name + mpc_flag +
                    " · auto " + (auto_mode ? "on" : "off") +
                    " · plan " + (planning_enabled ? "on" : "off") + " ";
                int fill = width - 4 - static_cast<int>(utf8_len(meta_left)) - static_cast<int>(utf8_len(meta_right));
                if (fill < 2) {
                    int budget = std::max(0, width - 4 - static_cast<int>(utf8_len(meta_left)) - 2);
                    meta_right = utf8_substr(meta_right, 0, static_cast<std::size_t>(budget));
                    fill = std::max(2, width - 4 - static_cast<int>(utf8_len(meta_left)) -
                                       static_cast<int>(utf8_len(meta_right)));
                }
                return std::string(indent) + MUTED + "╭─" + Colors::RESET +
                       ACCENT + Colors::BOLD + meta_left + Colors::RESET +
                       MUTED + repeat_str("─", fill) + Colors::RESET +
                       Colors::DIM + Colors::GRAY + meta_right + Colors::RESET +
                       MUTED + "─╮" + Colors::RESET;
            };
            auto render_top = [&]() -> std::string {

                if (first_prompt) {
                    std::vector<std::string> lines = welcome_lines(welcome_phrase.value_or(""));
                    lines.push_back("");
                    lines.push_back(render_bar());
                    return join_str(lines, "\n");
                }
                return render_bar();
            };
            auto render_prompt = []() -> std::string {
                return left_indent() + MUTED + "│" + Colors::RESET + " " +
                       ACCENT + Colors::BOLD + "❯" + Colors::RESET + " ";
            };

            std::string user_input;
            if (live_active()) {
                user_input = strip_py(live_read_line(&prompt_history));
            } else {
                std::cout << "\n";
                std::cout << render_top() << "\n";
                user_input = strip_py(styled_input(
                    render_prompt(), std::string(""), render_top, render_prompt, &prompt_history));
            }
            first_prompt = false;
            if (!user_input.empty()) {
                if (prompt_history.empty() || prompt_history.back() != user_input) {
                    prompt_history.push_back(user_input);
                    if (prompt_history.size() > 200)
                        prompt_history.erase(prompt_history.begin(),
                                             prompt_history.end() - 200);
                }
            }
            if (!live_active()) {
                std::cout << left_indent() << MUTED << "╰" << repeat_str("─", term_width() - 2)
                          << "╯" << Colors::RESET << "\n";
                std::cout << "\n";
                std::cout << soft_rule(Colors::VIOLET) << "\n";
            } else if (!user_input.empty()) {
                std::cout << left_indent() << ACCENT << Colors::BOLD << "\xE2\x9D\xAF "
                          << Colors::RESET << Colors::WHITE << user_input << Colors::RESET << "\n";
            }
            if (user_input.empty()) continue;

            if (starts_with(user_input, "/")) {
                std::vector<std::string> cmd_parts = split_ws(user_input);
                std::string cmd0 = cmd_parts.empty() ? std::string("") : cmd_parts[0];
                std::string cmd = lower_ascii(cmd0);
                std::string rest = strip_py(utf8_substr(user_input, utf8_len(cmd0)));

                if (cmd == "/exit" || cmd == "/quit") {
                    goodbye();
                    break;
                } else if (cmd == "/auto") {
                    auto_mode = !auto_mode;
                    log_info("Auto-mode is now " + mode_value(auto_mode));
                    continue;
                } else if (cmd == "/loop") {
                    loop_command(rest);
                    continue;
                } else if (cmd == "/plan") {
                    planning_enabled = !planning_enabled;
                    log_info("Planning mode is now " + mode_value(planning_enabled, "ENABLED", "DISABLED"));
                    const std::string DISABLED_PLAN =
                        "Planning is DISABLED. Do not use create_plan tool unless planning is explicitly enabled.";
                    std::string c = msg_content(messages[0]);
                    if (planning_enabled) {
                        if (c.find(DISABLED_PLAN) != std::string::npos) {
                            c = replace_all(c, DISABLED_PLAN, PLAN_PROMPT);
                        } else if (c.find(PLAN_PROMPT) == std::string::npos) {
                            c += " " + PLAN_PROMPT;
                        }
                    } else {
                        c = replace_all(c, PLAN_PROMPT, DISABLED_PLAN);
                    }
                    messages[0]["content"] = c;
                    continue;
                } else if (cmd == "/save") {
                    save_session(cmd_parts.size() > 1 ? join_str(tail_from(cmd_parts, 1), " ")
                                                      : std::string(""));
                    continue;
                } else if (cmd == "/load") {
                    load_session(cmd_parts.size() > 1 ? join_str(tail_from(cmd_parts, 1), " ")
                                                      : std::string(""));
                    continue;
                } else if (cmd == "/sessions") {
                    print_sessions();
                    continue;
                } else if (cmd == "/status") {
                    print_panel("status", {
                        kv_row("model",     std::string(Colors::WHITE) + model_name + Colors::RESET),
                        kv_row("backend",   std::string(Colors::WHITE) + backend + Colors::RESET),
                        kv_row("mpc",       (mpc_url && !mpc_url->empty())
                                                ? (std::string(Colors::WHITE) + *mpc_url + Colors::RESET)
                                                : (std::string(Colors::GRAY) + "not connected" + Colors::RESET)),
                        kv_row("auto",      mode_value(auto_mode)),
                        kv_row("planning",  mode_value(planning_enabled)),
                        kv_row("history",   std::string(Colors::WHITE) + std::to_string(messages.size()) + Colors::RESET +
                                            " " + Colors::DIM + Colors::GRAY + "messages" + Colors::RESET),
                        kv_row("compacted", std::string(Colors::WHITE) + std::to_string(compaction_count) + Colors::RESET +
                                            " " + Colors::DIM + Colors::GRAY + "times" + Colors::RESET),
                    }, MUTED);
                    continue;
                } else if (cmd == "/dashboard") {
                    int port = 8730;
                    if (cmd_parts.size() > 1) {
                        try {
                            int parsed = std::stoi(cmd_parts[1]);
                            if (parsed > 0 && parsed <= 65535) port = parsed;
                        } catch (...) {
                        }
                    }
                    start_dashboard(*this, port);
                    std::string url = std::string("http://") + dashboard_lan_ipv4() + ":" + std::to_string(port);
                    log_ok("Dashboard live at " + std::string(Colors::TEAL) + url + Colors::RESET);
                    log_info("Reachable on your LAN — open it from any device on this network.");
                    continue;
                } else if (cmd == "/terminal") {
                    if (live_active()) {
                        log_info("The shared terminal is already shown on the right. "
                                 "Press Ctrl-T to type into it.");
                    } else {
                        attach_shared_terminal();
                    }
                    continue;
                } else if (cmd == "/classic") {
                    if (live_active()) {
                        live_end();
                        log_info("Switched to the classic scrolling view.");
                    } else {
                        log_info("Already in the classic view.");
                    }
                    continue;
                } else if (cmd == "/live") {
                    if (!live_active()) {
                        live_begin(live_status);
                        if (!live_active())
                            log_warn("The split-pane view needs an interactive terminal.");
                    } else {
                        log_info("The split-pane view is already active.");
                    }
                    continue;
                } else if (cmd == "/backend") {
                    if (cmd_parts.size() > 1 && BACKEND_DEFAULT_URLS.count(cmd_parts[1])) {
                        std::optional<std::string> url = (cmd_parts.size() > 2)
                            ? std::optional<std::string>(cmd_parts[2]) : std::nullopt;
                        set_backend(cmd_parts[1], url, false);
                    } else {
                        backend_menu();
                    }
                    continue;
                } else if (cmd == "/vram") {
                    vram_command(rest);
                    continue;
                } else if (cmd == "/connect") {
                    connect_mpc_command(rest);
                    continue;
                } else if (cmd == "/model") {
                    if (cmd_parts.size() > 1) {
                        model_name = join_str(tail_from(cmd_parts, 1), " ");
                        if (backend == "mlx") {
                            if (server_process && server_model != model_name) {
                                cleanup();
                            } else if (server_model != model_name) {
                                server_model = std::nullopt;
                            }
                            log_info("MLX model will load on the next prompt.");
                            if (is_large_mlx_model(model_name)) {
                                log_info("Large MLX model selected; the first prompt can take several "
                                         "minutes while weights load into memory.");
                            }
                        }
                        log_info("Model switched to " + std::string(Colors::TEAL) + model_name + Colors::RESET);
                    } else {
                        model_menu();
                    }
                    continue;
                } else if (cmd == "/tasks") {
                    print_tasks();
                    continue;
                } else if (cmd == "/phrase") {
                    if (!LOGO_PHRASES.empty()) {
                        std::string phrase = random_choice_local(LOGO_PHRASES);
                        int cols = term_cols();
                        int pad = std::max(0, (cols - static_cast<int>(clean_len(phrase))) / 2);
                        std::cout << "\n" << std::string(static_cast<std::size_t>(pad), ' ')
                                  << Colors::DIM << Colors::ITALIC << ACCENT << phrase
                                  << Colors::RESET << "\n\n";
                    } else {
                        log_info("No phrases are configured.");
                    }
                    continue;
                } else if (cmd == "/effort") {
                    const std::map<std::string, std::string> EBARS = {
                        {"basic", "▰▱▱▱▱"}, {"tuned", "▰▰▱▱▱"}, {"elite", "▰▰▰▱▱"},
                        {"mythic", "▰▰▰▰▱"}, {"beyond", "▰▰▰▰▰"},
                    };
                    const std::map<std::string, std::string> ETAG = {
                        {"basic", "quick, focused answers"},
                        {"tuned", "digs deeper, cites the code"},
                        {"elite", "systematic — traces every path"},
                        {"mythic", "maximum — never gives up"},
                        {"beyond", "transcends the task — exceptional, anticipates your needs"},
                    };
                    auto apply_effort = [&](const std::string& k) {
                        effort_level = k;
                        const EffortLevel& lvl = effort_levels().at(k);
                        if (k == "beyond") {
                            glow_text(std::string("GO BEYOND  —  ") + ETAG.at(k));
                        } else {
                            std::cout << "\n  " << lvl.color << Colors::BOLD << "● " << lvl.label
                                      << Colors::RESET << " " << lvl.color << EBARS.at(k) << Colors::RESET
                                      << " " << Colors::GRAY << "│" << Colors::RESET << " "
                                      << Colors::DIM << Colors::GRAY << ETAG.at(k) << Colors::RESET << "\n";
                        }
                    };
                    std::string arg = (cmd_parts.size() > 1)
                        ? lower_ascii(join_str(tail_from(cmd_parts, 1), "")) : std::string("");
                    if (arg == "go" || arg == "max" || arg == "gobeyond") arg = "beyond";
                    if (effort_levels().count(arg)) {
                        apply_effort(arg);
                    } else {
                        std::string cur = effort_level;
                        std::vector<std::string> options;
                        for (const auto& k : EFFORT_ORDER) {
                            const EffortLevel& lvl = effort_levels().at(k);
                            std::string label = (k == "beyond")
                                ? gold_gradient(ljust(lvl.label, 9))
                                : (std::string(lvl.color) + Colors::BOLD + ljust(lvl.label, 9) + Colors::RESET);
                            std::string ctag = (k == cur)
                                ? (std::string("  ") + Colors::DIM + Colors::GRAY + "← current" + Colors::RESET)
                                : std::string("");
                            options.push_back(label + "  " + std::string(lvl.color) + EBARS.at(k) +
                                              Colors::RESET + "  " + Colors::DIM + Colors::GRAY +
                                              ETAG.at(k) + Colors::RESET + ctag);
                        }
                        std::optional<int> idx = interactive_menu("effort level", options, Colors::VIOLET);
                        if (idx.has_value()) {
                            apply_effort(EFFORT_ORDER[static_cast<std::size_t>(*idx)]);
                        }
                    }
                    continue;
                } else if (cmd == "/help") {
                    auto help_row = [](const std::string& cmd_text, const std::string& desc) -> std::string {
                        int pad = std::max(0, 34 - static_cast<int>(clean_len(cmd_text)));
                        return std::string("  ") + Colors::WHITE + cmd_text + Colors::RESET +
                               std::string(static_cast<std::size_t>(pad), ' ') +
                               Colors::DIM + Colors::GRAY + desc + Colors::RESET;
                    };
                    auto help_header = [](const std::string& text) -> std::string {
                        return std::string("  ") + Colors::DIM + Colors::GRAY + "── " + text + " ──" + Colors::RESET;
                    };
                    print_panel("commands", {
                        help_header("session"),
                        help_row("/status",                          "show model and backend status"),
                        help_row("/tasks",                           "show progress checkpoints"),
                        help_row("/save [name]",                     "save session (auto-named if omitted)"),
                        help_row("/load [name]",                     "load a session (menu if omitted)"),
                        help_row("/sessions",                        "list saved sessions"),
                        help_row("/usage",                           "show token, timing, and activity stats"),
                        help_row("/exit",                            "quit LOREA"),
                        std::string(""),
                        help_header("workspace"),
                        help_row("/cmd <shell>",                     "run a shell command without using a turn"),
                        help_row("/live",                            "split-pane view: conversation + the AI's live terminal"),
                        help_row("/classic",                         "switch back to the classic scrolling view"),
                        help_row("/terminal",                        "attach full-screen to the shared shell (Ctrl-] to detach)"),
                        help_row("/diff [path]",                     "show the git diff of your working tree"),
                        help_row("/copy",                            "copy the last answer to the clipboard"),
                        help_row("/retry",                           "re-run your last prompt for a fresh attempt"),
                        help_row("/undo",                            "revert the most recent file write"),
                        help_row("/clear",                           "clear the screen, keep the session"),
                        std::string(""),
                        help_header("agents"),
                        help_row("/agent [n] [goal]",                "spawn parallel worker agents and coordinate results"),
                        help_row("/agent --full [n] [goal]",         "allow spawned agents to run commands or edit files"),
                        std::string(""),
                        help_header("models & backends"),
                        help_row("/backend",                         "open the backend switcher"),
                        help_row("/model",                           "open the model switcher"),
                        help_row("/connect [url] [--token t]",       "route chat and model downloads through MPC"),
                        help_row("/download_model [m] [--path dir]", "download a model for the active backend"),
                        help_row("/download <repo> [--path dir]",    "download an MLX model from Hugging Face"),
                        help_row("/setup_mlx",                       "install and download MLX models"),
                        std::string(""),
                        help_header("behavior"),
                        help_row("/auto",                            "toggle auto-execution mode"),
                        help_row("/effort",                          "pick how hard the model works (animated selector)"),
                        help_row("/loop <goal>",                     "work autonomously on a goal until done (Esc stops)"),
                        help_row("/plan",                            "toggle autonomous planning mode"),
                        help_row("/vram [--auto]",                   "tune Mac GPU memory limit (clickable slider)"),
                        help_row("/theme [name]",                    "change the accent color"),
                        help_row("/phrase",                          "show a random phrase"),
                        help_row("/help",                            "show this command list"),
                    }, MUTED);
                    continue;
                } else if (cmd == "/setup") {
                    setup_llama_cpp();
                    continue;
                } else if (cmd == "/setup_mlx") {
                    setup_mlx();
                    continue;
                } else if (cmd == "/download") {
                    auto pr = parse_download_args(rest);
                    std::optional<std::string> repo_id = pr.first;
                    std::optional<std::string> download_dir = pr.second;
                    if (!repo_id || repo_id->empty()) {

                        repo_id = LOREA_CYBER_HF_REPO;
                        log_info("No repo given — downloading the LOREA-cyber model: " + *repo_id);
                        log_info("~16 GB MLX 4-bit model (Apple Silicon). It will be set as your model when done.");
                    }
                    if (!download_dir.has_value()) {
                        download_dir = prompt_download_dir(*repo_id);
                    }
                    download_mlx_model(*repo_id, download_dir.value_or(""));

                    std::vector<std::string> files;
                    if (download_dir && !download_dir->empty()) {
                        std::error_code ec;
                        if (std::filesystem::is_directory(*download_dir, ec)) {
                            std::filesystem::directory_iterator it(*download_dir, ec), end;
                            for (; !ec && it != end; it.increment(ec)) {
                                files.push_back(it->path().filename().string());
                            }
                        }
                    }
                    bool has_model = false;
                    for (const auto& f : files) {
                        if (ends_with(f, ".safetensors")) { has_model = true; break; }
                    }
                    if (!has_model) {
                        for (const auto& f : files) {
                            if (f == "config.json") { has_model = true; break; }
                        }
                    }
                    if (has_model) {
                        backend = "mlx";
                        model_name = download_dir.value_or("");
                        if (server_process && server_model != model_name) {
                            cleanup();
                        } else {
                            server_model = std::nullopt;
                        }
                        log_ok("Model set to " + model_name +
                               " — it loads on your next message. Type a prompt to use it.");
                    } else {
                        log_info("Download finished. Point OCLI at it with: /model <path-to-folder>");
                    }
                    continue;
                } else if (cmd == "/download_model") {
                    auto pr = parse_download_args(rest);
                    std::optional<std::string> model_arg = pr.first;
                    std::optional<std::string> download_dir = pr.second;
                    if (mpc_url && !mpc_url->empty()) {
                        download_mpc_model_menu(model_arg, download_dir);
                    } else {
                        download_model_menu(model_arg, download_dir);
                    }
                    continue;
                } else if (cmd == "/agent") {
                    agent_command(rest);
                    continue;
                } else if (cmd == "/copy") {
                    copy_last_answer();
                    continue;
                } else if (cmd == "/retry") {
                    if (retry_last_turn()) {
                        user_input = last_user_goal;

                    } else {
                        continue;
                    }
                } else if (cmd == "/undo") {
                    undo_last_write();
                    continue;
                } else if (cmd == "/cmd") {
                    run_oneoff_shell(rest);
                    continue;
                } else if (cmd == "/clear") {
                    std::cout << "\033[3J\033[H\033[2J";
                    std::cout.flush();
                    print_logo(pick_logo_phrase());
                    continue;
                } else if (cmd == "/usage") {
                    print_usage();
                    continue;
                } else if (cmd == "/theme") {
                    theme_command(cmd_parts.size() > 1 ? cmd_parts[1] : std::string(""));
                    continue;
                } else if (cmd == "/diff") {
                    show_working_diff(cmd_parts.size() > 1 ? join_str(tail_from(cmd_parts, 1), " ")
                                                           : std::string(""));
                    continue;
                } else {
                    log_info("Unknown command: " + cmd);
                    continue;
                }
            }

            if (lower_ascii(user_input) == "exit" || lower_ascii(user_input) == "quit") break;
            bool continuing_previous_goal = is_continue_request(user_input);
            if (continuing_previous_goal) {
                if (messages.size() > 1 && !last_user_goal.empty()) {
                    user_input = "Continue the previous task and provide the next concrete tool call "
                                 "or final answer.\nOriginal request:\n" + last_user_goal;
                } else if (messages.size() > 1) {
                    user_input = "Please continue.";
                } else {
                    continue;
                }
            } else {
                last_user_goal = user_input;
                session_turns += 1;
            }
            last_tool_signature = std::nullopt;
            repeated_tool_count = 0;
            last_failure_signature = std::nullopt;
            repeated_failure_count = 0;
            tool_steps_this_turn = 0;
            std::string ai_input = user_input;
            if (live_active()) {
                std::string term_ctx = live_take_terminal_context();
                if (!term_ctx.empty()) {
                    ai_input = "I ran some commands in the shared terminal. Here is the terminal "
                               "output for context:\n```\n" + term_ctx + "\n```\n\n" + user_input;
                    log_info("Including your terminal output with this message.");
                }
            }
            messages.push_back(make_message("user", ai_input));
            int auto_count = 0;
            while (auto_count < 10) {
                compact_history();
                bool had_tools = process_chat();
                std::string last_msg = messages.empty() ? std::string("") : msg_content(messages.back());
                bool should_continue =
                    (had_tools && auto_mode)
                    || (contains_sub(upper_ascii(last_msg), "CONTINUE") && auto_count < 3)
                    || (had_tools && auto_mode && msg_role(messages.back()) == "tool" && auto_count < 3);
                if (should_continue) {
                    auto_count += 1;
                    std::cout << "\n" << left_indent() << ACCENT << "↻" << Colors::RESET << " "
                              << Colors::DIM << Colors::GRAY << "auto" << Colors::RESET
                              << "  continuing step " << Colors::WHITE << auto_count
                              << Colors::DIM << "/10" << Colors::RESET << "\n";
                    messages.push_back(make_message("user",
                        "Continue the original task and make concrete progress toward completion. "
                        "Original request:\n" + last_user_goal +
                        "\nDo not repeat the same command/read cycle. If implementation is incomplete, "
                        "write the full files now. If files are written, run pytest. If tests pass, "
                        "summarize final files and usage."));
                } else {
                    break;
                }
            }
        } catch (const std::runtime_error& e) {

            if (std::string(e.what()) == "KeyboardInterrupt") break;
            throw;
        }
    }
    if (live_active()) live_end();
}

}
