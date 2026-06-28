#include "lorea.hpp"

#include "secutil.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace ocli {

std::string current_executable_path();

namespace {

const char* const PY_WS = " \t\n\r\f\v";

std::string find_nvidia_backend() {
    namespace fs = std::filesystem;
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return !p.empty() && fs::exists(p, ec);
    };
    if (const char* e = std::getenv("OCLI_NVIDIA_BACKEND"); e && *e && exists(e))
        return std::string(e);
    std::vector<std::string> cands;
    std::string exe = current_executable_path();
    if (!exe.empty()) {
        std::string dir = fs::path(exe).parent_path().string();
        cands.push_back(dir + "/nvidia_backend.py");
        cands.push_back(fs::path(dir).parent_path().string() + "/nvidia_backend.py");
    }
    cands.push_back(expanduser("~/ocli-cpp/nvidia_backend.py"));
    cands.push_back("nvidia_backend.py");
    for (const auto& c : cands)
        if (exists(c)) {
            std::error_code ec;
            std::string canon = fs::weakly_canonical(c, ec).string();
            return ec ? c : canon;
        }
    return "";
}

std::string strip(const std::string& s) {
    std::size_t b = s.find_first_not_of(PY_WS);
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(PY_WS);
    return s.substr(b, e - b + 1);
}
std::string lstrip(const std::string& s) {
    std::size_t b = s.find_first_not_of(PY_WS);
    if (b == std::string::npos) return "";
    return s.substr(b);
}
std::string rstrip_ws(const std::string& s) {
    std::size_t e = s.find_last_not_of(PY_WS);
    if (e == std::string::npos) return "";
    return s.substr(0, e + 1);
}
std::string lower_ascii(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}
std::string upper_ascii(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}
std::string capitalize(const std::string& s) {
    if (s.empty()) return s;
    std::string o = lower_ascii(s);
    o[0] = (char)std::toupper((unsigned char)o[0]);
    return o;
}
bool startswith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endswith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
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
std::vector<std::string> split_char(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string join_ws(const std::string& s) {
    std::string out;
    bool first = true;
    std::size_t i = 0, n = s.size();
    std::string ws(PY_WS);
    while (i < n) {
        while (i < n && ws.find(s[i]) != std::string::npos) ++i;
        if (i >= n) break;
        std::size_t st = i;
        while (i < n && ws.find(s[i]) == std::string::npos) ++i;
        if (!first) out += ' ';
        first = false;
        out += s.substr(st, i - st);
    }
    return out;
}
std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
    return out;
}

bool jtruthy(const json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<long long>() != 0;
    if (v.is_number_unsigned()) return v.get<unsigned long long>() != 0;
    if (v.is_number_float()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    if (v.is_array()) return !v.empty();
    if (v.is_object()) return !v.empty();
    return true;
}

const json* jget(const json& o, const char* key) {
    if (o.is_object()) {
        auto it = o.find(key);
        if (it != o.end()) return &(*it);
    }
    return nullptr;
}

json jval(const json& o, const char* key) {
    const json* p = jget(o, key);
    return p ? *p : json(nullptr);
}

std::string py_str(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) { std::ostringstream o; o << v.get<double>(); return o.str(); }
    return v.dump();
}

std::string str_or_empty(const json& v) { return jtruthy(v) ? py_str(v) : std::string(); }

std::string get_str(const json& o, const char* key, const std::string& def = "") {
    const json* p = jget(o, key);
    if (!p) return def;
    if (p->is_string()) return p->get<std::string>();
    return py_str(*p);
}

json arr_of(const std::vector<json>& v) {
    json a = json::array();
    for (const auto& m : v) a.push_back(m);
    return a;
}

long now_unix() { return (long)std::time(nullptr); }

std::string last_cp(const std::string& s, std::size_t n) {
    std::size_t total = utf8_len(s);
    std::size_t start = (n >= total) ? 0 : total - n;
    return utf8_substr(s, start);
}

std::string drop_last_cp(const std::string& s, std::size_t k) {
    if (k == 0) return "";
    std::size_t total = utf8_len(s);
    if (k >= total) return "";
    return utf8_substr(s, 0, total - k);
}

std::string regex_escape(const std::string& s) {
    static const std::string special = "\\^$.|?*+()[]{}";
    std::string out;
    for (char c : s) {
        if (special.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

std::vector<std::string> findall_group1(const std::regex& re, const std::string& text) {
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re), end = std::sregex_iterator();
         it != end; ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

std::optional<json> try_parse(const std::string& s) {
    try { return json::parse(s); } catch (...) { return std::nullopt; }
}

const std::set<std::string>& accepted_args(const std::string& name) {
    static const std::map<std::string, std::set<std::string>> T = {
        {"run_cmd", {"command"}},
        {"read_file", {"path"}},
        {"write_file", {"path", "content"}},
        {"web_search", {"query", "num_results"}},
        {"create_plan", {"plan"}},
        {"update_task", {"index", "status"}},
        {"test_cmd", {"command"}},
        {"send_input", {"text"}},
        {"read_url", {"url"}},
        {"download_mlx_model", {"repo_id", "download_dir"}},
        {"list_files", {"path"}},
        {"search_files", {"query", "path"}},
        {"find_files", {"query", "path"}},
        {"grep", {"pattern", "path"}},
        {"git_status", {}},
        {"git_diff", {"path"}},
        {"http_request", {"url", "method", "data", "headers", "params", "json_body",
                          "follow_redirects", "cookies"}},
        {"spawn_agents", {"agents", "shared_context", "timeout_seconds", "max_steps",
                          "tool_access"}},
    };
    static const std::set<std::string> EMPTY;
    auto it = T.find(name);
    return it != T.end() ? it->second : EMPTY;
}

const std::string SYSPROF =
    "system_profiler SPDisplaysDataType SPHardwareDataType | sed -n "
    "'/Chipset Model/p;/VRAM/p;/Total Number of Cores/p;/Memory:/p;/Model Name/p;/Chip/p'";

const char* const BASE_TOOLS_JSON = R"json([
{"type":"function","function":{"name":"run_cmd","description":"Run shell command","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}}},
{"type":"function","function":{"name":"web_search","description":"Search web via DuckDuckGo. Use authoritative domains for official-source searches.","parameters":{"type":"object","properties":{"query":{"type":"string"},"num_results":{"type":"integer","default":10}},"required":["query"]}}},
{"type":"function","function":{"name":"read_url","description":"Fetch and read the text content of a URL.","parameters":{"type":"object","properties":{"url":{"type":"string"}},"required":["url"]}}},
{"type":"function","function":{"name":"download_mlx_model","description":"Download an MLX model from Hugging Face.","parameters":{"type":"object","properties":{"repo_id":{"type":"string"},"download_dir":{"type":"string","description":"Optional local directory for the downloaded model files."}},"required":["repo_id"]}}},
{"type":"function","function":{"name":"read_file","description":"Read file","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}},
{"type":"function","function":{"name":"write_file","description":"Write file","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}}},
{"type":"function","function":{"name":"test_cmd","description":"Run command with live feedback (use for interactive tests or long processes).","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}}},
{"type":"function","function":{"name":"send_input","description":"Send text input to the active test process.","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}},
{"type":"function","function":{"name":"list_files","description":"List files and directories in a path (tree view, max depth 3).","parameters":{"type":"object","properties":{"path":{"type":"string","default":"."}},"required":[]}}},
{"type":"function","function":{"name":"search_files","description":"Find files by name pattern.","parameters":{"type":"object","properties":{"query":{"type":"string"},"path":{"type":"string","default":"."}},"required":["query"]}}},
{"type":"function","function":{"name":"find_files","description":"Alias for search_files. Find files by name pattern.","parameters":{"type":"object","properties":{"query":{"type":"string"},"path":{"type":"string","default":"."}},"required":["query"]}}},
{"type":"function","function":{"name":"grep","description":"Search file contents for a pattern (like grep -rIn).","parameters":{"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string","default":"."}},"required":["pattern"]}}},
{"type":"function","function":{"name":"git_status","description":"Show git status of the current repo.","parameters":{"type":"object","properties":{},"required":[]}}},
{"type":"function","function":{"name":"git_diff","description":"Show git diff of changes. Optionally for a specific file.","parameters":{"type":"object","properties":{"path":{"type":"string","default":""}},"required":[]}}},
{"type":"function","function":{"name":"http_request","description":"Send an HTTP request WITHOUT a shell, for authorized local web pentesting. Use this instead of curl-via-run_cmd whenever a payload contains quotes/spaces (e.g. SQL injection admin' --): pass the payload as a structured field and it is sent verbatim, no shell quoting to get wrong. Keeps a cookie jar across calls and follows redirects, so a successful login lands on the post-auth page and returns its body. Scope: loopback/LAN only by default.","parameters":{"type":"object","properties":{"url":{"type":"string","description":"Target URL, e.g. http://127.0.0.1:8000/login"},"method":{"type":"string","description":"GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS","default":"GET"},"data":{"type":"object","description":"Form fields, e.g. {\"username\":\"admin' --\",\"password\":\"x\"} (sent url-encoded). May also be a raw string body."},"json_body":{"type":"object","description":"Object to send as a JSON request body instead of form data."},"params":{"type":"object","description":"Querystring parameters."},"headers":{"type":"object","description":"Request headers."}},"required":["url"]}}}
])json";

const char* const CREATE_PLAN_JSON = R"json({"type":"function","function":{"name":"create_plan","description":"Create an implementation plan before a multi-step task. Provide a numbered or bulleted list where each line is one discrete, verifiable step.","parameters":{"type":"object","properties":{"plan":{"type":"string","description":"A numbered or bulleted list of discrete steps, one per line."}},"required":["plan"]}}})json";

const char* const UPDATE_TASK_JSON = R"json({"type":"function","function":{"name":"update_task","description":"Update a plan task's status. Mark exactly one task 'doing' before working it, then 'done' when finished.","parameters":{"type":"object","properties":{"index":{"type":"string","description":"The 1-based index of the task"},"status":{"type":"string","enum":["todo","doing","done"]}},"required":["index","status"]}}})json";

constexpr int CLOUD_MAX_TOKENS = 8192;
constexpr int MAX_ROUNDS_LIMIT = 50;
constexpr int MAX_WEDGE_RECOVERIES_LIMIT = 2;

struct ChatStream {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<int, std::string>> q;
    std::atomic<bool> stop{false};

    long status = 0;
    bool network_error = false;
    bool any_chunk = false;
    std::string error;
    std::string errbody;
};

struct AnthBlock { std::string id, name, jsonbuf; };

}

bool LOREA::process_chat() {
    bool had_tool_calls = false;

    std::vector<std::string> avail = {
        "run_cmd", "read_file", "write_file", "web_search", "create_plan", "update_task",
        "test_cmd", "send_input", "read_url", "download_mlx_model", "list_files",
        "search_files", "find_files", "grep", "git_status", "git_diff", "http_request",
        "spawn_agents"};
    const std::set<std::string> read_only_tool_names = {
        "read_file", "web_search", "read_url", "list_files", "search_files",
        "find_files", "grep", "git_status", "git_diff"};
    if (tool_access == "read_only") {
        std::vector<std::string> f;
        for (const auto& n : avail) if (read_only_tool_names.count(n)) f.push_back(n);
        avail.swap(f);
    }
    if (!allow_spawn_agents) {
        avail.erase(std::remove(avail.begin(), avail.end(), std::string("spawn_agents")),
                    avail.end());
    }
    std::set<std::string> avail_set(avail.begin(), avail.end());
    auto in_avail = [&](const std::string& n) { return avail_set.count(n) != 0; };

    int tool_reprompt_count = 0;
    int invalid_tool_reprompt_count = 0;
    int thought_only_reprompt_count = 0;
    int blocked_repeat_count = 0;
    (void)blocked_repeat_count;
    bool plain_text_pushed = false;
    stall_last_sig.clear();
    stuck_last_response.clear();
    int wedge_recoveries = 0;
    const int MAX_WEDGE_RECOVERIES = MAX_WEDGE_RECOVERIES_LIMIT;
    turn_call_norms.clear();
    turn_sig_cache.clear();
    int round_count = 0;
    const int MAX_ROUNDS = MAX_ROUNDS_LIMIT;

    const std::vector<std::string> tags_to_hide = {
        "<thought>", "</thought>", "<tools>", "</tools>", "<tool_call>", "</tool_call>",
        "</function>", "</parameter>", "<response>", "</response>", "<result>", "</result>",
        "<tool_response>", "</tool_response>", "```json", "```", "Thought:", "THINKING:",
        "<|im_end|>", "<|im_start|>", "<|endoftext|>"};
    std::vector<std::string> tool_name_parts;
    for (const auto& n : avail) tool_name_parts.push_back(regex_escape(n));
    std::string tool_name_alt = join(tool_name_parts, "|");
    std::regex tool_start_re("<(?:function|parameter)\\s*=|<(?:" + tool_name_alt + ")\\s*>");
    std::regex tool_end_re("</(?:function|parameter|" + tool_name_alt + ")\\s*>");

    static const std::regex tools_wrap_re(
        "<(?:tools|tool_call|response)>([\\s\\S]*?)(?:</(?:tools|tool_call|response)>|$)");
    static const std::regex json_fence_re("```json\\s*(\\{[\\s\\S]*?\\})\\s*```");
    static const std::regex thought_re("<thought>([\\s\\S]*?)(?:</thought>|$)");
    static const std::vector<std::regex> failure_patterns = {
        std::regex("ModuleNotFoundError: No module named .+"),
        std::regex("ImportError while importing test module .+"),
        std::regex("collected 0 items / 1 error"),
        std::regex("FAILED .+"),
        std::regex("ERROR .+"),
    };

    json tools = json::parse(BASE_TOOLS_JSON);
    tools.push_back(spawn_agents_tool_schema());
    if (planning_enabled) {
        tools.push_back(json::parse(CREATE_PLAN_JSON));
        tools.push_back(json::parse(UPDATE_TASK_JSON));
    }
    if (tool_access == "read_only" || !allow_spawn_agents) {
        json filtered = json::array();
        for (auto& t : tools) {
            std::string nm;
            const json* fi = jget(t, "function");
            if (fi) nm = get_str(*fi, "name");
            if (in_avail(nm)) filtered.push_back(t);
        }
        tools.swap(filtered);
    }

    const bool mpc_active = mpc_url.has_value() && !mpc_url->empty();
    const std::vector<std::string> hardware_queries = {
        "vram", "gpu memory", "video memory", "unified memory", "how much memory"};
    auto goal_has = [&](const std::vector<std::string>& terms) {
        std::string g = lower_ascii(last_user_goal);
        for (const auto& t : terms) if (contains(g, t)) return true;
        return false;
    };

    while (true) {
        round_count += 1;
        if (round_count > MAX_ROUNDS) {
            log_warn("Reached the " + std::to_string(MAX_ROUNDS) +
                     "-round limit for one turn; stopping to avoid an endless loop.");
            json am = json::object();
            am["role"] = "assistant";
            am["content"] = "[LOREA LOOP GUARD] Stopped after " + std::to_string(MAX_ROUNDS) +
                            " model rounds in one turn. Give the user a concise status of what "
                            "was done and what remains.";
            messages.push_back(am);
            interrupter.stop_listening();
            return false;
        }
        std::unique_ptr<Spinner> spinner;
        try {

            compact_history();
            interrupter.start_listening();

            if ((backend == "mlx" || backend == "llama-cpp") && !mpc_active) {
                ensure_local_server();
            }

            spinner = std::make_unique<Spinner>(
                (mpc_active ? std::string("MPC") : capitalize(backend)) + " generating");
            spinner->start();

            std::string content;
            std::vector<json> tool_calls;
            json response_metadata = json::object();
            bool first_chunk = true;
            bool in_thought = false, in_tool = false, thought_labeled = false;
            std::string line_buffer;
            bool displayed_any = false;

            auto process_token = [&](const std::string& token) {
                content += token;
                if (first_chunk && !strip(token).empty()) {
                    spinner->stop();
                    std::cout << "\n" << left_indent() << ACCENT << Colors::BOLD << "\xE2\x9D\xAF LOREA"
                              << Colors::RESET << " " << Colors::DIM << Colors::GRAY << "\xC2\xB7"
                              << Colors::RESET << " " << std::flush;
                    first_chunk = false;
                }
                std::string recent = last_cp(content, utf8_len(token) + 24);
                if (!in_tool && std::regex_search(recent, tool_start_re)) in_tool = true;
                if (in_tool && std::regex_search(recent, tool_end_re)) in_tool = false;

                std::string content_before = drop_last_cp(content, utf8_len(token));
                for (const auto& tag : tags_to_hide) {
                    if (contains(content, tag) && !contains(content_before, tag)) {
                        if (startswith(tag, "</") || tag == "```") {
                            if (in_thought && tag == "</thought>") in_thought = false;
                            if (in_tool) in_tool = false;
                        } else {
                            if (tag == "<thought>" || tag == "Thought:" || tag == "THINKING:") {
                                in_thought = true;
                                if (!thought_labeled) {
                                    std::cout << Colors::ORANGE << Colors::BOLD << "thinking"
                                              << Colors::RESET << std::flush;
                                    thought_labeled = true;
                                }
                            } else {
                                in_tool = true;
                            }
                        }
                    }
                }
                if (!in_thought && !in_tool) {
                    line_buffer += token;
                    for (const auto& tag : tags_to_hide) line_buffer = replace_all(line_buffer, tag, "");
                    bool is_potential_tag = false;
                    for (const auto& tag : tags_to_hide) {
                        for (std::size_t i = 1; i < tag.size(); ++i) {
                            if (endswith(line_buffer, tag.substr(0, i))) { is_potential_tag = true; break; }
                        }
                        if (is_potential_tag) break;
                    }
                    if (!is_potential_tag) is_potential_tag = endswith(rstrip_ws(line_buffer), "<");
                    if (!is_potential_tag && contains(line_buffer, "\n")) {
                        std::vector<std::string> parts = split_char(line_buffer, '\n');
                        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
                            std::cout << render_text(parts[i] + "\n") << std::flush;
                            if (!strip(parts[i]).empty()) displayed_any = true;
                        }
                        line_buffer = parts.back();
                    }
                }
            };

            if (mpc_active) {
                int mpc_attempt = 0;
                while (true) {
                    try {
                        if (mpc_supports("streaming")) {
                            auto res = mpc_chat_stream(
                                arr_of(server_messages()), tools,
                                [&](const std::string& t) { process_token(t); },
                                [] { return interrupter.interrupted.is_set(); });
                            response_metadata = std::get<2>(res);
                            for (auto& tc : std::get<1>(res)) tool_calls.push_back(tc);
                        } else {
                            auto res = mpc_chat(arr_of(server_messages()), tools);
                            response_metadata = std::get<2>(res);
                            const std::string& remote_content = std::get<0>(res);
                            if (!remote_content.empty()) process_token(remote_content);
                            for (auto& tc : std::get<1>(res)) tool_calls.push_back(tc);
                        }
                        break;
                    } catch (const MPCRetryable& e) {
                        mpc_attempt += 1;
                        if (mpc_attempt > MPC_CHAT_MAX_RETRIES) {
                            spinner->stop();
                            interrupter.stop_listening();
                            log_info("MPC stream failed after " + std::to_string(MPC_CHAT_MAX_RETRIES) +
                                     " retries: " + e.what());
                            json am = json::object();
                            am["role"] = "assistant";
                            am["content"] = std::string("[MPC ERROR] ") + e.what();
                            messages.push_back(am);
                            return false;
                        }
                        content.clear();
                        tool_calls.clear();
                        first_chunk = true; in_thought = false; in_tool = false;
                        thought_labeled = false; line_buffer.clear();
                        log_info("MPC stream dropped; retrying (" + std::to_string(mpc_attempt) + "/" +
                                 std::to_string(MPC_CHAT_MAX_RETRIES) + ")\xE2\x80\xA6");
                        std::this_thread::sleep_for(
                            std::chrono::duration<double>(0.5 * mpc_attempt));
                        spinner->start();
                        continue;
                    } catch (const std::exception& e) {
                        if (std::string(e.what()) == "KeyboardInterrupt") throw;
                        spinner->stop();
                        interrupter.stop_listening();
                        log_info(std::string("MPC request failed: ") + e.what());
                        json am = json::object();
                        am["role"] = "assistant";
                        am["content"] = std::string("[MPC ERROR] ") + e.what();
                        messages.push_back(am);
                        return false;
                    }
                }
            } else if (backend == "airllm") {

                spinner->stop();
                interrupter.stop_listening();
                log_info(std::string("AirLLM or PyTorch is not installed. Please run: ") +
                         Colors::TEAL + "pip install airllm torch" + Colors::RESET);
                json am = json::object();
                am["role"] = "assistant";
                am["content"] = "[AirLLM NOT INSTALLED]";
                messages.push_back(am);
                return false;
            } else if (backend == "nvidia") {

                std::string script = find_nvidia_backend();
                if (script.empty()) {
                    spinner->stop();
                    interrupter.stop_listening();
                    log_info("nvidia_backend.py not found. Put it next to the ocli binary or in "
                             "~/ocli-cpp/, then retry.");
                    json am = json::object();
                    am["role"] = "assistant";
                    am["content"] = "[NVIDIA BRIDGE MISSING]";
                    messages.push_back(am);
                    return false;
                }

                json payload = json::object();
                payload["model"] = model_name;
                payload["messages"] = arr_of(server_messages());
                payload["temperature"] = 1;
                payload["top_p"] = 1;
                payload["max_tokens"] = 16384;
                if (auto nkey = api_key("nvidia", false); nkey && !nkey->empty())
                    payload["api_key"] = *nkey;

                char tmpl[] = "/tmp/ocli_nvidia_XXXXXX";
                int tfd = ::mkstemp(tmpl);
                std::string tmp = tmpl;
                if (tfd >= 0) {
                    std::string bodytxt = payload.dump();
                    ssize_t wn = ::write(tfd, bodytxt.data(), bodytxt.size());
                    (void)wn;
                    ::close(tfd);
                }

                const char* pyenv = std::getenv("PYTHON");
                std::string python = (pyenv && *pyenv) ? std::string(pyenv) : std::string("python3");
                std::string cmd = python + " " + shlex_quote(script) + " " + shlex_quote(tmp) +
                                  " 2>/dev/null";
                FILE* pf = ::popen(cmd.c_str(), "r");
                if (!pf) {
                    spinner->stop();
                    interrupter.stop_listening();
                    if (!tmp.empty()) std::remove(tmp.c_str());
                    log_info("Could not launch the NVIDIA Python bridge (is python3 installed?).");
                    json am = json::object();
                    am["role"] = "assistant";
                    am["content"] = "[NVIDIA BRIDGE FAILED]";
                    messages.push_back(am);
                    return false;
                }
                int pfd = ::fileno(pf);
                char rbuf[4096];
                ssize_t got;
                while ((got = ::read(pfd, rbuf, sizeof(rbuf))) != 0) {
                    if (got < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    process_token(std::string(rbuf, static_cast<std::size_t>(got)));
                    if (interrupter.interrupted.is_set()) break;
                }
                ::pclose(pf);
                if (!tmp.empty()) std::remove(tmp.c_str());
                response_metadata = json::object();

            } else {

                HttpRequest req;
                req.method = "POST";
                req.follow_redirects = true;

                if (backend == "ollama") {
                    json ollama_messages = json::array();
                    for (auto& m : messages) ollama_messages.push_back(m);
                    for (auto& m : ephemeral_reminders()) ollama_messages.push_back(m);
                    json body = json::object();
                    body["model"] = model_name;
                    body["messages"] = ollama_messages;
                    body["stream"] = true;
                    body["tools"] = tools;
                    std::string base;
                    const char* oh = std::getenv("OLLAMA_HOST");
                    if (oh && *oh) {
                        base = oh;
                        if (!startswith(base, "http://") && !startswith(base, "https://"))
                            base = "http://" + base;
                    } else {
                        base = url;
                    }
                    while (!base.empty() && base.back() == '/') base.pop_back();
                    req.url = base + "/api/chat";
                    req.headers = {{"Content-Type", "application/json"}};
                    req.body = body.dump();
                    req.timeout_ms = 0;
                } else if (backend == "anthropic") {
                    auto headers_opt = anthropic_auth_headers(true);
                    if (!headers_opt) {
                        spinner->stop();
                        interrupter.stop_listening();
                        log_info("No Anthropic credential set. Set ANTHROPIC_API_KEY (or "
                                 "ANTHROPIC_AUTH_TOKEN for a gateway) or run /backend.");
                        json am = json::object();
                        am["role"] = "assistant";
                        am["content"] = "[NO API KEY]";
                        messages.push_back(am);
                        return false;
                    }
                    auto am_pair = anthropic_messages();
                    const std::string& system_text = am_pair.first;
                    json payload = json::object();
                    payload["model"] = model_name;
                    payload["max_tokens"] = CLOUD_MAX_TOKENS;
                    payload["messages"] = am_pair.second;
                    payload["stream"] = true;
                    if (!strip(system_text).empty()) payload["system"] = system_text;
                    json anthropic_tools = to_anthropic_tools(tools);
                    if (!anthropic_tools.empty()) payload["tools"] = anthropic_tools;
                    req.url = cloud_base() + "/v1/messages";
                    for (const auto& kv : *headers_opt) req.headers.emplace_back(kv.first, kv.second);
                    req.body = payload.dump();
                    req.timeout_ms = 300000;
                } else {
                    json payload = json::object();
                    payload["model"] = model_name;
                    payload["messages"] = arr_of(server_messages());
                    payload["stream"] = true;
                    if (backend == "mlx" || backend == "llama-cpp" || backend == "server") {
                        payload["temperature"] = 0.5;
                        payload["top_p"] = 0.95;
                        payload["repetition_penalty"] = 1.1;
                    }
                    req.headers = {{"Content-Type", "application/json"}};
                    if (backend == "openai") {
                        auto key = api_key("openai", true);
                        if (!key || key->empty()) {
                            spinner->stop();
                            interrupter.stop_listening();
                            log_info("No OpenAI API key set. Set OPENAI_API_KEY or run /backend.");
                            json am = json::object();
                            am["role"] = "assistant";
                            am["content"] = "[NO API KEY]";
                            messages.push_back(am);
                            return false;
                        }
                        req.headers.emplace_back("Authorization", "Bearer " + *key);
                        payload["tools"] = tools;
                    }
                    req.url = cloud_base() + "/v1/chat/completions";
                    req.body = payload.dump();
                    req.timeout_ms = 300000;
                }

                if (backend == "anthropic") {
                    spinner->stop();
                    log_info("Claude " + model_name + " \xE2\x80\x94 awaiting response...");
                    spinner->start();
                } else if (backend != "ollama") {
                    spinner->stop();
                    log_info("Connected to " + backend + " " + model_name + ". Awaiting response...");
                    spinner->start();
                }

                auto cs = std::make_shared<ChatStream>();
                std::thread worker([cs, req]() {
                    std::string buf, errbuf;
                    bool got = false;
                    auto on_chunk = [&](const char* data, std::size_t n) -> bool {
                        if (cs->stop.load()) return false;
                        got = true;
                        if (errbuf.size() < 8192)
                            errbuf.append(data, std::min(n, (std::size_t)8192 - errbuf.size()));
                        buf.append(data, n);
                        std::size_t pos;
                        while ((pos = buf.find('\n')) != std::string::npos) {
                            std::string line = buf.substr(0, pos);
                            buf.erase(0, pos + 1);
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            {
                                std::lock_guard<std::mutex> lk(cs->m);
                                cs->q.emplace_back(0, line);
                            }
                            cs->cv.notify_one();
                            if (cs->stop.load()) return false;
                        }
                        return true;
                    };
                    HttpResponse resp = http_stream(req, on_chunk);
                    {
                        std::lock_guard<std::mutex> lk(cs->m);
                        cs->status = resp.status;
                        cs->network_error = resp.network_error;
                        cs->error = resp.error;
                        cs->any_chunk = got;
                        cs->errbody = errbuf;
                        if (!buf.empty()) {
                            std::string line = buf;
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            cs->q.emplace_back(0, line);
                        }
                        cs->q.emplace_back(1, std::string());
                    }
                    cs->cv.notify_one();
                });

                std::map<long, AnthBlock> anthropic_tool_blocks;
                std::optional<std::string> stream_error;
                std::optional<std::string> connect_error;
                bool ended_done = false;

                while (true) {
                    if (interrupter.interrupted.is_set()) { cs->stop.store(true); break; }
                    std::pair<int, std::string> ev;
                    {
                        std::unique_lock<std::mutex> lk(cs->m);
                        if (cs->q.empty()) cs->cv.wait_for(lk, std::chrono::milliseconds(30));
                        if (cs->q.empty()) continue;
                        ev = cs->q.front();
                        cs->q.pop_front();
                    }
                    if (ev.first == 1) { ended_done = true; break; }
                    const std::string& line = ev.second;

                    if (backend == "ollama") {
                        auto item = try_parse(line);
                        if (!item) continue;
                        const json* msg = jget(*item, "message");
                        if (msg && msg->is_object()) {
                            const json* c = jget(*msg, "content");
                            if (c) process_token(c->is_string() ? c->get<std::string>() : py_str(*c));
                            const json* tcs = jget(*msg, "tool_calls");
                            if (tcs && jtruthy(*tcs) && tcs->is_array())
                                for (auto& tc : *tcs) tool_calls.push_back(normalize_tool_call(tc));
                        }
                        if (item->is_object() && item->contains("total_duration")) response_metadata = *item;
                    } else if (backend == "anthropic") {
                        if (line.empty()) continue;
                        if (!startswith(line, "data:")) continue;
                        auto data_opt = try_parse(strip(line.substr(5)));
                        if (!data_opt) continue;
                        json& data = *data_opt;
                        std::string etype = get_str(data, "type");
                        if (etype == "content_block_start") {
                            json cb = jval(data, "content_block");
                            if (!cb.is_object()) cb = json::object();
                            if (get_str(cb, "type") == "tool_use") {
                                long idx = jget(data, "index") && data["index"].is_number()
                                               ? data["index"].get<long>() : -1;
                                AnthBlock b;
                                b.id = get_str(cb, "id");
                                b.name = get_str(cb, "name");
                                b.jsonbuf = "";
                                anthropic_tool_blocks[idx] = b;
                            }
                        } else if (etype == "content_block_delta") {
                            json d = jval(data, "delta");
                            if (!d.is_object()) d = json::object();
                            std::string dt = get_str(d, "type");
                            if (dt == "text_delta") {
                                const json* tx = jget(d, "text");
                                if (tx && jtruthy(*tx))
                                    process_token(tx->is_string() ? tx->get<std::string>() : py_str(*tx));
                            } else if (dt == "input_json_delta") {
                                long idx = jget(data, "index") && data["index"].is_number()
                                               ? data["index"].get<long>() : -1;
                                auto it = anthropic_tool_blocks.find(idx);
                                if (it != anthropic_tool_blocks.end()) {
                                    const json* pj = jget(d, "partial_json");
                                    if (pj && pj->is_string()) it->second.jsonbuf += pj->get<std::string>();
                                }
                            }
                        } else if (etype == "content_block_stop") {
                            long idx = jget(data, "index") && data["index"].is_number()
                                           ? data["index"].get<long>() : -1;
                            auto it = anthropic_tool_blocks.find(idx);
                            if (it != anthropic_tool_blocks.end()) {
                                AnthBlock blk = it->second;
                                anthropic_tool_blocks.erase(it);
                                if (!blk.name.empty()) {
                                    json args = json::object();
                                    if (!strip(blk.jsonbuf).empty()) {
                                        auto p = try_parse(blk.jsonbuf);
                                        args = p ? *p : json::object();
                                    }
                                    json tc = json::object();
                                    tc["id"] = !blk.id.empty() ? blk.id
                                                               : ("call_" + std::to_string(now_unix()));
                                    tc["type"] = "function";
                                    json fn = json::object();
                                    fn["name"] = blk.name;
                                    fn["arguments"] = args;
                                    tc["function"] = fn;
                                    tool_calls.push_back(tc);
                                }
                            }
                        } else if (etype == "message_delta") {
                            response_metadata = data;
                        } else if (etype == "error") {
                            json err = jval(data, "error");
                            std::string msg = (err.is_object() ? get_str(err, "message") : "");
                            stream_error = !msg.empty() ? msg : std::string("Anthropic stream error");
                            break;
                        } else if (etype == "message_stop") {
                            cs->stop.store(true);
                            break;
                        }
                    } else {
                        if (line.empty()) continue;
                        if (startswith(line, "data: ")) {
                            if (strip(line) == "data: [DONE]") { cs->stop.store(true); break; }
                            auto data_opt = try_parse(line.substr(6));
                            if (!data_opt) continue;
                            json& data = *data_opt;
                            if (!data.contains("choices") || !data["choices"].is_array() ||
                                data["choices"].empty())
                                continue;
                            json delta = jval(data["choices"][0], "delta");
                            if (!delta.is_object()) delta = json::object();
                            const json* c = jget(delta, "content");
                            if (c && jtruthy(*c))
                                process_token(c->is_string() ? c->get<std::string>() : py_str(*c));
                            const json* tcs = jget(delta, "tool_calls");
                            if (tcs && tcs->is_array()) {
                                for (auto& tc : *tcs) {
                                    long idx = jget(tc, "index") && tc["index"].is_number()
                                                   ? tc["index"].get<long>() : 0;
                                    while ((long)tool_calls.size() <= idx) {
                                        json blank = json::object();
                                        blank["id"] = "";
                                        blank["type"] = "function";
                                        json fn = json::object();
                                        fn["name"] = "";
                                        fn["arguments"] = "";
                                        blank["function"] = fn;
                                        tool_calls.push_back(blank);
                                    }
                                    if (tc.contains("id")) {
                                        std::string add = tc["id"].is_string()
                                                              ? tc["id"].get<std::string>() : py_str(tc["id"]);
                                        tool_calls[idx]["id"] =
                                            tool_calls[idx]["id"].get<std::string>() + add;
                                    }
                                    if (tc.contains("function")) {
                                        const json& tfn = tc["function"];
                                        if (tfn.contains("name")) {
                                            std::string add = tfn["name"].is_string()
                                                                  ? tfn["name"].get<std::string>()
                                                                  : py_str(tfn["name"]);
                                            tool_calls[idx]["function"]["name"] =
                                                tool_calls[idx]["function"]["name"].get<std::string>() + add;
                                        }
                                        if (tfn.contains("arguments")) {
                                            std::string add = tfn["arguments"].is_string()
                                                                  ? tfn["arguments"].get<std::string>()
                                                                  : py_str(tfn["arguments"]);
                                            tool_calls[idx]["function"]["arguments"] =
                                                tool_calls[idx]["function"]["arguments"].get<std::string>() + add;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (ended_done) { if (worker.joinable()) worker.join(); }
                else { if (worker.joinable()) worker.detach(); }

                if (ended_done) {
                    if (cs->network_error) {
                        if (backend != "ollama" && !cs->any_chunk) connect_error = cs->error;
                        else stream_error = cs->error;
                    } else if (cs->status != 200) {
                        if (backend == "ollama")
                            stream_error = "HTTP " + std::to_string(cs->status) + ": " +
                                           utf8_substr(cs->errbody, 0, 400);

                    }
                }

                if (connect_error) {
                    spinner->stop();
                    interrupter.stop_listening();
                    json am = json::object();
                    am["role"] = "assistant";
                    if (backend == "anthropic") {
                        log_info("Anthropic request failed: " + *connect_error);
                        am["content"] = "[API ERROR] " + *connect_error;
                    } else {
                        log_info("Server request failed: " + *connect_error);
                        auto note = server_crash_note(std::runtime_error(*connect_error));
                        am["content"] = note ? *note : ("[SERVER ERROR] " + *connect_error);
                    }
                    messages.push_back(am);
                    return false;
                }

                if (ended_done && backend != "ollama" && !cs->network_error && cs->status != 200) {
                    std::string txt = utf8_substr(cs->errbody, 0, 400);
                    spinner->stop();
                    interrupter.stop_listening();
                    json am = json::object();
                    am["role"] = "assistant";
                    if (backend == "anthropic") {
                        log_info("Anthropic API error (" + std::to_string(cs->status) + "): " + txt);
                        am["content"] = "[API ERROR " + std::to_string(cs->status) + "] " + txt;
                    } else {
                        log_info("Server Error (" + std::to_string(cs->status) + "): " + txt);
                        am["content"] = "[SERVER ERROR " + std::to_string(cs->status) + "] " + txt;
                    }
                    messages.push_back(am);
                    return false;
                }

                if (stream_error) {
                    spinner->stop();
                    interrupter.stop_listening();
                    log_info("Generation failed: " + *stream_error);
                    auto note = server_crash_note(std::runtime_error(*stream_error));
                    json am = json::object();
                    am["role"] = "assistant";
                    am["content"] = note ? *note : ("[STREAM ERROR] " + *stream_error);
                    messages.push_back(am);
                    return false;
                }
            }

            spinner->stop();
            if (interrupter.interrupted.is_set()) {
                interrupter.stop_listening();
                if (!line_buffer.empty() && !in_thought && !in_tool)
                    std::cout << render_text(line_buffer) << std::flush;
                std::cout << "\n" << left_indent() << Colors::YELLOW << Colors::BOLD << "\xE2\x96\xA0"
                          << Colors::RESET << " " << Colors::DIM << Colors::GRAY << "stopped"
                          << Colors::RESET << "\n";
                json am = json::object();
                am["role"] = "assistant";
                am["content"] = strip(content) + " [USER INTERRUPTED]";
                messages.push_back(am);
                return false;
            }
            if (!line_buffer.empty() && !in_thought && !in_tool) {
                std::cout << render_text(line_buffer) << std::flush;
                if (!strip(line_buffer).empty()) displayed_any = true;
            }
            std::cout << "\n";

            if (tool_calls.empty()) {
                auto xml_calls = parse_xml_tool_calls(content);
                for (auto& c : xml_calls) tool_calls.push_back(c);
            }
            if (tool_calls.empty()) {
                auto named_calls = parse_named_xml_tool_calls(content, &avail);
                for (auto& c : named_calls) tool_calls.push_back(c);
            }
            if (tool_calls.empty()) {
                std::vector<std::string> tool_matches = findall_group1(tools_wrap_re, content);
                if (tool_matches.empty()) tool_matches = findall_group1(json_fence_re, content);
                if (tool_matches.empty()) {
                    for (const auto& obj : extract_json_objects(content)) {
                        if (contains(obj, "\"name\"") && contains(obj, "\"arguments\""))
                            tool_matches.push_back(replace_all(replace_all(obj, "{{", "{"), "}}", "}"));
                    }
                }
                for (const auto& match : tool_matches) {
                    std::string snippet = strip(match);
                    if (snippet.empty()) continue;
                    bool parsed_ok = false;
                    std::string parse_err;
                    json data;
                    try { data = json::parse(snippet); parsed_ok = true; }
                    catch (const std::exception& e) { parse_err = e.what(); }
                    if (parsed_ok) {
                        if (data.is_array()) {
                            for (auto& item : data) {
                                if (item.is_object() && jtruthy(jval(item, "name"))) {
                                    json tc = json::object();
                                    json fn = json::object();
                                    fn["name"] = jval(item, "name");
                                    fn["arguments"] = item.contains("arguments") ? item["arguments"]
                                                                                 : json::object();
                                    tc["function"] = fn;
                                    tool_calls.push_back(tc);
                                }
                            }
                        } else if (data.is_object() && jtruthy(jval(data, "name"))) {
                            json tc = json::object();
                            json fn = json::object();
                            fn["name"] = jval(data, "name");
                            fn["arguments"] = data.contains("arguments") ? data["arguments"]
                                                                         : json::object();
                            tc["function"] = fn;
                            tool_calls.push_back(tc);
                        }
                    } else {
                        auto xmlc = parse_xml_tool_calls(snippet);
                        if (!xmlc.empty()) {
                            log_info("Recovered tool call from non-JSON model output");
                            for (auto& c : xmlc) tool_calls.push_back(c);
                        } else {
                            auto rec = recover_tool_call_from_text(snippet);
                            if (rec && !rec->is_null()) {
                                log_info("Recovered tool call from non-JSON model output");
                                tool_calls.push_back(*rec);
                            } else {
                                log_info("Failed to parse tool call: " + parse_err);
                            }
                        }
                    }
                }

                if (tool_calls.empty()) {
                    bool looks_callish = contains(content, "<tool");
                    if (!looks_callish && contains(content, "{")) {
                        looks_callish = contains(content, "\"name\"") || contains(content, "'name'") ||
                                        contains(content, "\"arguments\"") || contains(content, "'arguments'");
                    }
                    if (!looks_callish && contains(content, "(")) {
                        for (const auto& t : KNOWN_TOOL_NAMES) {
                            std::regex r("\\b" + regex_escape(t) + "\\s*\\(");
                            if (std::regex_search(content, r)) { looks_callish = true; break; }
                        }
                    }
                    std::optional<json> recovered;
                    if (looks_callish) recovered = recover_tool_call_from_text(content);
                    if (recovered && !recovered->is_null()) {
                        log_info("Recovered malformed tool call from full model output");
                        tool_calls.push_back(*recovered);
                    }
                }
            }

            if (tool_calls.empty() && !displayed_any) {
                std::string answer_text = visible_answer(content);
                if (!answer_text.empty()) {
                    std::cout << render_text(answer_text) << "\n";
                    displayed_any = true;
                }
            }

            if (tool_calls.empty() && strip(strip_tool_markup(content)).empty()) {
                std::string g = lower_ascii(last_user_goal);
                bool wants_result = contains(g, "result") || contains(g, "output") ||
                                    contains(g, "what happened") || strip(last_user_goal) == "?";
                std::string latest_tool_result = wants_result ? latest_tool_result_text() : "";
                if (!latest_tool_result.empty()) {
                    content = latest_tool_result;
                    std::cout << render_text(content) << "\n";
                }
            }

            if (tool_calls.empty() && implies_web_search(content)) {
                int prior_searches = 0;
                for (auto& m : messages)
                    if (get_str(m, "role") == "tool" && get_str(m, "name") == "web_search")
                        prior_searches += 1;
                if (prior_searches >= 4) {
                    log_info("Search budget reached; asking the model to synthesize the gathered results.");
                    json am = json::object();
                    am["role"] = "assistant";
                    am["content"] = content;
                    messages.push_back(am);
                    json um = json::object();
                    um["role"] = "user";
                    um["content"] = "You have enough search results. Stop searching. Write a structured "
                                    "deep-research answer using the gathered source results, include caveats "
                                    "where sources are weak, and separate facts from analysis.";
                    messages.push_back(um);
                    continue;
                }
                std::string query = infer_web_search_query(content, last_user_goal, prior_searches);
                if (!query.empty()) {
                    log_info(std::string("Recovered implied web_search: ") + Colors::TEAL + query +
                             Colors::RESET);
                    json tc = json::object();
                    json fn = json::object();
                    fn["name"] = "web_search";
                    json args = json::object();
                    args["query"] = query;
                    args["num_results"] = 20;
                    fn["arguments"] = args;
                    tc["function"] = fn;
                    tool_calls.push_back(tc);
                }
            }

            std::string _vis = visible_answer(content);
            std::string _vsig = lower_ascii(join_ws(_vis));
            if (tool_calls.empty() && !_vsig.empty() && utf8_len(_vsig) >= 40) {
                int _ndup = 0;
                for (const auto& s : recent_answer_sigs)
                    if (difflib_ratio(_vsig, s) >= 0.85) _ndup += 1;
                recent_answer_sigs.push_back(_vsig);
                while (recent_answer_sigs.size() > 6) recent_answer_sigs.pop_front();
                if (_ndup >= 2) {
                    int _pruned = prune_repeated_assistant(_vsig);
                    recent_answer_sigs.clear();
                    bool _local = (backend == "mlx" || backend == "llama-cpp") && !mpc_active;
                    if (_local && wedge_recoveries < MAX_WEDGE_RECOVERIES) {
                        wedge_recoveries += 1;
                        log_warn("Model wedged (repeated one answer, ignoring input); pruned " +
                                 std::to_string(_pruned) + " duplicate message(s). Auto-recovering " +
                                 std::to_string(wedge_recoveries) + "/" +
                                 std::to_string(MAX_WEDGE_RECOVERIES) +
                                 ": save session -> restart server -> reload -> retry.");
                        std::cout << left_indent() << Colors::YELLOW << Colors::BOLD << "!"
                                  << Colors::RESET << " " << Colors::DIM << Colors::GRAY
                                  << "Model wedged. Saving the session, restarting the inference server, "
                                     "and reloading it \xE2\x80\x94 this clears the server-side cache behind "
                                     "the wedge. Retrying automatically..."
                                  << Colors::RESET << "\n";
                        interrupter.stop_listening();
                        save_restart_reload();
                        continue;
                    }
                    log_warn("Model stayed wedged after auto-recovery; removed " + std::to_string(_pruned) +
                             " duplicated message(s) poisoning its context and reset inference.");
                    std::cout << left_indent() << Colors::YELLOW << Colors::BOLD << "!" << Colors::RESET
                              << " " << Colors::DIM << Colors::GRAY
                              << "The model stayed wedged after auto-recovery (save/restart/reload). "
                                 "Cleared the repeated text and reset inference. Run /clear to fully reset, "
                                 "and try /effort basic \xE2\x80\x94 GO BEYOND makes a weak model over-narrate "
                                 "and lock up."
                              << Colors::RESET << "\n";
                    json am = json::object();
                    am["role"] = "assistant";
                    am["content"] = "[LOREA LOOP GUARD] I stayed wedged even after auto-recovery (save "
                                    "session, restart server, reload), so I stopped. Send your message again, "
                                    "or /clear to reset.";
                    messages.push_back(am);
                    interrupter.stop_listening();
                    restart_inference_server();
                    break;
                }
            }

            std::string lower_content = lower_ascii(content);
            std::string user_context;
            {
                std::vector<std::string> ucparts;
                std::size_t start = messages.size() >= 3 ? messages.size() - 3 : 0;
                for (std::size_t i = start; i < messages.size(); ++i)
                    if (get_str(messages[i], "role") == "user")
                        ucparts.push_back(str_or_empty(jval(messages[i], "content")));
                user_context = lower_ascii(join(ucparts, "\n"));
            }
            const std::vector<std::string> fake_tool_markers = {
                "<tool_response>", "</tool_response>", "running tasklite.py...", "diff --git"};
            const std::vector<std::string> tool_required_phrases = {
                "create ", "write ", "edit ", "modify ", "fix ", "test ", "run ", "execute ",
                "search ", "research ", "look up", "find information", "gather information",
                "pytest", "tasklite.py", "devlog.py", "test_devlog.py", "readme.md"};
            bool needs_real_tool = false;
            if (!had_tool_calls)
                for (const auto& p : tool_required_phrases)
                    if (contains(user_context, p)) { needs_real_tool = true; break; }
            bool appears_fake_tool_result = false;
            if (!had_tool_calls)
                for (const auto& mk : fake_tool_markers)
                    if (contains(lower_content, mk)) { appears_fake_tool_result = true; break; }
            bool bare_continue = upper_ascii(strip(content)) == "CONTINUE";
            auto direct_command = direct_command_from_user(last_user_goal);

            if (tool_calls.empty() && !had_tool_calls && direct_command && !direct_command->empty()) {
                log_info(std::string("Model did not emit a valid tool call; using recovered command: ") +
                         Colors::TEAL + *direct_command + Colors::RESET);
                json tc = json::object();
                json fn = json::object();
                fn["name"] = "run_cmd";
                json args = json::object();
                args["command"] = *direct_command;
                fn["arguments"] = args;
                tc["function"] = fn;
                tool_calls.push_back(tc);
            } else if (tool_calls.empty() && !had_tool_calls &&
                       [&] {
                           for (const auto& t : hardware_queries)
                               if (contains(user_context, t)) return true;
                           return false;
                       }()) {

                log_info("Using deterministic hardware inspection command");
                json tc = json::object();
                json fn = json::object();
                fn["name"] = "run_cmd";
                json args = json::object();
                args["command"] = SYSPROF;
                fn["arguments"] = args;
                tc["function"] = fn;
                tool_calls.push_back(tc);
            } else if (tool_calls.empty() && !plain_text_pushed &&
                       (needs_real_tool || appears_fake_tool_result || bare_continue)) {
                tool_reprompt_count += 1;
                json am = json::object();
                am["role"] = "assistant";
                am["content"] = content;
                messages.push_back(am);
                std::string decision = stall_decision(content, tool_reprompt_count);
                if (decision == "stop") {
                    log_warn("Model is repeating the same step without emitting a runnable tool call; ending turn.");
                    std::cout << left_indent() << Colors::YELLOW << Colors::BOLD << "!" << Colors::RESET
                              << " " << Colors::DIM << Colors::GRAY
                              << "Stuck restating one step without running it \xE2\x80\x94 ended the turn. Tip: "
                                 "give it the exact run_cmd to use, or lower /effort. For a payload it must "
                                 "GENERATE (many bytes, fuzzing), a shell one-liner like  run_cmd  python3 -c "
                                 "'print(\"A\"*100000)'  is the right tool \xE2\x80\x94 don't make it type the literal."
                              << Colors::RESET << "\n";
                    json gm = json::object();
                    gm["role"] = "assistant";
                    gm["content"] = "[LOREA LOOP GUARD] I kept restating the same step without turning it into "
                                    "a runnable command, so I stopped instead of looping forever. The action I "
                                    "described could not be expressed as a single literal tool argument. To "
                                    "proceed: give me the exact command to run, or have me use run_cmd with a "
                                    "shell/python one-liner that GENERATES the payload (for example  python3 -c "
                                    "'print(\"A\"*100000)' ), rather than typing it out.";
                    messages.push_back(gm);
                    break;
                }
                if (decision == "finalize") {
                    plain_text_pushed = true;
                    log_info("No valid tool call after repeated reminders; asking once for a plain-text answer.");
                    json um = json::object();
                    um["role"] = "user";
                    um["content"] = "You still have not produced a valid tool call after several reminders. "
                                    "Stop trying to call tools and, using the information already above, write "
                                    "your complete answer to the original request now in plain natural language:\n" +
                                    last_user_goal;
                    messages.push_back(um);
                    continue;
                }
                log_info("Invalid/again tool call (attempt " + std::to_string(tool_reprompt_count) +
                         "); re-injecting the tool-call format reminder.");
                json um = json::object();
                um["role"] = "user";
                um["content"] = "That was not a valid tool call. Continue the user's original task by emitting "
                                "exactly ONE real tool call in the correct format \xE2\x80\x94 do not output a "
                                "bare CONTINUE, prose-as-output, or simulated results. For data you must GENERATE "
                                "or repeat (a long string, many requests, fuzzing input), do NOT type the literal "
                                "\xE2\x80\x94 use run_cmd with a shell/python one-liner such as python3 -c "
                                "'print(\"A\"*100000)'. " + tool_reminder_text();
                messages.push_back(um);
                continue;
            }

            interrupter.stop_listening();
            if (interrupter.interrupted.is_set()) {
                json am = json::object();
                am["role"] = "assistant";
                am["content"] = content + " [USER INTERRUPTED]";
                messages.push_back(am);
                break;
            }

            if (!tool_calls.empty()) {
                json formatted_calls = json::array();
                for (auto& tc : tool_calls) {
                    json function_info = (tc.is_object() && jget(tc, "function"))
                                             ? *jget(tc, "function") : json::object();
                    std::string name = get_str(function_info, "name");
                    json args = function_info.is_object() && function_info.contains("arguments")
                                    ? function_info["arguments"] : json::object();
                    if (args.is_string()) {
                        std::string a = args.get<std::string>();
                        if (!strip(a).empty()) { auto p = try_parse(a); args = p ? *p : json::object(); }
                        else args = json::object();
                    }
                    if (!args.is_object()) args = json::object();
                    if (!name.empty()) {
                        json fc = json::object();
                        fc["id"] = (tc.is_object() && tc.contains("id"))
                                       ? get_str(tc, "id") : ("call_" + std::to_string(now_unix()));
                        fc["type"] = "function";
                        json fn = json::object();
                        fn["name"] = name;
                        fn["arguments"] = args;
                        fc["function"] = fn;
                        formatted_calls.push_back(fc);
                    }
                }
                json am = json::object();
                am["role"] = "assistant";
                if (!formatted_calls.empty()) {
                    am["content"] = strip_tool_markup(content);
                    am["tool_calls"] = formatted_calls;
                } else {
                    am["content"] = content;
                }
                messages.push_back(am);
            } else {
                json am = json::object();
                am["role"] = "assistant";
                am["content"] = content;
                messages.push_back(am);
            }

            {
                std::vector<std::string> thoughts = findall_group1(thought_re, content);
                if (!thoughts.empty() && !strip(thoughts[0]).empty()) {
                    std::cout << "\n" << status_label("THINKING", Colors::ORANGE) << "\n";
                    for (const auto& t : thoughts)
                        if (!strip(t).empty())
                            std::cout << Colors::GRAY << "  > " << render_text(strip(t))
                                      << Colors::RESET << "\n";
                }
            }
            display_metrics(response_metadata);

            if (tool_calls.empty()) {
                std::string answer = visible_answer(content);
                bool empty_response = strip(content).empty();
                bool thinking_only = answer.empty() && !strip(content).empty();
                bool promised_more = !answer.empty() && had_tool_calls && promises_next_action(answer);
                if (empty_response || thinking_only || promised_more) {
                    std::string _norm = join_ws(content);
                    if ((empty_response || thinking_only) && !_norm.empty() && _norm == stuck_last_response) {
                        bool _local = (backend == "mlx" || backend == "llama-cpp") && !mpc_active;
                        if (_local && wedge_recoveries < MAX_WEDGE_RECOVERIES) {
                            wedge_recoveries += 1;
                            stuck_last_response.clear();
                            log_warn("Identical non-answer repeated \xE2\x80\x94 server hung; auto-recovering " +
                                     std::to_string(wedge_recoveries) + "/" +
                                     std::to_string(MAX_WEDGE_RECOVERIES) +
                                     ": save -> restart -> reload -> retry.");
                            std::cout << left_indent() << Colors::ORANGE << Colors::BOLD << "!"
                                      << Colors::RESET << " " << Colors::DIM << Colors::GRAY
                                      << "The inference server looked stuck (repeated reply). Saving, restarting, "
                                         "reloading, and retrying automatically..."
                                      << Colors::RESET << "\n";
                            save_restart_reload();
                            continue;
                        }
                        log_warn("Identical non-answer repeated \xE2\x80\x94 the inference server is likely "
                                 "hung/crashed; restarting it.");
                        std::cout << left_indent() << Colors::ORANGE << Colors::BOLD << "!" << Colors::RESET
                                  << " " << Colors::DIM << Colors::GRAY
                                  << "The inference server stayed stuck after auto-recovery. Restarted it "
                                     "\xE2\x80\x94 send your message again."
                                  << Colors::RESET << "\n";
                        restart_inference_server();
                        break;
                    }
                    if (promised_more &&
                        stall_decision(content, thought_only_reprompt_count + 1) == "stop") {
                        log_warn("Model keeps re-describing a step without acting on a result it already has; "
                                 "ending turn.");
                        std::string _tail = latest_tool_result_text();
                        std::cout << left_indent() << Colors::YELLOW << Colors::BOLD << "!" << Colors::RESET
                                  << " " << Colors::DIM << Colors::GRAY
                                  << "Stuck re-describing the same step without running it \xE2\x80\x94 ended the "
                                     "turn. The last tool result is above; if it shows the goal is met (e.g. a "
                                     "connection reset/refused means the target is down), the task is already done."
                                  << Colors::RESET << "\n";
                        json gm = json::object();
                        gm["role"] = "assistant";
                        std::string gc = "[LOREA LOOP GUARD] I kept re-describing the next step without performing "
                                         "it, so I stopped. Act on the result I already have instead of restating "
                                         "the plan.";
                        if (!_tail.empty()) gc += "\nLatest tool result:\n" + utf8_substr(_tail, 0, 600);
                        gm["content"] = gc;
                        messages.push_back(gm);
                        break;
                    }
                    stuck_last_response = _norm;
                    thought_only_reprompt_count += 1;
                    if (thought_only_reprompt_count <= 3) {
                        std::string nudge;
                        if (empty_response) {
                            log_info("Model returned an empty response; prompting it to continue.");
                            nudge = "Your last response was empty. Continue the user's task now: either emit "
                                    "exactly one real tool call with complete arguments to make progress, or, if "
                                    "the task is already complete, give a concise final answer. Original request:\n" +
                                    last_user_goal;
                        } else if (thinking_only) {
                            log_info("Model stopped after thinking without acting; prompting it to continue.");
                            nudge = "You wrote your reasoning but did not act. Do not stop after thinking. Continue "
                                    "the task now by emitting exactly one real tool call (e.g. write_file, run_cmd, "
                                    "test_cmd) with complete arguments, or give the final answer if the task is "
                                    "already done.";
                        } else {
                            log_info("Model described a next step but did not act; prompting it to continue.");
                            nudge = "You described the next step but did not perform it. Do not narrate actions "
                                    "without doing them. Emit exactly one real tool call now to carry out that step, "
                                    "or, if the task is fully complete, reply with a final summary that does not "
                                    "promise further actions.";
                        }
                        json um = json::object();
                        um["role"] = "user";
                        um["content"] = nudge;
                        messages.push_back(um);
                        continue;
                    }
                    if (empty_response) {
                        log_info("Model kept returning empty responses; ending turn.");
                        std::cout << left_indent() << Colors::YELLOW << Colors::BOLD << "!" << Colors::RESET
                                  << " " << Colors::DIM << Colors::GRAY
                                  << "The model stopped producing output. Try rephrasing, /compact, or a new message."
                                  << Colors::RESET << "\n";
                    } else {
                        log_info("Model kept describing actions without acting; ending turn.");
                    }
                }
                break;
            }

            int valid_tool_calls = 0;
            bool stuck_repeating = false;
            bool force_finalize = false;
            for (auto& tool : tool_calls) {
                try {
                    json function_info = (tool.is_object() && jget(tool, "function"))
                                             ? *jget(tool, "function") : json::object();
                    std::string name = get_str(function_info, "name");
                    json args = function_info.is_object() && function_info.contains("arguments")
                                    ? function_info["arguments"] : json::object();
                    std::string call_id = (tool.is_object() && tool.contains("id"))
                                              ? get_str(tool, "id")
                                              : ("call_" + std::to_string(now_unix()));

                    if (name == "create_plan" && !planning_enabled) {
                        log_info("Ignoring create_plan because planning mode is disabled");
                        json tm = json::object();
                        tm["role"] = "tool";
                        tm["content"] = "Planning mode is disabled. Continue the original task with write_file, "
                                        "test_cmd, or run_cmd instead of create_plan.";
                        tm["tool_call_id"] = call_id;
                        tm["name"] = name;
                        messages.push_back(tm);
                        continue;
                    }

                    if (name.empty() || !in_avail(name)) {
                        log_info("Ignoring invalid tool call: " + (name.empty() ? std::string("missing tool name") : name));
                        if (goal_has({"vram", "gpu memory", "video memory", "unified memory", "how much memory"})) {
                            log_info("Replacing invalid hardware tool call with deterministic run_cmd");
                            name = "run_cmd";
                            args = json::object();
                            args["command"] = SYSPROF;
                        } else {
                            continue;
                        }
                    }

                    if (args.is_string()) {
                        std::string a = args.get<std::string>();
                        if (!strip(a).empty()) {
                            auto p = try_parse(a);
                            if (p) {
                                args = *p;
                            } else {
                                log_info("Ignoring malformed arguments for tool '" + name + "': parse error");
                                if (name == "run_cmd" && goal_has({"vram", "gpu memory", "video memory",
                                                                   "unified memory", "how much memory"})) {
                                    log_info("Replacing malformed hardware run_cmd with deterministic command");
                                    args = json::object();
                                    args["command"] = SYSPROF;
                                } else {
                                    json tm = json::object();
                                    tm["role"] = "tool";
                                    tm["content"] = "Malformed tool arguments: parse error";
                                    tm["tool_call_id"] = call_id;
                                    tm["name"] = name;
                                    messages.push_back(tm);
                                    continue;
                                }
                            }
                        } else {
                            args = json::object();
                        }
                    }

                    if (!args.is_object()) {
                        log_info("Ignoring invalid arguments for tool '" + name + "': expected object");
                        if (name == "run_cmd" && goal_has({"vram", "gpu memory", "video memory",
                                                           "unified memory", "how much memory"})) {
                            log_info("Replacing invalid hardware run_cmd arguments with deterministic command");
                            args = json::object();
                            args["command"] = SYSPROF;
                        } else {
                            json tm = json::object();
                            tm["role"] = "tool";
                            tm["content"] = "Invalid tool arguments: expected object";
                            tm["tool_call_id"] = call_id;
                            tm["name"] = name;
                            messages.push_back(tm);
                            continue;
                        }
                    }

                    if (name == "run_cmd" && wants_system_and_ip(last_user_goal)) {
                        std::string command_text = str_or_empty(jval(args, "command"));
                        std::string ct = lower_ascii(command_text);
                        if (!contains(ct, "ip") && !contains(ct, "hostname -i") && !contains(ct, "ifconfig")) {
                            log_info("Expanding system lookup command to include IP addresses.");
                            args = json::object();
                            args["command"] = system_and_ip_command();
                        }
                    }

                    if (tool_steps_this_turn >= 200) {
                        log_info("Stopping tool loop after 100 tool steps in one user turn");
                        json tm = json::object();
                        tm["role"] = "tool";
                        tm["content"] = "Tool step limit reached for this turn. Stop looping and give the user a "
                                        "concise status report with the exact remaining failure and next manual fix.";
                        tm["tool_call_id"] = call_id;
                        tm["name"] = name;
                        messages.push_back(tm);
                        break;
                    }

                    print_tool_call(name, args);
                    session_tools_run += 1;
                    if (name == "write_file" && args.is_object() && jtruthy(jval(args, "path")))
                        session_files_touched.insert(py_str(args["path"]));
                    std::size_t tool_limit = (name == "read_url") ? (std::size_t)MAX_URL_OUTPUT_LENGTH
                                                                  : (std::size_t)MAX_OUTPUT_LENGTH;

                    json call_args = args.is_object() ? args : json::object();

                    {
                        const std::set<std::string>& allowed = accepted_args(name);
                        std::vector<std::string> dropped;
                        for (auto it = call_args.begin(); it != call_args.end(); ++it)
                            if (!allowed.count(it.key())) dropped.push_back(it.key());
                        if (!dropped.empty()) {
                            log_info("Ignoring unsupported argument(s) for " + name + ": " + join(dropped, ", "));
                            json kept = json::object();
                            for (auto it = call_args.begin(); it != call_args.end(); ++it)
                                if (allowed.count(it.key())) kept[it.key()] = it.value();
                            call_args.swap(kept);
                        }
                    }

                    auto gate = offensive_gate(name, call_args);
                    if (!gate.first) {
                        std::string refusal = gate.second ? *gate.second : "";
                        log_warn("SAFETY: blocked an offensive/destructive tool call");
                        std::cout << "\n  " << status_label("SAFETY BLOCK", Colors::RED) << " " << Colors::GRAY
                                  << "\xE2\x94\x82" << Colors::RESET << " " << refusal << "\n";
                        json tm = json::object();
                        tm["role"] = "tool";
                        tm["content"] = refusal;
                        tm["tool_call_id"] = call_id;
                        tm["name"] = name;
                        messages.push_back(tm);
                        valid_tool_calls += 1;
                        had_tool_calls = true;
                        continue;
                    }

                    std::string result = truncate_output(invoke_tool(name, call_args), tool_limit);
                    tool_steps_this_turn += 1;

                    if (name == "test_cmd" || name == "run_cmd") {
                        std::optional<std::string> matched_failure;
                        for (const auto& pat : failure_patterns) {
                            std::smatch mm;
                            if (std::regex_search(result, mm, pat)) { matched_failure = mm[0].str(); break; }
                        }
                        if (matched_failure) {
                            if (last_failure_signature && *last_failure_signature == *matched_failure) {
                                repeated_failure_count += 1;
                            } else {
                                last_failure_signature = *matched_failure;
                                repeated_failure_count = 0;
                            }
                            if (repeated_failure_count >= 2) {
                                result += "\n\n[LOREA LOOP GUARD] The same failure has repeated. Do not rewrite the "
                                          "same test file or rerun the same command. First inspect the project tree "
                                          "with `find . -maxdepth 3 -type f`, then fix the actual package/import "
                                          "structure, such as missing __init__.py or wrong file placement.";
                                json tm = json::object();
                                tm["role"] = "tool";
                                tm["content"] = result;
                                tm["tool_call_id"] = call_id;
                                tm["name"] = name;
                                messages.push_back(tm);
                                valid_tool_calls += 1;
                                had_tool_calls = true;
                                continue;
                            }
                        }
                    }

                    std::string _sig = norm_tool_sig(name, call_args);
                    std::string _body = result;
                    if (startswith(_body, "Command Output:")) _body = _body.substr(std::strlen("Command Output:"));
                    bool _is_empty = strip(_body).empty() || startswith(lstrip(_body), "(no output)");
                    auto _it = turn_sig_cache.find(_sig);
                    SigCacheEntry* _prior = (_it != turn_sig_cache.end()) ? &_it->second : nullptr;
                    if (_prior && (_is_empty || _prior->result == result)) {
                        _prior->count += 1;
                        std::string _interp =
                            _is_empty
                                ? std::string(" An EMPTY result is itself a valid, complete answer: the "
                                              "pattern/target is simply ABSENT here \xE2\x80\x94 that IS the finding. "
                                              "Report that, or broaden the search (different pattern, wider path, or "
                                              "another tool). Do NOT retry it as if it were an error.")
                                : std::string(" It returned the exact same output as before, so re-running it cannot "
                                              "reveal anything new.");
                        if (!_is_empty && _prior->count == 1) {
                            std::string _note = "[LOREA LOOP GUARD] You already ran `" + name +
                                                "` with these exact arguments this turn and got this same result." +
                                                _interp +
                                                " Do NOT issue this identical call again. Take a different action "
                                                "(adjust the pattern, try a different path or tool) or answer the user "
                                                "directly now.";
                            std::string _guarded = _is_empty ? _note : (result + "\n\n" + _note);
                            json tm = json::object();
                            tm["role"] = "tool";
                            tm["content"] = _guarded;
                            tm["tool_call_id"] = call_id;
                            tm["name"] = name;
                            messages.push_back(tm);
                            valid_tool_calls += 1;
                            had_tool_calls = true;
                            continue;
                        }
                        std::string _note = "[LOREA LOOP GUARD] You have issued this identical `" + name + "` call " +
                                            std::to_string(_prior->count + 1) +
                                            " times with the same result." + _interp +
                                            " STOP calling tools now and write your complete final answer to the user "
                                            "from what you already have.";
                        std::string _guarded = _is_empty ? _note : (result + "\n\n" + _note);
                        json tm = json::object();
                        tm["role"] = "tool";
                        tm["content"] = _guarded;
                        tm["tool_call_id"] = call_id;
                        tm["name"] = name;
                        messages.push_back(tm);
                        valid_tool_calls += 1;
                        had_tool_calls = true;
                        if (_is_empty || _prior->count >= 3) { stuck_repeating = true; break; }
                        force_finalize = true;
                        continue;
                    }
                    turn_sig_cache[_sig] = SigCacheEntry{result, 0};

                    json tm = json::object();
                    tm["role"] = "tool";
                    tm["content"] = result;
                    tm["tool_call_id"] = call_id;
                    tm["name"] = name;
                    messages.push_back(tm);
                    valid_tool_calls += 1;
                    had_tool_calls = true;
                } catch (const std::invalid_argument& e) {

                    std::string tool_name = "unknown";
                    if (tool.is_object()) { const json* fi = jget(tool, "function"); if (fi) tool_name = get_str(*fi, "name", "unknown"); }
                    std::string call_id = (tool.is_object() && tool.contains("id"))
                                              ? get_str(tool, "id") : ("call_" + std::to_string(now_unix()));
                    log_info("Tool '" + tool_name + "' was called with invalid arguments: " + e.what());
                    json tm = json::object();
                    tm["role"] = "tool";
                    tm["content"] = "Invalid tool arguments for " + tool_name + ": " + e.what() + "\n" +
                                    tool_reminder_text();
                    tm["tool_call_id"] = call_id;
                    tm["name"] = tool_name;
                    messages.push_back(tm);
                } catch (const std::exception& e) {
                    if (std::string(e.what()) == "KeyboardInterrupt") throw;
                    std::string tool_name = "unknown";
                    if (tool.is_object()) { const json* fi = jget(tool, "function"); if (fi) tool_name = get_str(*fi, "name", "unknown"); }
                    std::string call_id = (tool.is_object() && tool.contains("id"))
                                              ? get_str(tool, "id") : ("call_" + std::to_string(now_unix()));
                    log_info("Tool '" + tool_name + "' failed: " + e.what());
                    json tm = json::object();
                    tm["role"] = "tool";
                    tm["content"] = std::string("Tool failed: ") + e.what();
                    tm["tool_call_id"] = call_id;
                    tm["name"] = tool_name;
                    messages.push_back(tm);
                }
            }

            if (stuck_repeating) {
                interrupter.stop_listening();
                return false;
            }

            if (force_finalize) {
                json um = json::object();
                um["role"] = "user";
                um["content"] = "Stop calling tools. You repeated the same query with no new information. Using "
                                "ONLY what is already above, write your complete final answer to the original "
                                "request now, in plain natural language:\n" + last_user_goal;
                messages.push_back(um);
                continue;
            }

            if (valid_tool_calls == 0) {
                if (goal_has({"vram", "gpu memory", "video memory", "unified memory", "how much memory"})) {
                    log_info("No valid hardware tool call was produced; running deterministic hardware command");
                    std::string result = truncate_output(run_cmd(SYSPROF));
                    json tm = json::object();
                    tm["role"] = "tool";
                    tm["content"] = result;
                    tm["tool_call_id"] = "hardware_fallback_" + std::to_string(now_unix());
                    tm["name"] = "run_cmd";
                    messages.push_back(tm);
                    return true;
                }
                invalid_tool_reprompt_count += 1;
                std::string decision = stall_decision(content, invalid_tool_reprompt_count);
                if (decision == "stop" || plain_text_pushed) {
                    log_warn("Repeated invalid tool calls with no progress; ending turn instead of looping.");
                    std::cout << left_indent() << Colors::YELLOW << Colors::BOLD << "!" << Colors::RESET << " "
                              << Colors::DIM << Colors::GRAY
                              << "Kept emitting invalid tool calls (bad/unknown tool name or arguments) without "
                                 "progress \xE2\x80\x94 ended the turn."
                              << Colors::RESET << "\n";
                    json gm = json::object();
                    gm["role"] = "assistant";
                    gm["content"] = "[LOREA LOOP GUARD] I kept emitting tool calls that aren't valid (likely a tool "
                                    "name that does not exist or malformed arguments) and made no progress, so I "
                                    "stopped instead of looping. Valid tools: " + join(avail, ", ") +
                                    ". Tell me which to use, or rephrase the request.";
                    messages.push_back(gm);
                    break;
                }
                if (decision == "finalize") {
                    plain_text_pushed = true;
                    log_info("No valid tool call after repeated reminders; asking once for a plain-text answer.");
                    json um = json::object();
                    um["role"] = "user";
                    um["content"] = "You still have not produced a valid tool call after several reminders. Stop "
                                    "trying to call tools and, using the information already above, write your "
                                    "complete answer to the original request now in plain natural language.";
                    messages.push_back(um);
                    continue;
                }
                json um = json::object();
                um["role"] = "user";
                um["content"] = "Your previous tool call was invalid, incomplete, or repetitive. Original request:\n" +
                                last_user_goal + "\n" + tool_reminder_text();
                messages.push_back(um);
                continue;
            }
            continue;
        } catch (const std::exception& e) {
            if (std::string(e.what()) == "KeyboardInterrupt") throw;
            if (spinner) spinner->stop();
            interrupter.stop_listening();
            json am = json::object();
            am["role"] = "assistant";
            am["content"] = std::string("[LOREA ERROR] ") + e.what();
            messages.push_back(am);
            std::cout << Colors::RED << "Error: " << e.what() << Colors::RESET << "\n";
            return false;
        }
    }

    return false;
}

}
