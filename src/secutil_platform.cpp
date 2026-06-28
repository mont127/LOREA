#include "secutil.hpp"

#include <string>
#include <cctype>
#include <cerrno>
#include <cstdlib>

#if defined(__APPLE__)
#include <sys/utsname.h>
#endif

namespace ocli {

namespace {

std::string strip_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool str_isdigit(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}

bool is_apple_silicon() {

    try {
#if !defined(__APPLE__)

        return false;
#else

        {
            struct utsname u;
            if (uname(&u) == 0) {
                std::string machine = u.machine;
                if (machine == "arm64") {
                    return true;
                }
            }
        }

        ProcResult out = run_subprocess(
            {"sysctl", "-n", "machdep.cpu.brand_string"}, "", 4.0, false);

        const std::string& s = out.out;
        return s.find("Apple") != std::string::npos;
#endif
    } catch (...) {

        return false;
    }
}

long mac_total_ram_mb() {
    try {

        ProcResult out = run_subprocess({"sysctl", "-n", "hw.memsize"}, "", 4.0, false);

        std::string text = strip_ws(out.out);

        errno = 0;
        char* end = nullptr;
        long long val = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || end != text.c_str() + text.size() || errno != 0) {
            return 0;
        }
        return static_cast<long>(val / (1024LL * 1024LL));
    } catch (...) {
        return 0;
    }
}

long mac_current_wired_limit_mb() {

    try {
        ProcResult out = run_subprocess(
            {"sysctl", "-n", "iogpu.wired_limit_mb"}, "", 4.0, false);

        std::string text = strip_ws(out.out);

        if (str_isdigit(text)) {
            return std::strtol(text.c_str(), nullptr, 10);
        }
        return 0;
    } catch (...) {
        return 0;
    }
}

VramBounds mac_vram_bounds(long total_mb) {

    if (total_mb <= 0) {
        return VramBounds{4096, 16384, 4096};
    }

    long reserve = (total_mb >= 32768) ? 6144 : (total_mb >= 16384) ? 4096 : 3072;

    long quarter = total_mb / 4;
    long lo = (4096 < quarter) ? 4096 : quarter;

    long hi_cand = total_mb - reserve;
    long hi = ((lo + 1024) > hi_cand) ? (lo + 1024) : hi_cand;
    return VramBounds{lo, hi, reserve};
}

long mac_recommended_wired_limit_mb(long total_mb) {

    VramBounds b = mac_vram_bounds(total_mb);
    long lo = b.lo, hi = b.hi;

    double frac = (total_mb >= 32768) ? 0.80 : (total_mb >= 16384) ? 0.75 : 0.66;

    long rec = static_cast<long>(static_cast<double>(total_mb) * frac);

    long inner = (hi < rec) ? hi : rec;
    return (lo > inner) ? lo : inner;
}

}
