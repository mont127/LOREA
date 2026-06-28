#include "lorea.hpp"

#include <string>
#include <vector>
#include <set>
#include <optional>
#include <algorithm>
#include <chrono>
#include <thread>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <typeinfo>
#include <exception>
#include <filesystem>

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <cxxabi.h>

namespace ocli {

namespace {

namespace fs = std::filesystem;

const std::string LOREA_DIR      = expanduser("~/lorea-ft/lorea-coder-30b-a3b-v3-re");
const std::string LOREA_BASE     = "mlx-community/Qwen3-Coder-30B-A3B-Instruct-4bit";
const std::string LOREA_ADAPTERS = expanduser("~/lorea-ft/adapters_v3_30b");
const std::string LOREA_FALLBACK = expanduser("~/lorea-ft/lorea-coder-14b-v3-re");

std::string sys_executable() {
    const char* p = std::getenv("PYTHON");
    if (p && *p) return p;
    return "python3";
}

std::string after_slashslash(const std::string& url) {
    auto pos = url.rfind("//");
    return pos == std::string::npos ? url : url.substr(pos + 2);
}

std::string before_first_colon(const std::string& s) {
    auto pos = s.find(':');
    return pos == std::string::npos ? s : s.substr(0, pos);
}

std::string after_last_colon(const std::string& s) {
    auto pos = s.rfind(':');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

int parse_int_or(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    errno = 0;
    char* end = nullptr;
    long val = std::strtol(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || errno != 0) return fallback;
    return static_cast<int>(val);
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string exc_type_name(const std::exception& exc) {
    const char* mangled = typeid(exc).name();
    int status = 0;
    char* dem = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    std::string out = (status == 0 && dem) ? std::string(dem) : std::string(mangled);
    std::free(dem);
    return out;
}

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}

bool LOREA::is_port_open(const std::string& host, int port) {
    struct addrinfo hints {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string portstr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0)
        return false;
    bool ok = false;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        int s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        int rc = ::connect(s, p->ai_addr, p->ai_addrlen);
        ::close(s);
        if (rc == 0) { ok = true; break; }
    }
    ::freeaddrinfo(res);
    return ok;
}

bool LOREA::ensure_lorea() {
    std::error_code ec;
    if (fs::is_directory(LOREA_DIR, ec) &&
        fs::exists(fs::path(LOREA_DIR) / "config.json", ec)) {
        return true;
    }
    const std::string adapter_file = (fs::path(LOREA_ADAPTERS) / "adapters.safetensors").string();
    if (!fs::exists(adapter_file, ec)) {
        log_info("LOREA adapter not ready yet (fine-tune still running). Using fallback for now.");
        return false;
    }
    log_info("Building LOREA (downloading base model + fusing adapter)... one-time, a few minutes.");
    try {

        ProcResult r = run_subprocess(
            {sys_executable(), "-m", "mlx_lm", "fuse",
             "--model", LOREA_BASE, "--adapter-path", LOREA_ADAPTERS,
             "--save-path", LOREA_DIR},
            "", 0.0, false);
        if (!r.started || r.exit_code != 0) {

            throw std::runtime_error(
                "Command '[" + sys_executable() + ", -m, mlx_lm, fuse]' returned non-zero exit status " +
                std::to_string(r.exit_code) + ".");
        }
        return fs::is_directory(LOREA_DIR, ec);
    } catch (const std::exception& e) {
        log_info(std::string("LOREA build failed: ") + e.what());
        return false;
    }
}

void LOREA::ensure_mlx_server() {

    if (model_name == LOREA_DIR && !ensure_lorea()) {
        model_name = LOREA_FALLBACK;
    }
    const std::string host = before_first_colon(after_slashslash(url));
    int port = parse_int_or(after_last_colon(url), 8080);

    if (server_process && server_process->poll().has_value()) {
        server_process = nullptr;
        server_model = std::nullopt;
    }
    const bool model_matches = server_model.has_value() && *server_model == model_name;
    if (server_process && !model_matches) {
        cleanup();
    }
    if (server_process && model_matches && is_port_open(host, port)) {
        return;
    }
    if (is_port_open(host, port)) {
        log_info("MLX Server already running on " + host + ":" + std::to_string(port));
        server_model = model_name;
        return;
    }

    log_info("Auto-starting MLX Server with model: " + model_name);
    std::vector<std::string> cmd = {sys_executable(), "-m", "mlx_lm.server", "--model", model_name};
    try {
        server_process = spawn_process(cmd, true);
        log_info("Waiting for MLX server to initialize...");
        for (int i = 0; i < 30; ++i) {
            if (is_port_open(host, port)) {
                server_model = model_name;
                log_info("MLX Server is ready!");
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        log_info("Warning: MLX Server is taking a long time to start. It might still be loading.");
    } catch (const std::exception& e) {
        log_info(std::string("Failed to start MLX server: ") + e.what());
    }
}

std::optional<std::string> LOREA::find_llama_server_bin() {
    const char* envbin = std::getenv("LLAMA_SERVER_BIN");
    std::vector<std::string> cands;
    cands.push_back(envbin ? std::string(envbin) : std::string());
    cands.push_back(expanduser("~/llama.cpp/build/bin/llama-server"));
    cands.push_back("/opt/homebrew/bin/llama-server");
    cands.push_back("/usr/local/bin/llama-server");
    std::error_code ec;
    for (const std::string& c : cands) {
        if (!c.empty() && fs::is_regular_file(c, ec) && ::access(c.c_str(), X_OK) == 0) {
            return c;
        }
    }
    try {
        ProcResult r = run_subprocess({"which", "llama-server"}, "", 5.0, false);
        std::string out = strip(r.out);
        if (!out.empty() && fs::is_regular_file(out, ec)) {
            return out;
        }
    } catch (...) {

    }
    return std::nullopt;
}

void LOREA::ensure_llamacpp_server() {
    auto [host, port] = inference_hostport();
    if (server_process && server_process->poll().has_value()) {
        server_process = nullptr;
        server_model = std::nullopt;
    }
    const bool model_matches = server_model.has_value() && *server_model == model_name;
    if (server_process && !model_matches) {
        cleanup();
    }
    if (server_process && model_matches && is_port_open(host, port)) {
        return;
    }
    if (is_port_open(host, port)) {
        log_info("llama.cpp server already running on " + host + ":" + std::to_string(port));
        server_model = model_name;
        return;
    }

    const std::string model = expanduser(model_name);
    std::error_code ec;
    if (!(ends_with(model, ".gguf") && fs::is_regular_file(model, ec))) {
        log_info("llama-cpp model is not a local .gguf file (" + model_name + "); "
                 "expecting an external server at " + url + ".");
        return;
    }
    std::optional<std::string> binpath = find_llama_server_bin();
    if (!binpath) {
        log_info("Could not find the llama-server binary (set LLAMA_SERVER_BIN or "
                 "install llama.cpp); expecting an external server at " + url + ".");
        return;
    }

    log_info("Auto-starting llama.cpp server: " + fs::path(model).filename().string());
    std::vector<std::string> cmd = {*binpath, "-m", model, "-ngl", "99", "-c", "8192",
                                    "--host", host, "--port", std::to_string(port)};
    try {
        server_process = spawn_process(cmd, true);
        log_info("Waiting for llama.cpp server to load the model...");

        for (int i = 0; i < 120; ++i) {
            if (server_process->poll().has_value()) {
                log_info("llama.cpp server exited during startup.");
                server_process = nullptr;
                return;
            }
            if (llamacpp_health_ok(host, port)) {
                server_model = model_name;
                log_info("llama.cpp server is ready!");
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        log_info("Warning: llama.cpp server is taking a long time to start. It might still be loading.");
    } catch (const std::exception& e) {
        log_info(std::string("Failed to start llama.cpp server: ") + e.what());
    }
}

bool LOREA::llamacpp_health_ok(const std::string& host, int port) {
    try {
        HttpRequest req;
        req.method     = "GET";
        req.url        = "http://" + host + ":" + std::to_string(port) + "/health";
        req.timeout_ms = 2000;
        HttpResponse resp = http_perform(req);
        if (!resp.error.empty()) return false;
        return resp.status == 200;
    } catch (...) {
        return false;
    }
}

void LOREA::ensure_local_server() {
    if (backend == "mlx") {
        ensure_mlx_server();
    } else if (backend == "llama-cpp") {
        ensure_llamacpp_server();
    }
}

std::pair<std::string, int> LOREA::inference_hostport() {
    std::string host;
    try {
        host = before_first_colon(after_slashslash(url));
        if (host.empty()) host = "127.0.0.1";
    } catch (...) {
        host = "127.0.0.1";
    }
    int port;
    try {
        port = parse_int_or(after_last_colon(url), 8080);
    } catch (...) {
        port = 8080;
    }
    return {host, port};
}

void LOREA::kill_inference_procs_on_port(int port) {
    std::string out;
    try {
        ProcResult r = run_subprocess(
            {"lsof", "-nP", "-iTCP:" + std::to_string(port), "-sTCP:LISTEN", "-t"},
            "", 5.0, false);
        out = r.out;
    } catch (...) {
        return;
    }

    std::set<std::string> pids;
    {
        std::string tok;
        auto flush = [&]() {
            if (!tok.empty()) {
                std::string t = strip(tok);
                if (is_all_digits(t)) pids.insert(t);
                tok.clear();
            }
        };
        for (char c : out) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                flush();
            } else {
                tok += c;
            }
        }
        flush();
    }
    static const std::vector<std::string> keys = {
        "mlx_lm.server", "llama-server", "llama_cpp", "llama.cpp"};
    for (const std::string& pid : pids) {
        std::string cmd;
        try {
            ProcResult r = run_subprocess(
                {"ps", "-p", pid, "-o", "command="}, "", 5.0, false);
            cmd = to_lower(r.out);
        } catch (...) {
            cmd = "";
        }
        bool match = false;
        for (const std::string& k : keys) {
            if (cmd.find(k) != std::string::npos) { match = true; break; }
        }
        if (match) {
            try {
                ::kill(static_cast<pid_t>(std::strtol(pid.c_str(), nullptr, 10)), SIGKILL);
            } catch (...) {

            }
        }
    }
}

bool LOREA::wait_port_closed(const std::string& host, int port, double timeout) {
    double deadline = now_seconds() + timeout;
    while (now_seconds() < deadline) {
        if (!is_port_open(host, port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}

void LOREA::restart_inference_server() {
    const bool local = (backend == "mlx" || backend == "llama-cpp" || backend == "server");
    std::shared_ptr<Subprocess> proc = server_process;
    if (proc) {
        try {
            proc->terminate();
            proc->wait(5);
            if (proc->alive()) {
                try { proc->kill(); } catch (...) {}
            }
        } catch (...) {
            try { proc->kill(); } catch (...) {}
        }
    }
    server_process = nullptr;
    server_model = std::nullopt;
    if (!local) {
        return;
    }
    auto [host, port] = inference_hostport();

    kill_inference_procs_on_port(port);
    wait_port_closed(host, port, 12.0);

    try {
        ensure_local_server();
    } catch (...) {

    }
}

void LOREA::save_restart_reload() {
    std::string path;
    bool have_path = false;
    bool ok = false;
    try {
        std::time_t t = std::time(nullptr);
        std::tm tmv {};
        ::localtime_r(&t, &tmv);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
        path = (fs::path(ensure_sessions_dir()) /
                (std::string("wedge_recovery_") + stamp + ".json")).string();
        have_path = true;
        std::string saved = save_session(path);
        std::error_code ec;
        ok = (saved.rfind("Session saved", 0) == 0) && fs::exists(path, ec);
    } catch (const std::exception& e) {
        log_warn(std::string("Wedge recovery: save failed (") + e.what() + "); restarting server only.");
    }
    restart_inference_server();
    if (ok) {
        try {
            load_session(path);
        } catch (const std::exception& e) {
            log_warn(std::string("Wedge recovery: reload failed (") + e.what() +
                     "); continuing with in-memory messages.");
        }
    }
    (void)have_path;

    try {
        std::vector<std::string> snaps;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(fs::path(SESSIONS_DIR), ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("wedge_recovery_", 0) == 0 && ends_with(name, ".json")) {
                snaps.push_back(entry.path().string());
            }
        }
        std::sort(snaps.begin(), snaps.end());
        if (snaps.size() > 5) {
            for (size_t i = 0; i + 5 < snaps.size(); ++i) {
                std::error_code rec;
                fs::remove(snaps[i], rec);
            }
        }
    } catch (...) {

    }
}

std::optional<std::string> LOREA::server_crash_note(const std::exception& exc) {
    if (!(backend == "mlx" || backend == "llama-cpp" || backend == "server")) {
        return std::nullopt;
    }
    std::shared_ptr<Subprocess> proc = server_process;
    bool died = proc && proc->poll().has_value();
    if (died) {

        server_process = nullptr;
        server_model = std::nullopt;
    }
    const std::string where = died ? "while generating" : "on connect";
    return std::string(
        "[LOREA] The local " + backend + " inference server stopped " + where +
        " (" + exc_type_name(exc) + "). This is a GPU error, not a problem with your prompt: the "
        "Metal command buffer failed, which on this machine almost always means the GPU ran "
        "out of memory or another GPU-heavy app (a game, etc.) is competing for it — the 30B "
        "model needs most of the 32 GB on its own. Fix: quit other GPU apps, then send your "
        "message again and I'll auto-restart the server. For a smaller, more resilient model, "
        "run /model and pick a 4-8B one.");
}

}
