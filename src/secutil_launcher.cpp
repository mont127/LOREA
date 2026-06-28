#include "secutil.hpp"
#include "ansi.hpp"
#include "render.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <string>

#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>

namespace ocli {

const std::vector<std::string> ALLOWED_SEARCH_DOMAINS = {
    "ollama.com",
    "googleblog.com",
    "ai.google.dev",
    "huggingface.co",
};

const std::vector<std::string> CLI_INSTALL_PATHS = {
    std::string("/usr/local/bin/") + CLI_COMMAND_NAME,
    expanduser(std::string("~/.local/bin/") + CLI_COMMAND_NAME),
    std::string("/bin/") + CLI_COMMAND_NAME,
};

namespace {

std::string to_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool starts_with_any(const std::string& s, const std::vector<std::string>& prefixes) {
    for (const auto& p : prefixes)
        if (starts_with(s, p)) return true;
    return false;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}

std::string current_executable_path() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::string();
    }

    if (auto pos = buf.find('\0'); pos != std::string::npos) buf.resize(pos);
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec);
    if (!ec) return canon.string();
    return buf;
}

namespace {

std::vector<std::string> split_pathsep(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, ':')) out.push_back(cur);

    if (!s.empty() && s.back() == ':') out.push_back("");
    if (s.empty()) out.push_back("");
    return out;
}

std::string env_get(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

}

bool launcher_matches_source(const std::string& path, const std::string& source) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();
        return content.find(source) != std::string::npos;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> command_on_path(const std::string& name) {
    const std::string path_env = env_get("PATH");
    for (const std::string& directory : split_pathsep(path_env)) {

        std::string candidate;
        if (directory.empty()) {
            candidate = name;
        } else if (!directory.empty() && directory.back() == '/') {
            candidate = directory + name;
        } else {
            candidate = directory + "/" + name;
        }
        struct stat st {};
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            ::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool installed_via_package() {
    if (!env_get("LOREA_INSTALLED_VIA_PACKAGE").empty()) {
        return true;
    }
    const std::string here = current_executable_path();
    static const std::vector<std::string> markers = {
        "site-packages",
        "dist-packages",
        "pipx/venvs",
    };
    for (const auto& m : markers)
        if (contains(here, m)) return true;
    return false;
}

void install_cli_launcher() {

    if (env_get("LOREA_SKIP_INSTALL") == "1" || installed_via_package()) {
        return;
    }
    const std::string source = current_executable_path();

    if (!source.empty()) {
        const std::string exe_dir =
            std::filesystem::path(source).parent_path().string();
        for (const std::string& d : split_pathsep(env_get("PATH")))
            if (!d.empty() && d == exe_dir) return;
    }

    std::optional<std::string> existing = command_on_path(CLI_COMMAND_NAME);
    if (existing && launcher_matches_source(*existing, source)) {
        return;
    }

    const std::string wrapper =
        "#!/bin/sh\nexec " + shlex_quote(source) + " \"$@\"\n";

    std::vector<std::string> errors;

    const std::string path_env = env_get("PATH");
    const std::vector<std::string> path_dirs = split_pathsep(path_env);

    for (const std::string& path : CLI_INSTALL_PATHS) {
        std::error_code ec_exists;
        if (std::filesystem::exists(std::filesystem::path(path), ec_exists)) {
            if (launcher_matches_source(path, source)) {
                return;
            }
            errors.push_back(path + " already exists");
            continue;
        }
        try {
            const std::string dir =
                std::filesystem::path(path).parent_path().string();

            if (!dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(dir), ec);
                if (ec) {
                    throw std::runtime_error(ec.message());
                }
            }

            {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                if (!f) {
                    throw std::runtime_error("could not open for writing");
                }
                f << wrapper;
                if (!f) {
                    throw std::runtime_error("write failed");
                }
            }

            if (::chmod(path.c_str(), 0755) != 0) {
                throw std::runtime_error("chmod failed");
            }
            log_info(std::string("Installed launcher: ") + Colors::TEAL + path +
                     Colors::RESET);

            bool dir_on_path = false;
            for (const auto& d : path_dirs) {
                if (d == dir) { dir_on_path = true; break; }
            }
            if (!dir_on_path) {
                log_info(std::string("Add ") + Colors::TEAL + dir + Colors::RESET +
                         " to PATH to run " + Colors::TEAL + "LOREA" +
                         Colors::RESET + " directly.");
            }
            return;
        } catch (const std::exception& e) {
            errors.push_back(path + ": " + e.what());
        } catch (...) {
            errors.push_back(path + ": error");
        }
    }

    if (can_use_terminal_keys()) {
        log_info("Could not install LOREA launcher automatically. Try running "
                 "with permissions for /usr/local/bin or create a launcher "
                 "manually.");
    }
}

bool model_matches_backend(const std::string& model, const std::string& backend) {
    if (model.empty()) {
        return false;
    }
    if (backend == "nvidia") {
        return true;
    }
    const std::string lowered = to_lower(model);
    if (backend == "anthropic") {
        return starts_with(lowered, "claude");
    }
    if (backend == "openai") {
        return starts_with_any(lowered, {"gpt-", "o1", "o3", "o4", "chatgpt"}) ||
               starts_with(lowered, "text-");
    }
    if (backend == "airllm") {
        return contains(model, "/") &&
               !starts_with(model, "mlx-community/") &&
               !contains(lowered, "gguf");
    }
    if (backend == "mlx") {
        return starts_with(model, "mlx-community/") ||
               starts_with_any(model, {"/", ".", "~"});
    }
    if (backend == "llama-cpp") {
        return ends_with(lowered, ".gguf") ||
               contains(lowered, "gguf") ||
               starts_with_any(model, {"/", ".", "~"});
    }

    return !starts_with_any(lowered, {"claude", "gpt-", "o1", "o3", "o4"}) &&
           !starts_with(model, "mlx-community/") &&
           !contains(lowered, "gguf") &&
           !ends_with(lowered, ".gguf");
}

bool is_large_mlx_model(const std::string& model) {
    const std::string lowered = to_lower(model);
    static const std::vector<std::string> sizes = {
        "24b", "26b", "27b", "31b", "32b", "35b", "40b", "70b",
    };
    for (const auto& size : sizes)
        if (contains(lowered, size)) return true;
    return false;
}

}
