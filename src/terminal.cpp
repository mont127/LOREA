#include "terminal.hpp"
#include "ansi.hpp"

#include <unistd.h>
#include <sys/select.h>

#include <cstring>
#include <cstdint>
#include <thread>
#include <chrono>
#include <regex>

namespace ocli {

volatile std::sig_atomic_t       RESIZE_FLAG = 0;
std::deque<unsigned char>        INPUT_PUSHBACK;
std::mutex                       INPUT_PUSHBACK_MUTEX;

static const std::regex MOUSE_SGR_RE(
    "\x1b\\[<(\\d+);(\\d+);(\\d+)([Mm])");

const std::vector<std::pair<std::string, std::string>> SLASH_COMMANDS = {
    {"/help", "show the full command list"},
    {"/status", "model, backend, and session status"},
    {"/usage", "token, timing, and activity stats"},
    {"/tasks", "show plan progress checkpoints"},
    {"/auto", "toggle auto-execution mode"},
    {"/effort", "how hard the model works (pick a level)"},
    {"/loop", "work autonomously on a goal until done"},
    {"/plan", "toggle autonomous planning mode"},
    {"/vram", "tune Mac GPU memory limit (slider)"},
    {"/model", "switch the active model"},
    {"/backend", "switch the inference backend"},
    {"/connect", "route chat through an MPC server"},
    {"/download_model", "download a model for the backend"},
    {"/download", "download the LOREA-cyber model (bare), or any HF repo, and use it"},
    {"/setup_mlx", "install and download MLX models"},
    {"/theme", "change the accent color"},
    {"/cmd", "run a shell command (no turn used)"},
    {"/diff", "show the git diff of your tree"},
    {"/copy", "copy the last answer to clipboard"},
    {"/retry", "re-run your last prompt"},
    {"/undo", "revert the most recent file write"},
    {"/clear", "clear the screen, keep session"},
    {"/save", "save the session"},
    {"/load", "load a saved session"},
    {"/sessions", "list saved sessions"},
    {"/phrase", "show a random phrase"},
    {"/exit", "quit LOREA"},
};

static inline bool py_in(const std::string& needle, const std::string& haystack) {
    return haystack.find(needle) != std::string::npos;
}

static inline bool py_isdigit(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

static std::string utf8_decode_ignore(const unsigned char* p, std::size_t n) {
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        unsigned char b = p[i];
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        int len;
        unsigned int code;
        if ((b & 0xE0) == 0xC0) { len = 2; code = b & 0x1Fu; }
        else if ((b & 0xF0) == 0xE0) { len = 3; code = b & 0x0Fu; }
        else if ((b & 0xF8) == 0xF0) { len = 4; code = b & 0x07u; }
        else { ++i; continue; }
        if (i + static_cast<std::size_t>(len) > n) { ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char c = p[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            code = (code << 6) | (c & 0x3Fu);
        }
        if (!ok) { ++i; continue; }

        if (len == 2 && code < 0x80u) { ++i; continue; }
        if (len == 3 && code < 0x800u) { ++i; continue; }
        if (len == 4 && (code < 0x10000u || code > 0x10FFFFu)) { ++i; continue; }
        if (code >= 0xD800u && code <= 0xDFFFu) { ++i; continue; }
        for (int k = 0; k < len; ++k) out.push_back(static_cast<char>(p[i + k]));
        i += static_cast<std::size_t>(len);
    }
    return out;
}

static bool fd_select_ready(int fd, double timeout) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    if (timeout < 0.0) timeout = 0.0;
    tv.tv_sec = static_cast<long>(timeout);
    tv.tv_usec = static_cast<long>((timeout - static_cast<double>(tv.tv_sec)) * 1e6);
    int r = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    return r > 0 && FD_ISSET(fd, &rfds);
}

void _on_resize(int ) {
    RESIZE_FLAG = 1;
}

void install_resize_handler() {

    static bool installed = false;
    if (installed) return;
    installed = true;
#ifdef SIGWINCH
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = _on_resize;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGWINCH, &sa, nullptr);
#endif
}

bool input_ready(int fd, double timeout) {
    {
        std::lock_guard<std::mutex> lock(INPUT_PUSHBACK_MUTEX);
        if (!INPUT_PUSHBACK.empty()) return true;
    }
    return fd_select_ready(fd, timeout);
}

void wait_for_key_or_resize(int fd, const std::function<void()>& on_resize,
                            double debounce) {
    while (true) {
        if (RESIZE_FLAG) {
            RESIZE_FLAG = 0;
            double settled = 0.0;
            while (settled < debounce) {
                std::this_thread::sleep_for(std::chrono::duration<double>(0.04));
                settled += 0.04;
                if (RESIZE_FLAG) {
                    RESIZE_FLAG = 0;
                    settled = 0.0;
                }
            }
            on_resize();
        }
        if (input_ready(fd, 0.08)) {
            return;
        }
    }
}

std::string read_input_byte(int fd) {
    {
        std::lock_guard<std::mutex> lock(INPUT_PUSHBACK_MUTEX);
        if (!INPUT_PUSHBACK.empty()) {
            unsigned char b = INPUT_PUSHBACK.front();
            INPUT_PUSHBACK.pop_front();
            return utf8_decode_ignore(&b, 1);
        }
    }
    unsigned char buf;
    ssize_t r = ::read(fd, &buf, 1);
    if (r <= 0) {
        return std::string();
    }
    return utf8_decode_ignore(&buf, 1);
}

std::string read_key() {
    int fd = STDIN_FILENO;
    std::string key = read_input_byte(fd);
    if (key != "\x1b") {
        return key;
    }
    if (!input_ready(fd, 0.05)) {
        return key;
    }
    std::string second = read_input_byte(fd);
    std::string sequence = key + second;
    if (second != "[") {
        return sequence;
    }
    if (!input_ready(fd, 0.05)) {
        return sequence;
    }
    std::string third = read_input_byte(fd);
    sequence += third;
    if (py_in(third, "ABCDHF")) {
        return sequence;
    }

    if (third == "<") {
        while (input_ready(fd, 0.1) && sequence.size() < 24) {
            std::string ch = read_input_byte(fd);
            sequence += ch;
            if (py_in(ch, "Mm")) {
                break;
            }
        }
        return sequence;
    }
    if (py_isdigit(third)) {
        while (input_ready(fd, 0.05) && sequence.size() < 6) {
            std::string ch = read_input_byte(fd);
            sequence += ch;
            if (ch == "~") {
                break;
            }
        }
    }
    return sequence;
}

std::optional<MouseEvent> parse_mouse(const std::string& sequence) {

    std::smatch match;
    if (!std::regex_search(sequence, match, MOUSE_SGR_RE,
                           std::regex_constants::match_continuous)) {
        return std::nullopt;
    }
    MouseEvent ev;
    ev.button = std::stoi(match.str(1));
    ev.col = std::stoi(match.str(2));
    ev.row = std::stoi(match.str(3));
    ev.pressed = (match.str(4) == "M");
    return ev;
}

std::string read_bracketed_paste(int fd) {
    static const unsigned char marker[] = {0x1b, '[', '2', '0', '1', '~'};
    const std::size_t marker_len = sizeof(marker);
    std::vector<unsigned char> data;
    while (true) {
        if (!fd_select_ready(fd, 0.5)) {
            break;
        }
        unsigned char chunk[4096];
        ssize_t r = ::read(fd, chunk, sizeof(chunk));
        if (r <= 0) {
            break;
        }
        data.insert(data.end(), chunk, chunk + r);

        std::size_t index = std::string::npos;
        if (data.size() >= marker_len) {
            for (std::size_t i = 0; i + marker_len <= data.size(); ++i) {
                if (std::memcmp(data.data() + i, marker, marker_len) == 0) {
                    index = i;
                    break;
                }
            }
        }
        if (index != std::string::npos) {
            std::size_t tail_start = index + marker_len;
            if (tail_start < data.size()) {

                std::lock_guard<std::mutex> lock(INPUT_PUSHBACK_MUTEX);
                INPUT_PUSHBACK.insert(INPUT_PUSHBACK.begin(),
                                      data.begin() + static_cast<std::ptrdiff_t>(tail_start),
                                      data.end());
            }
            return utf8_decode_ignore(data.data(), index);
        }
    }
    return utf8_decode_ignore(data.data(), data.size());
}

}
