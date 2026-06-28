#include "secutil.hpp"
#include "ansi.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <sys/stat.h>

namespace ocli {

const std::string SESSIONS_DIR = expanduser("~/.ocli/sessions");

namespace {

std::string py_strip(const std::string& s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\v' || c == '\f';
    };
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string path_join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}

const std::string& ensure_sessions_dir() {
    std::error_code ec;
    std::filesystem::create_directories(SESSIONS_DIR, ec);
    return SESSIONS_DIR;
}

std::string resolve_session_path(const std::string& name_in) {
    std::string name = py_strip(name_in);
    if (name.find('/') != std::string::npos ||
        (!name.empty() && name[0] == '~')) {
        return expanduser(name);
    }
    if (!ends_with(name, ".json")) {
        name += ".json";
    }
    return path_join(ensure_sessions_dir(), name);
}

std::vector<SessionInfo> list_saved_sessions() {
    ensure_sessions_dir();
    std::vector<SessionInfo> items;

    std::error_code ec;
    std::filesystem::directory_iterator it(SESSIONS_DIR, ec);
    if (ec) {
        return items;
    }

    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string filename = it->path().filename().string();
        if (!ends_with(filename, ".json")) continue;
        const std::string full = path_join(SESSIONS_DIR, filename);

        struct stat st {};
        if (::stat(full.c_str(), &st) != 0) {
            continue;
        }

        SessionInfo info;
        info.name = filename;
        info.path = full;

        info.mtime = static_cast<double>(st.st_mtimespec.tv_sec) +
                     static_cast<double>(st.st_mtimespec.tv_nsec) / 1e9;
        info.size  = static_cast<long>(st.st_size);
        items.push_back(std::move(info));
    }

    std::stable_sort(items.begin(), items.end(),
                     [](const SessionInfo& a, const SessionInfo& b) {
                         return a.mtime > b.mtime;
                     });
    return items;
}

int estimate_tokens(const std::string& text) {
    const std::size_t n = utf8_len(text);
    return std::max<int>(1, static_cast<int>(n / 4));
}

std::string head_tail_trim(const std::string& text, size_t limit,
                            const std::string& marker) {
    const std::size_t n = utf8_len(text);
    if (n <= limit) return text;
    const std::size_t head = std::max<std::size_t>(1, (limit * 2) / 3);

    const std::size_t tail = std::max<std::size_t>(1, limit - head);

    return utf8_substr(text, 0, head) + marker +
           utf8_substr(text, n - tail);
}

std::string truncate_output(const std::string& output, size_t limit) {
    if (utf8_len(output) > limit) {
        return utf8_substr(output, 0, limit) + "\n\n[Output truncated due to size.]";
    }
    return output;
}

}
