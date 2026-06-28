#include "lorea.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

#include <termios.h>
#include <unistd.h>

namespace ocli {
namespace {

bool json_truthy(const json& v) {
    if (v.is_null())            return false;
    if (v.is_boolean())         return v.get<bool>();
    if (v.is_number_integer())  return v.get<long long>() != 0;
    if (v.is_number_unsigned()) return v.get<unsigned long long>() != 0;
    if (v.is_number_float())    return v.get<double>() != 0.0;
    if (v.is_string())          return !v.get<std::string>().empty();
    if (v.is_array())           return !v.empty();
    if (v.is_object())          return !v.empty();
    return true;
}

std::string json_scalar_str(const json& v) {
    if (v.is_null())            return "";
    if (v.is_string())          return v.get<std::string>();
    if (v.is_boolean())         return v.get<bool>() ? "True" : "False";
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (d == static_cast<long long>(d)) return std::to_string(static_cast<long long>(d));
        std::string s = std::to_string(d);

        if (s.find('.') != std::string::npos) {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return s;
    }
    return v.dump();
}

json jget(const json& o, const std::string& k) {
    if (o.is_object()) {
        auto it = o.find(k);
        if (it != o.end()) return *it;
    }
    return json(nullptr);
}

std::string jget_or(const json& o, const std::string& k, const std::string& def) {
    json v = jget(o, k);
    return json_truthy(v) ? json_scalar_str(v) : def;
}

std::string jget_default(const json& o, const std::string& k, const std::string& def) {
    if (o.is_object()) {
        auto it = o.find(k);
        if (it != o.end()) return json_scalar_str(*it);
    }
    return def;
}

std::string strip(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool in_list(const std::string& s, std::initializer_list<const char*> items) {
    for (const char* i : items) if (s == i) return true;
    return false;
}

std::string split_after_first(const std::string& s, const std::string& sep) {
    size_t pos = s.find(sep);
    if (pos == std::string::npos) return s;
    return s.substr(pos + sep.size());
}

std::vector<std::string> splitlines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c == '\r') {
            out.push_back(cur);
            cur.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string basename_of(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string mpc_getpass(const std::string& prompt) {
    FILE* tty = std::fopen("/dev/tty", "r+");
    if (!tty) throw std::runtime_error("getpass: no tty");
    std::fputs(prompt.c_str(), tty);
    std::fflush(tty);
    int fd = ::fileno(tty);
    struct termios oldt {};
    if (::tcgetattr(fd, &oldt) != 0) { std::fclose(tty); throw std::runtime_error("getpass: tcgetattr"); }
    struct termios newt = oldt;
    newt.c_lflag &= ~(static_cast<tcflag_t>(ECHO));
    ::tcsetattr(fd, TCSAFLUSH, &newt);
    std::string line;
    int c;
    while ((c = std::fgetc(tty)) != EOF && c != '\n') line.push_back(static_cast<char>(c));
    ::tcsetattr(fd, TCSAFLUSH, &oldt);
    std::fputc('\n', tty);
    std::fclose(tty);
    return line;
}

const char* env_nonempty(const char* name) {
    const char* v = std::getenv(name);
    if (v && *v) return v;
    return nullptr;
}

}

ConnectOpts LOREA::parse_connect_args(const std::string& arg_text) {
    ConnectOpts opts;
    if (arg_text.empty()) return opts;

    std::vector<std::string> parts;
    try {
        parts = shlex_split(arg_text);
    } catch (const ShlexError& e) {
        log_info(std::string("Could not parse /connect arguments: ") + e.what());
        opts.url = strip(arg_text);
        return opts;
    }

    size_t i = 0;
    while (i < parts.size()) {
        const std::string& part = parts[i];
        std::string lowered = lower(part);
        if (in_list(lowered, {"--disconnect", "disconnect", "off", "clear", "none"})) {
            opts.disconnect = true;
            i += 1;
            continue;
        }
        if (in_list(lowered, {"--status", "status"})) {
            opts.status = true;
            opts.no_menu = true;
            i += 1;
            continue;
        }
        if (lowered == "--no-menu") {
            opts.no_menu = true;
            i += 1;
            continue;
        }
        if (part == "--token" || part == "-t") {
            if (i + 1 >= parts.size()) {
                log_info("Missing token after --token.");
                break;
            }
            opts.token = parts[i + 1];
            i += 2;
            continue;
        }
        if (starts_with(part, "--token=")) {
            size_t eq = part.find('=');
            opts.token = part.substr(eq + 1);
            i += 1;
            continue;
        }
        if (!opts.url.has_value()) {
            opts.url = part;
        } else {
            log_info("Ignoring extra /connect argument: " + part);
        }
        i += 1;
    }
    return opts;
}

std::optional<std::string> LOREA::normalize_mpc_url(const std::string& url_in) {
    std::string url = strip(url_in);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.empty()) return std::nullopt;
    if (url.find("://") == std::string::npos) {
        static const std::vector<std::string> local_prefixes = {
            "localhost", "127.", "0.0.0.0", "[::1]"};
        bool local = false;
        for (const auto& p : local_prefixes) {
            if (starts_with(url, p)) { local = true; break; }
        }
        std::string scheme = local ? "http" : "https";
        url = scheme + "://" + url;
    }
    return url;
}

std::map<std::string, std::string> LOREA::mpc_headers() {
    std::string token;
    if (mpc_token && !mpc_token->empty()) {
        token = *mpc_token;
    } else if (const char* e = env_nonempty("OCLI_MPC_TOKEN")) {
        token = e;
    } else if (const char* e2 = env_nonempty("MPC_TOKEN")) {
        token = e2;
    }
    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    if (!token.empty()) {
        headers["Authorization"] = "Bearer " + token;
        headers["X-MPC-Token"] = token;
    }
    return headers;
}

json LOREA::mpc_request(const std::string& method, const std::string& path,
                        const json* json_body,
                        const std::map<std::string, std::string>* params,
                        const std::map<std::string, std::string>* extra_headers,
                        long timeout_s) {
    if (!mpc_url || mpc_url->empty()) {
        throw MpcNoConnection("No MPC server is connected. Run /connect <cloudflared-url> first.");
    }
    std::map<std::string, std::string> headers = mpc_headers();
    if (extra_headers) {
        for (const auto& kv : *extra_headers) headers[kv.first] = kv.second;
    }

    HttpRequest req;
    req.method = method;
    std::string full_url = *mpc_url + path;
    if (params && !params->empty()) {
        std::string q = build_query(*params);
        if (!q.empty()) full_url += "?" + q;
    }
    req.url = full_url;
    for (const auto& kv : headers) req.headers.emplace_back(kv.first, kv.second);
    if (json_body) {
        req.body = json_body->dump();
        req.headers.emplace_back("Content-Type", "application/json");
    }
    req.timeout_ms = timeout_s * 1000;
    req.follow_redirects = true;

    HttpResponse resp = http_perform(req);
    if (!resp.error.empty()) throw std::runtime_error(resp.error);
    if (resp.status == 401) throw MpcUnauthorized("MPC token is required or invalid.");
    if (resp.status >= 400) {
        throw HttpStatusError(resp.status,
                              std::to_string(resp.status) + " Error for url: " + full_url);
    }
    if (resp.body.empty()) return json::object();
    try {
        return json::parse(resp.body);
    } catch (...) {
        return json{{"raw", resp.body}};
    }
}

void LOREA::connect_mpc_command(const std::string& arg_text) {
    ConnectOpts opts = parse_connect_args(arg_text);
    if (opts.disconnect) {
        disconnect_mpc();
        log_info("Disconnected from the MPC server.");
        return;
    }

    std::string url_in;
    if (opts.url && !opts.url->empty()) {
        url_in = *opts.url;
    } else if (mpc_url && !mpc_url->empty()) {
        url_in = *mpc_url;
    } else {
        auto pv = prompt_value("MPC server URL", std::string(MPC_DEFAULT_URL));
        url_in = pv.value_or("");
    }
    auto url = normalize_mpc_url(url_in);
    if (!url) {
        log_info("MPC connection canceled.");
        return;
    }

    std::optional<std::string> previous_url = mpc_url;
    std::optional<std::string> previous_token = mpc_token;
    mpc_url = *url;
    if (opts.token.has_value()) {
        mpc_token = opts.token;
    }

    json status;
    try {
        status = mpc_request("GET", "/status");
    } catch (const MpcUnauthorized&) {
        std::string token;
        try {
            token = strip(mpc_getpass("  MPC token: "));
        } catch (...) {
            auto pv = prompt_value("MPC token");
            token = pv.value_or("");
        }
        mpc_token = token.empty() ? std::nullopt : std::optional<std::string>(token);
        try {
            status = mpc_request("GET", "/status");
        } catch (const std::exception& e) {
            mpc_url = previous_url;
            mpc_token = previous_token;
            log_warn(std::string("Could not connect to MPC server: ") + e.what());
            return;
        }
    } catch (const std::exception& e) {
        mpc_url = previous_url;
        mpc_token = previous_token;
        log_warn(std::string("Could not connect to MPC server: ") + e.what());
        return;
    }

    json featv = (status.is_object() && status.contains("features")) ? status["features"] : json(nullptr);
    mpc_features = featv.is_object() ? featv : json::object();

    if (status.is_object() && status.contains("version") && !status["version"].is_null()) {
        mpc_version = json_scalar_str(status["version"]);
    } else {
        mpc_version = std::nullopt;
    }

    bool has_version = mpc_version && !mpc_version->empty();
    std::string vsuffix = has_version
        ? std::string(" ") + Colors::DIM + Colors::GRAY + "v" + *mpc_version + Colors::RESET
        : std::string();
    log_ok(std::string("Connected to MPC server ") + Colors::WHITE + *mpc_url + Colors::RESET + vsuffix);
    print_mpc_status(&status);
    sync_mpc_selection(status);
    if (!opts.status && !opts.no_menu) {
        mpc_control_menu();
    }
}

void LOREA::disconnect_mpc() {
    mpc_url = std::nullopt;
    mpc_token = std::nullopt;
    mpc_features = json::object();
    mpc_version = std::nullopt;
}

bool LOREA::mpc_supports(const std::string& feature) {
    if (!json_truthy(mpc_features)) return false;
    if (!mpc_features.is_object()) return false;
    return json_truthy(jget(mpc_features, feature));
}

void LOREA::print_mpc_status(const json* status) {
    if (!mpc_url || mpc_url->empty()) {
        log_info("No MPC server is connected.");
        return;
    }
    json st;
    if (status && json_truthy(*status)) {
        st = *status;
    } else {
        try {
            st = mpc_request("GET", "/status");
        } catch (const std::exception& e) {
            log_warn(std::string("Could not read MPC status: ") + e.what());
            return;
        }
    }

    std::string selected_backend = jget_or(st, "selected_backend", "none");
    std::string selected_model = jget_or(st, "selected_model", "none");

    json downloads_v = jget(st, "downloads");
    json downloads = json_truthy(downloads_v) ? downloads_v : json::array();
    int active = 0;
    if (downloads.is_array()) {
        for (const auto& job : downloads) {
            std::string s = job.is_object() ? json_scalar_str(jget(job, "status")) : "";
            if (s == "queued" || s == "running") ++active;
        }
    }

    std::string mver = mpc_version.value_or("");
    std::string version = jget_or(st, "version", mver);
    std::string name = jget_default(st, "name", "LOREA MPC") + (version.empty() ? "" : (" v" + version));

    json featv = jget(st, "features");
    json features = featv.is_object() ? featv : mpc_features;
    std::vector<std::string> feature_names;
    if (features.is_object()) {
        for (auto it = features.begin(); it != features.end(); ++it) {
            if (json_truthy(it.value())) feature_names.push_back(it.key());
        }
    }
    if (feature_names.empty()) feature_names.push_back("legacy (no streaming)");
    std::sort(feature_names.begin(), feature_names.end());
    std::string feats;
    for (size_t i = 0; i < feature_names.size(); ++i) {
        if (i) feats += ", ";
        feats += feature_names[i];
    }

    std::string models_dir = jget_default(st, "models_dir", "default");
    int total = downloads.is_array() ? static_cast<int>(downloads.size()) : 0;

    print_panel("mpc server", {
        kv_row("url",        std::string(Colors::WHITE) + *mpc_url + Colors::RESET),
        kv_row("name",       std::string(Colors::WHITE) + name + Colors::RESET),
        kv_row("selected",   std::string(Colors::WHITE) + selected_backend + ":" + selected_model + Colors::RESET),
        kv_row("features",   std::string(Colors::WHITE) + feats + Colors::RESET),
        kv_row("models dir", std::string(Colors::WHITE) + models_dir + Colors::RESET),
        kv_row("downloads",  std::string(Colors::WHITE) + std::to_string(active) + Colors::RESET +
                             " active / " + Colors::WHITE + std::to_string(total) + Colors::RESET + " total"),
    }, Colors::CYAN);
}

void LOREA::print_mpc_downloads() {
    if (!mpc_url || mpc_url->empty()) {
        log_info("No MPC server is connected.");
        return;
    }
    json data;
    try {
        data = mpc_request("GET", "/downloads");
    } catch (const std::exception& e) {
        log_warn(std::string("Could not read MPC downloads: ") + e.what());
        return;
    }
    json jobs_v = jget(data, "downloads");
    json jobs = json_truthy(jobs_v) ? jobs_v : json::array();
    if (!jobs.is_array() || jobs.empty()) {
        log_info("No MPC downloads have been started.");
        return;
    }
    std::vector<std::string> lines;
    size_t start = jobs.size() > 12 ? jobs.size() - 12 : 0;
    for (size_t i = start; i < jobs.size(); ++i) {
        const json& job = jobs[i];
        std::string idv = jget_default(job, "id", "");
        std::string label = utf8_substr(idv, 0, 8) + "  " + jget_default(job, "status", "unknown");
        std::string target = jget_default(job, "backend", "?") + ":" + jget_default(job, "model", "?");
        std::string path = jget_or(job, "path", "");
        lines.push_back(std::string(Colors::WHITE) + label + Colors::RESET + "  " +
                        Colors::GRAY + target + Colors::RESET);
        if (!path.empty()) {
            lines.push_back(std::string("  ") + Colors::DIM + Colors::GRAY + path + Colors::RESET);
        }
    }
    print_panel("mpc downloads", lines, Colors::CYAN);
}

void LOREA::mpc_control_menu() {
    if (!mpc_url || mpc_url->empty()) {
        log_info("No MPC server is connected.");
        return;
    }
    while (true) {
        std::vector<std::string> options;
        options.push_back(std::string("1. ") + Colors::CYAN + "Download/select remote model" + Colors::RESET);
        options.push_back(std::string("2. ") + Colors::CYAN + "Show server status" + Colors::RESET);
        options.push_back(std::string("3. ") + Colors::CYAN + "Show downloads" + Colors::RESET);
        if (mpc_supports("delete")) {
            options.push_back(std::string("4. ") + Colors::CYAN + "Delete a remote model" + Colors::RESET);
        }
        options.push_back(std::to_string(options.size() + 1) + ". " + Colors::ORANGE + "Disconnect" + Colors::RESET);
        options.push_back(std::to_string(options.size() + 1) + ". " + Colors::GRAY + "Close" + Colors::RESET);

        auto choice = menu_choice("MPC", options);
        if (!choice) return;
        std::string label = strip(clean_ansi(options[*choice]));
        std::string after = split_after_first(label, ". ");
        if (starts_with(after, "Download/select")) {
            download_mpc_model_menu();
            return;
        }
        if (ends_with(label, "Show server status")) {
            print_mpc_status();
        } else if (ends_with(label, "Show downloads")) {
            print_mpc_downloads();
        } else if (ends_with(label, "Delete a remote model")) {
            delete_mpc_model_menu();
        } else if (ends_with(label, "Disconnect")) {
            disconnect_mpc();
            log_info("Disconnected from the MPC server.");
            return;
        } else if (ends_with(label, "Close")) {
            return;
        }
    }
}

void LOREA::delete_mpc_model_menu() {
    if (!mpc_url || mpc_url->empty()) {
        log_info("No MPC server is connected.");
        return;
    }
    auto backend_opt = choose_mpc_backend();
    if (!backend_opt || backend_opt->empty()) return;
    std::string be = *backend_opt;

    json catalog = mpc_model_catalog(be);
    json installed_v = jget(catalog, "installed");
    json installed = json_truthy(installed_v) ? installed_v : json::array();

    std::vector<std::string> models;
    std::set<std::string> seen;
    if (installed.is_array()) {
        for (const auto& item : installed) {
            std::string model;
            if (item.is_object()) {
                json mv = jget(item, "model");
                if (json_truthy(mv)) model = json_scalar_str(mv);
            } else {
                model = json_scalar_str(item);
            }
            if (!model.empty() && !seen.count(model)) {
                seen.insert(model);
                models.push_back(model);
            }
        }
    }
    if (models.empty()) {
        log_info("No installed " + be + " models to delete.");
        return;
    }
    std::vector<std::string> options;
    for (size_t i = 0; i < models.size(); ++i) {
        options.push_back(std::to_string(i + 1) + ". " + Colors::WHITE + models[i] + Colors::RESET);
    }
    auto choice = menu_choice("DELETE MODEL", options);
    if (!choice) return;
    std::string target = models[*choice];
    auto confirm = prompt_value("Type 'yes' to delete " + target);
    if (lower(strip(confirm.value_or(""))) != "yes") {
        log_info("Delete canceled.");
        return;
    }
    try {
        json body = {{"backend", be}, {"model", target}};
        mpc_request("POST", "/delete", &body, nullptr, nullptr, 120);
        log_ok(std::string("Deleted ") + Colors::WHITE + be + ":" + target + Colors::RESET + " on the MPC server.");
    } catch (const std::exception& e) {
        log_warn(std::string("Could not delete MPC model: ") + e.what());
    }
}

std::optional<std::string> LOREA::choose_mpc_backend() {
    std::vector<std::string> backends = {"ollama", "llama-cpp", "mlx", "airllm"};
    std::vector<std::string> labels;
    for (const auto& b : backends) {
        std::string marker = (b == backend) ? "current" : "remote";
        labels.push_back(std::string(Colors::CYAN) + b + Colors::RESET + " " +
                         Colors::GRAY + marker + Colors::RESET);
    }
    auto choice = menu_choice("MPC BACKEND", labels);
    if (!choice) return std::nullopt;
    return backends[*choice];
}

json LOREA::mpc_model_catalog(const std::string& backend_arg) {
    try {
        std::map<std::string, std::string> params = {{"backend", backend_arg}};
        return mpc_request("GET", "/models", nullptr, &params);
    } catch (const std::exception& e) {
        log_warn(std::string("Could not fetch MPC model catalog: ") + e.what());
        return json{{"backend", backend_arg}, {"models", json::array()}, {"installed", json::array()}};
    }
}

bool LOREA::select_mpc_model(const std::string& backend_arg, const std::string& model_name_arg) {
    try {
        json body = {{"backend", backend_arg}, {"model", model_name_arg}};
        json data = mpc_request("POST", "/select", &body);
        std::string selected = jget_or(data, "selected_model", model_name_arg);
        backend = jget_or(data, "selected_backend", backend_arg);
        model_name = selected;
        log_ok(std::string("MPC selected ") + Colors::WHITE + backend_arg + ":" + selected + Colors::RESET);
        return true;
    } catch (const std::exception& e) {
        log_warn(std::string("Could not select MPC model: ") + e.what());
        return false;
    }
}

void LOREA::sync_mpc_selection(const json& status) {
    json bk = status.is_object() ? jget(status, "selected_backend") : json(nullptr);
    json md = status.is_object() ? jget(status, "selected_model") : json(nullptr);
    if (bk.is_string() && BACKEND_DEFAULT_URLS.count(bk.get<std::string>()) && json_truthy(md)) {
        backend = bk.get<std::string>();
        model_name = json_scalar_str(md);
        server_model = std::nullopt;
        log_info(std::string("Using MPC model ") + Colors::TEAL + backend + ":" + model_name + Colors::RESET);
    }
}

std::tuple<std::string, std::vector<ToolCall>, json>
LOREA::mpc_chat(const json& messages_arg, const json& tools_arg) {
    json payload = {
        {"backend", backend},
        {"model", model_name},
        {"messages", messages_arg},
        {"tools", tools_arg},
    };
    json data = mpc_request("POST", "/chat", &payload, nullptr, nullptr, MPC_CHAT_TIMEOUT);

    json sb = jget(data, "selected_backend");
    if (sb.is_string() && BACKEND_DEFAULT_URLS.count(sb.get<std::string>())) {
        backend = sb.get<std::string>();
    }
    json sm = jget(data, "selected_model");
    if (json_truthy(sm)) model_name = json_scalar_str(sm);

    std::vector<ToolCall> tool_calls;
    json tcl = jget(data, "tool_calls");
    if (tcl.is_array()) {
        for (const auto& call : tcl) tool_calls.push_back(normalize_tool_call(call));
    }

    json mdv = jget(data, "metadata");
    json metadata = (json_truthy(mdv) && mdv.is_object()) ? mdv : json::object();
    metadata["mpc"] = true;
    metadata["backend"] = jget_or(data, "backend", backend);
    metadata["model"] = jget_or(data, "model", model_name);

    std::string content = jget_or(data, "content", "");
    return {content, tool_calls, metadata};
}

std::tuple<std::string, std::vector<ToolCall>, json>
LOREA::mpc_chat_stream(const json& messages_arg, const json& tools_arg,
                       std::function<void(const std::string&)> on_token,
                       std::function<bool()> should_stop) {
    json payload = {
        {"backend", backend},
        {"model", model_name},
        {"messages", messages_arg},
        {"tools", tools_arg},
        {"stream", true},
    };

    std::vector<std::string> content_parts;
    std::vector<json> tool_calls;
    json metadata = json::object();
    metadata["mpc"] = true;

    bool saw_done = false;
    bool have_error = false, error_retryable = false;
    std::string error_msg;
    bool stop_requested = false;
    std::string raw_body;
    std::string line_buf;

    auto process_line = [&](const std::string& raw) -> bool {
        if (should_stop && should_stop()) { stop_requested = true; return false; }
        if (raw.empty()) return true;
        json event;
        try {
            event = json::parse(raw);
        } catch (...) {
            return true;
        }
        if (!event.is_object()) return true;
        std::string etype = event.contains("type") ? json_scalar_str(event["type"]) : "";
        if (etype == "content") {
            std::string chunk = jget_or(event, "content", "");
            content_parts.push_back(chunk);
            if (!chunk.empty() && on_token) on_token(chunk);
        } else if (etype == "tool_calls") {
            json tcs = jget(event, "tool_calls");
            if (tcs.is_array()) {
                for (const auto& c : tcs) tool_calls.push_back(normalize_tool_call(c));
            }
        } else if (etype == "metadata") {
            json md = jget(event, "metadata");
            if (json_truthy(md) && md.is_object()) metadata.update(md);
        } else if (etype == "done") {
            saw_done = true;
            apply_mpc_selection(event);
        } else if (etype == "error") {
            error_msg = jget_or(event, "error", "MPC stream error.");
            error_retryable = json_truthy(jget(event, "retryable"));
            have_error = true;
            return false;
        }
        return true;
    };

    auto on_chunk = [&](const char* data, size_t n) -> bool {
        if (should_stop && should_stop()) { stop_requested = true; return false; }
        raw_body.append(data, n);
        line_buf.append(data, n);
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!process_line(line)) return false;
        }
        return true;
    };

    HttpRequest req;
    req.method = "POST";
    req.url = (mpc_url ? *mpc_url : std::string()) + "/chat";
    {
        std::map<std::string, std::string> headers = mpc_headers();
        for (const auto& kv : headers) req.headers.emplace_back(kv.first, kv.second);
    }
    req.body = payload.dump();
    req.headers.emplace_back("Content-Type", "application/json");
    req.timeout_ms = static_cast<long>(MPC_CHAT_TIMEOUT) * 1000;
    req.follow_redirects = true;

    HttpResponse resp = http_stream(req, on_chunk);

    if (!stop_requested && !have_error && !line_buf.empty()) {
        std::string line = line_buf;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        process_line(line);
    }

    if (resp.status == 401) throw MpcUnauthorized("MPC token is required or invalid.");

    std::string joined;
    for (const auto& p : content_parts) joined += p;

    if (resp.network_error && tool_calls.empty() && strip(joined).empty() && !have_error) {
        throw MPCRetryable(std::string("MPC stream dropped: ") +
                           (resp.error.empty() ? std::string("connection error") : resp.error));
    }

    std::string ctype = resp.content_type();
    if (ctype.find("application/x-ndjson") == std::string::npos) {

        if (resp.status >= 400) {
            throw HttpStatusError(resp.status,
                                  std::to_string(resp.status) + " Error for url: " + req.url);
        }
        json data;
        bool parsed = true;
        try {
            data = json::parse(raw_body);
        } catch (...) {
            parsed = false;
        }
        if (!parsed) {
            std::string t = utf8_substr(raw_body, 0, 500);
            throw std::runtime_error(!t.empty() ? t : std::string("Empty MPC response."));
        }
        if (json_truthy(jget(data, "error"))) {
            throw std::runtime_error(json_scalar_str(data["error"]));
        }
        std::string content = jget_or(data, "content", "");
        if (!content.empty() && on_token) on_token(content);
        std::vector<json> tcs2;
        json tcl = jget(data, "tool_calls");
        if (tcl.is_array()) {
            for (const auto& c : tcl) tcs2.push_back(normalize_tool_call(c));
        }
        json md = jget(data, "metadata");
        if (json_truthy(md) && md.is_object()) metadata.update(md);
        apply_mpc_selection(data);
        std::vector<ToolCall> out;
        for (const auto& c : tcs2) if (json_truthy(c)) out.push_back(c);
        return {content, out, metadata};
    }

    if (resp.status >= 400) {
        throw HttpStatusError(resp.status,
                              std::to_string(resp.status) + " Error for url: " + req.url);
    }
    if (have_error) {
        if (error_retryable) throw MPCRetryable(error_msg);
        throw std::runtime_error(error_msg);
    }

    if (!saw_done && tool_calls.empty() && strip(joined).empty() &&
        !(should_stop && should_stop())) {
        throw MPCRetryable("MPC stream ended early with no output.");
    }

    metadata["backend"] = backend;
    metadata["model"] = model_name;
    std::vector<ToolCall> out;
    for (const auto& c : tool_calls) if (json_truthy(c)) out.push_back(c);
    return {joined, out, metadata};
}

void LOREA::apply_mpc_selection(const json& data) {
    json sb = jget(data, "selected_backend");
    if (sb.is_string() && BACKEND_DEFAULT_URLS.count(sb.get<std::string>())) {
        backend = sb.get<std::string>();
    }
    json sm = jget(data, "selected_model");
    if (json_truthy(sm)) model_name = json_scalar_str(sm);
}

void LOREA::start_mpc_download(const std::string& backend_arg, const std::string& model_name_arg,
                               std::optional<std::string> url, std::optional<std::string> download_dir) {
    if (!mpc_url || mpc_url->empty()) {
        log_info("No MPC server is connected. Run /connect <cloudflared-url> first.");
        return;
    }
    if (model_name_arg.empty()) return;
    json payload = {{"backend", backend_arg}, {"model", model_name_arg}};
    if (url && !url->empty()) payload["url"] = *url;
    if (download_dir && !download_dir->empty()) payload["path"] = *download_dir;

    json data;
    try {
        data = mpc_request("POST", "/download", &payload, nullptr, nullptr, 60);
    } catch (const std::exception& e) {
        log_warn(std::string("Could not start MPC download: ") + e.what());
        return;
    }
    json jid = jget(data, "job_id");
    if (!json_truthy(jid)) {
        log_warn("MPC server did not return a download job: " + data.dump());
        return;
    }
    std::string job_id = json_scalar_str(jid);
    log_ok(std::string("MPC download started ") + Colors::DIM + Colors::GRAY + "job=" + job_id + Colors::RESET);
    wait_for_mpc_download(job_id);
}

bool LOREA::cancel_mpc_download(const std::string& job_id) {
    if (!mpc_supports("cancel")) {
        log_info("This MPC server does not support canceling downloads.");
        return false;
    }
    try {
        json body = {{"job_id", job_id}};
        mpc_request("POST", "/cancel", &body, nullptr, nullptr, 30);
        log_info("Requested cancel for MPC download " + utf8_substr(job_id, 0, 8) + ".");
        return true;
    } catch (const std::exception& e) {
        log_warn(std::string("Could not cancel MPC download: ") + e.what());
        return false;
    }
}

void LOREA::wait_for_mpc_download(const std::string& job_id) {
    std::optional<std::string> last_status;
    std::string last_tail;
    bool can_cancel = mpc_supports("cancel") && can_use_terminal_keys();
    bool listening = can_cancel;
    if (can_cancel) {
        log_info(std::string("Downloading on the server. ") + Colors::DIM + Colors::GRAY +
                 "Press Esc to cancel." + Colors::RESET);
        interrupter.start_listening();
    }

    struct ListenGuard {
        bool active;
        ~ListenGuard() { if (active) interrupter.stop_listening(); }
    } guard{listening};

    try {
        while (true) {
            if (can_cancel && interrupter.interrupted.is_set()) {
                interrupter.interrupted.clear();
                cancel_mpc_download(job_id);
                can_cancel = false;
            }
            json data = mpc_request("GET", "/downloads/" + job_id, nullptr, nullptr, nullptr, 60);
            std::string status = jget_or(data, "status", "unknown");
            std::string tail = strip(jget_or(data, "log_tail", ""));
            if (!last_status || *last_status != status) {
                log_info("MPC download " + utf8_substr(job_id, 0, 8) + " is " +
                         Colors::WHITE + status + Colors::RESET);
                last_status = status;
            }
            if (!tail.empty() && tail != last_tail) {
                std::vector<std::string> sl = splitlines(tail);
                size_t s0 = sl.size() > 4 ? sl.size() - 4 : 0;
                std::string shown;
                for (size_t k = s0; k < sl.size(); ++k) {
                    if (k > s0) shown += "\n";
                    shown += sl[k];
                }
                print_panel("mpc download log", {shown}, Colors::CYAN);
                last_tail = tail;
            }
            if (status == "completed" || status == "failed" || status == "canceled") {
                if (status == "completed") {
                    json p = jget(data, "path");
                    std::string path = json_scalar_str(p);
                    log_ok(std::string("MPC download completed") +
                           (json_truthy(p) ? (std::string(" at ") + Colors::WHITE + path + Colors::RESET)
                                           : std::string()));
                } else if (status == "canceled") {
                    log_info("MPC download canceled.");
                } else {
                    json err = jget(data, "error");
                    log_warn("MPC download failed: " +
                             (json_truthy(err) ? json_scalar_str(err) : std::string("unknown error")));
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(MPC_DOWNLOAD_POLL_INTERVAL));
        }
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "KeyboardInterrupt") {
            log_info(std::string("MPC download continues on the server. Job id: ") +
                     Colors::WHITE + job_id + Colors::RESET);
        } else {
            log_warn("Stopped polling MPC download " + job_id + ": " + e.what());
        }
    }
}

void LOREA::download_mpc_model_menu(std::optional<std::string> model_name_arg,
                                    std::optional<std::string> download_dir) {
    if (!mpc_url || mpc_url->empty()) {
        log_info("No MPC server is connected. Run /connect <cloudflared-url> first.");
        return;
    }
    bool have_model = model_name_arg && !model_name_arg->empty();

    std::optional<std::string> backend_opt;
    if (have_model) {
        backend_opt = backend;
    } else {
        backend_opt = choose_mpc_backend();
    }
    if (!backend_opt || backend_opt->empty()) return;
    std::string be = *backend_opt;

    if (have_model) {
        select_mpc_model(be, *model_name_arg);
        start_mpc_download(be, *model_name_arg, std::nullopt, download_dir);
        return;
    }

    json catalog = mpc_model_catalog(be);
    json installed_v = jget(catalog, "installed");
    json installed = json_truthy(installed_v) ? installed_v : json::array();
    json models_v = jget(catalog, "models");
    json models = json_truthy(models_v) ? models_v : json::array();

    struct Choice {
        std::string label;
        std::string model;
        std::optional<std::string> url;
    };
    std::vector<Choice> choices;
    std::set<std::string> seen;

    if (installed.is_array()) {
        for (const auto& item : installed) {
            std::string model;
            if (item.is_object()) {
                json mv = jget(item, "model");
                if (json_truthy(mv)) model = json_scalar_str(mv);
            } else {
                model = json_scalar_str(item);
            }
            if (model.empty() || seen.count(model)) continue;
            seen.insert(model);
            choices.push_back({"installed: " + model, model, std::nullopt});
        }
    }
    if (models.is_array()) {
        for (const auto& item : models) {
            std::string model, label;
            std::optional<std::string> url;
            if (item.is_object()) {
                json mv = jget(item, "model");
                if (json_truthy(mv)) {
                    model = json_scalar_str(mv);
                } else {
                    json vv = jget(item, "value");
                    if (json_truthy(vv)) {
                        model = json_scalar_str(vv);
                    } else {
                        json nv = jget(item, "name");
                        if (json_truthy(nv)) model = json_scalar_str(nv);
                    }
                }
                json lv = jget(item, "label");
                label = json_truthy(lv) ? json_scalar_str(lv) : model;
                json uv = jget(item, "url");
                if (json_truthy(uv)) url = json_scalar_str(uv);
            } else {
                model = json_scalar_str(item);
                label = model;
            }
            if (model.empty() || seen.count(model)) continue;
            seen.insert(model);
            choices.push_back({label, model, url});
        }
    }

    std::vector<std::string> options;
    for (size_t i = 0; i < choices.size(); ++i) {
        options.push_back(std::to_string(i + 1) + ". " + Colors::CYAN + choices[i].label + Colors::RESET +
                          " " + Colors::GRAY + choices[i].model + Colors::RESET);
    }
    options.push_back(std::to_string(options.size() + 1) + ". " + Colors::ORANGE +
                      "Type a custom model or URL" + Colors::RESET);

    auto choice_index = menu_choice("MPC MODEL", options);
    if (!choice_index) return;
    if (static_cast<size_t>(*choice_index) < choices.size()) {
        const Choice& selected = choices[*choice_index];
        select_mpc_model(be, selected.model);
        start_mpc_download(be, selected.model, selected.url, download_dir);
        return;
    }

    if (be == "llama-cpp") {
        auto custom_o = prompt_value("GGUF URL or Hugging Face repo");
        std::string custom = custom_o.value_or("");
        if (custom.empty()) {
            log_info("MPC download canceled.");
            return;
        }
        if (starts_with(custom, "http://") || starts_with(custom, "https://")) {
            std::string before_q = custom.substr(0, custom.find('?'));
            std::string base = basename_of(before_q);
            std::string def = base.empty() ? std::string("model.gguf") : base;
            auto save_o = prompt_value("Save as", def);
            std::string save_as = save_o.value_or("");
            select_mpc_model(be, save_as);
            start_mpc_download(be, save_as, custom, download_dir);
            return;
        }
        select_mpc_model(be, custom);
        start_mpc_download(be, custom, std::nullopt, download_dir);
        return;
    }

    auto custom_o = prompt_value("Model to download", model_name);
    std::string custom = custom_o.value_or("");
    if (custom.empty()) {
        log_info("MPC download canceled.");
        return;
    }
    select_mpc_model(be, custom);
    start_mpc_download(be, custom, std::nullopt, download_dir);
}

}
