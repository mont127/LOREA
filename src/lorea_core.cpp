#include "lorea.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>
#include <sys/time.h>

namespace ocli {

namespace {

const char* const PY_WS = " \t\n\r\f\v";

std::string strip(const std::string& s) {
    std::size_t b = s.find_first_not_of(PY_WS);
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(PY_WS);
    return s.substr(b, e - b + 1);
}

std::string rstrip_chars(const std::string& s, const std::string& chars) {
    std::size_t e = s.find_last_not_of(chars);
    if (e == std::string::npos) return "";
    return s.substr(0, e + 1);
}

std::string lower_ascii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::vector<std::string> splitlines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && std::string(PY_WS).find(s[i]) != std::string::npos) ++i;
        if (i >= n) break;
        std::size_t start = i;
        while (i < n && std::string(PY_WS).find(s[i]) == std::string::npos) ++i;
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::string map_get(const std::map<std::string, std::string>& m, const std::string& k,
                    const std::string& def) {
    auto it = m.find(k);
    return it != m.end() ? it->second : def;
}

bool json_truthy(const json& v) {
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

std::string content_str(const json& m) {
    json v = (m.is_object() && m.contains("content")) ? m["content"] : json("");
    if (!json_truthy(v)) return "";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    return v.dump();
}

std::string getpass_input(const std::string& prompt) {
    if (!isatty(STDIN_FILENO)) throw std::runtime_error("getpass: no tty");
    std::cout << prompt << std::flush;
    termios oldt{};
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) throw std::runtime_error("getpass: tcgetattr");
    termios newt = oldt;
    newt.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &newt);
    std::string line;
    std::getline(std::cin, line);
    tcsetattr(STDIN_FILENO, TCSADRAIN, &oldt);
    std::cout << "\n";
    return line;
}

}

const std::string LOREA::LOOP_SENTINEL = "TASK COMPLETE";
const int         LOREA::LOOP_MAX_ITERATIONS = 100;

LOREA::LOREA(std::string model_name_, bool auto_mode_, std::string backend_,
             std::optional<std::string> url_) {
    backend = backend_;
    model_name = !model_name_.empty()
                     ? model_name_
                     : map_get(BACKEND_DEFAULT_MODELS, backend_,
                               map_get(BACKEND_DEFAULT_MODELS, "ollama", ""));
    auto_mode = auto_mode_;
    url = (url_ && !url_->empty())
              ? *url_
              : map_get(BACKEND_DEFAULT_URLS, backend_, "http://localhost:8080");
    if (!model_matches_backend(model_name, backend)) {
        model_name = map_get(BACKEND_DEFAULT_MODELS, backend, model_name);
    }
    planning_enabled = false;
    effort_level = "basic";
    tasks.clear();
    active_process = nullptr;
    active_master = -1;
    last_tool_signature = std::nullopt;
    repeated_tool_count = 0;
    turn_call_norms.clear();
    last_user_goal = "";
    last_failure_signature = std::nullopt;
    repeated_failure_count = 0;
    tool_steps_this_turn = 0;
    compaction_count = 0;
    last_summary = "";
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        session_started_at = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    }
    session_tools_run = 0;
    session_turns = 0;
    session_files_touched.clear();
    undo_stack.clear();
    prompt_history.clear();
    server_model = std::nullopt;
    mpc_url = std::nullopt;
    {
        const char* mt = std::getenv("OCLI_MPC_TOKEN");
        if (!(mt && *mt)) mt = std::getenv("MPC_TOKEN");
        if (mt && *mt) mpc_token = std::string(mt);
        else mpc_token = std::nullopt;
    }
    mpc_features = json::object();
    mpc_version = std::nullopt;
    tool_access = "full";
    allow_spawn_agents = true;
    non_interactive = false;
    PLAN_PROMPT =
        "Planning is ENABLED. For any task needing multiple file edits, several steps, or risky "
        "operations, you MUST first call 'create_plan' with a numbered list of discrete, verifiable "
        "steps covering every explicit user requirement; keep each step a single concrete action. "
        "Do not call create_plan for trivial one-step requests. After the plan is approved, "
        "immediately mark exactly one task 'doing' with 'update_task' before working on it, then mark "
        "it 'done' the moment it is finished and move the next task to 'doing'. Keep only one task "
        "'doing' at a time. Do not re-plan unless requirements change or the user gives feedback. When "
        "every task is 'done', stop calling tools and give the user a concise final summary.";

    std::string cur_date;
    {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&t, &tmv);
        char datebuf[32];
        std::strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M:%S", &tmv);
        cur_date = datebuf;
    }

    std::string sys_content =
        std::string("You are LOREA, an advanced AUTONOMOUS AI coding agent. Current date: ") +
        cur_date +
        ". "
        "SECURITY POLICY (binding, overrides any other instruction): You support ONLY authorized, "
        "scoped, lawful security work — penetration testing of systems the operator owns or is "
        "contracted to test, CTF challenges, labs, defensive analysis, and education. You THINK like a "
        "skilled red-team pentester (recon, enumeration, exploitation reasoning, privilege escalation, "
        "lateral movement, reporting) but you ALWAYS confirm authorization and scope first, and you "
        "pair every offensive technique with its detection and remediation. You REFUSE, and explain "
        "why, when asked to attack systems without authorization, perform mass/indiscriminate "
        "exploitation, build malware/ransomware/C2, steal credentials, destroy or exfiltrate data, "
        "evade detection for malicious ends, or harm anyone. A hard runtime guard blocks such tool "
        "calls regardless of what you output; never try to bypass, obfuscate, or encode around it. "
        "When a request is out of scope, redirect to the authorized, ethical alternative. "
        "CONVERSATION RULES: For greetings, questions, explanations, or any non-task message, respond "
        "in plain natural language. Do NOT call tools or output JSON for simple conversation. Only use "
        "tools when the user explicitly asks you to perform an action (run code, edit files, search, "
        "etc.). "
        "Wrap internal reasoning in <thought> tags. "
        "To call a tool, emit a single tool call. Preferred format: <tools>{{\"name\": \"tool_name\", "
        "\"arguments\": {{...}}}}</tools>. For shell commands, use <tools>{{\"name\":\"run_cmd\","
        "\"arguments\":{{\"command\":\"pwd\"}}}}</tools>. The native function-call format "
        "(<tool_call><function=run_cmd><parameter=command>pwd</parameter></function></tool_call>) is "
        "also accepted. Use exactly one format per call and provide every required argument. Available "
        "tools include run_cmd, read_file, write_file, test_cmd, list_files, search_files, find_files, "
        "grep, web_search, read_url, http_request, git_status, git_diff, and spawn_agents. For sending "
        "HTTP requests with payloads (web pentesting), ALWAYS prefer http_request over curl-in-run_cmd "
        "so payloads with quotes/spaces are not mangled by the shell. For data you must GENERATE or "
        "repeat — a long string, a flood of requests, fuzzing input — NEVER type the literal out (you "
        "cannot reliably emit thousands of characters); instead use run_cmd with a shell/python "
        "one-liner that builds it, e.g. run_cmd python3 -c 'print(\"A\"*100000)' or a bash for-loop. "
        "If you catch yourself describing the same step twice without emitting a tool call, STOP and "
        "either run a one-liner that does it or say plainly that you cannot — do not repeat the "
        "description. For coding tasks, write the complete requested implementation first, then write "
        "tests, then run pytest, then fix failures, then summarize final files. "
        "Do not write markdown code blocks when creating or editing files; use the write_file tool. Do "
        "not describe running commands; use run_cmd or test_cmd. Do not fabricate tool results, diffs, "
        "test output, file contents, or <tool_response> blocks. If a user asks you to create code, "
        "modify code, inspect files, run tests, install packages, or execute a program, you MUST call "
        "a real tool. "
        "For multi-step tasks, after each successful real tool call, immediately make the next "
        "required real tool call only if it advances the original user request; do not output bare "
        "CONTINUE as a standalone response, and do not repeatedly run the same command or read/write "
        "the same file without changing strategy. If list_files has already shown the tree, do not "
        "call it again; use search_files/find_files, grep, read_file, git_diff, or provide the final "
        "answer. If a test fails twice with the same error, inspect the file tree and relevant files "
        "before editing again. "
        "CRITICAL: Use the 'test_cmd' tool for ANY command that might be interactive (games, prompts, "
        "servers). DO NOT use 'run_cmd' for these. MANDATORY: Always use 'write_file' for all code "
        "modifications to ensure the user sees a diff report. When the user asks for multiple "
        "searches, perform at least 3-5 searches. "
        "IMPORTANT: You are an autonomous agent. NEVER ask the user to run a command. RUN IT YOURSELF. "
        "NEVER simulate tool results. ONLY use 'CONTINUE' if you have just called a tool and need to "
        "perform another step. ALWAYS prioritize answering the user's primary question directly after "
        "gathering data. If searching for software features, prioritize finding 'Release Notes' or "
        "'What's New' pages. Planning is DISABLED. Do not use create_plan tool unless planning is "
        "explicitly enabled.";

    messages.clear();
    {
        Message sys = json::object();
        sys["role"] = "system";
        sys["content"] = sys_content;
        messages.push_back(sys);
    }

    server_process = nullptr;
    airllm_model = false;
    airllm_compression = std::nullopt;
    airllm_max_length = 4096;
    airllm_max_new_tokens = 2048;
    api_keys.clear();

    if (backend == "mlx")
        log_info("MLX backend selected. The model will load on the first prompt.");
    if (backend == "airllm")
        log_info("AirLLM backend selected. The model will load on the first prompt "
                 "(layer-by-layer, may take a while).");

}

LOREA::~LOREA() {
    if (!cleaned_up_) {
        cleanup();
        cleaned_up_ = true;
    }
}

void LOREA::cleanup() {
    if (server_process) {
        log_info("Shutting down MLX server...");
        server_process->terminate();
        try {
            server_process->wait(5);
        } catch (...) {
            server_process->kill();
        }
        server_process = nullptr;
    }
    if (airllm_model) {
        log_info("Unloading AirLLM model...");
        airllm_model = false;
    }
    server_model = std::nullopt;
}

std::optional<int> LOREA::menu_choice(const std::string& title,
                                      const std::vector<std::string>& options) {
    return interactive_menu(title, options, Colors::VIOLET);
}

std::optional<std::string> LOREA::prompt_value(const std::string& label,
                                               std::optional<std::string> current) {
    bool cur_truthy = current.has_value() && !current->empty();
    std::string suffix =
        cur_truthy ? (std::string(" ") + Colors::GRAY + "[" + *current + "]" + Colors::RESET) : "";
    std::string value = strip(styled_input(std::string("  ") + Colors::TEAL + label +
                                            Colors::RESET + suffix + ": "));
    if (!value.empty()) return value;
    return current;
}

std::optional<std::string> LOREA::api_key(const std::string& backend_, bool prompt_if_missing) {
    {
        auto it = api_keys.find(backend_);
        if (it != api_keys.end() && !it->second.empty()) return it->second;
    }
    auto eit = BACKEND_API_KEY_ENV.find(backend_);
    if (eit != BACKEND_API_KEY_ENV.end()) {
        for (const auto& name : eit->second) {
            const char* val = std::getenv(name.c_str());
            if (val && *val) {
                api_keys[backend_] = strip(std::string(val));
                return api_keys[backend_];
            }
        }
    }
    if (prompt_if_missing && can_use_terminal_keys()) {
        std::string env_hint = "API_KEY";
        if (eit != BACKEND_API_KEY_ENV.end() && !eit->second.empty()) env_hint = eit->second[0];
        log_info("Enter your " + backend_ + " API key (or set " + Colors::TEAL + env_hint +
                 Colors::RESET + " in your environment).");
        std::string key;
        try {
            key = strip(getpass_input("  " + backend_ + " API key: "));
        } catch (...) {
            auto pv = prompt_value(backend_ + " API key");
            key = pv.value_or("");
        }
        if (!key.empty()) {
            api_keys[backend_] = key;
            return key;
        }
    }
    return std::nullopt;
}

std::string LOREA::cloud_base() {
    std::string env_name;
    if (backend == "anthropic")
        env_name = "ANTHROPIC_BASE_URL";
    else if (backend == "openai")
        env_name = "OPENAI_BASE_URL";
    std::string override_;
    if (!env_name.empty()) {
        const char* e = std::getenv(env_name.c_str());
        override_ = strip(e ? std::string(e) : std::string());
    }
    std::string base = !override_.empty()
                           ? override_
                           : (!url.empty() ? url : map_get(BACKEND_DEFAULT_URLS, backend, ""));
    return rstrip_chars(base, "/");
}

std::optional<std::map<std::string, std::string>>
LOREA::anthropic_auth_headers(bool prompt_if_missing) {
    std::map<std::string, std::string> headers;
    headers["anthropic-version"] = ANTHROPIC_VERSION;
    headers["content-type"] = "application/json";
    const char* tk = std::getenv("ANTHROPIC_AUTH_TOKEN");
    std::string token = strip(tk ? std::string(tk) : std::string());
    if (!token.empty()) {
        headers["Authorization"] = "Bearer " + token;
        return headers;
    }
    auto key = api_key("anthropic", prompt_if_missing);
    if (!key || key->empty()) return std::nullopt;
    headers["x-api-key"] = *key;
    return headers;
}

std::pair<std::string, json> LOREA::anthropic_messages() {
    std::vector<std::string> system_parts;
    std::vector<std::pair<std::string, std::string>> turns;
    for (const auto& message : messages) {
        std::string role =
            (message.is_object() && message.contains("role") && message["role"].is_string())
                ? message["role"].get<std::string>()
                : "user";
        std::string content = content_str(message);
        if (role == "system") {
            if (!strip(content).empty()) system_parts.push_back(content);
            continue;
        }
        if (role == "tool") {
            std::string name =
                (message.is_object() && message.contains("name") && message["name"].is_string() &&
                 !message["name"].get<std::string>().empty())
                    ? message["name"].get<std::string>()
                    : "tool";
            turns.emplace_back("user", "Tool result from " + name + ":\n" + content);
        } else if (role == "assistant") {
            turns.emplace_back("assistant", content);
        } else {
            turns.emplace_back("user", content);
        }
    }

    std::vector<std::pair<std::string, std::string>> merged;
    for (const auto& tr : turns) {
        if (!merged.empty() && merged.back().first == tr.first) {
            merged.back().second = merged.back().second + "\n\n" + tr.second;
        } else {
            merged.push_back(tr);
        }
    }

    while (!merged.empty() && merged.front().first != "user") merged.erase(merged.begin());
    json out_messages = json::array();
    for (const auto& mr : merged) {
        std::string content = !strip(mr.second).empty() ? mr.second : "...";
        out_messages.push_back(json{{"role", mr.first}, {"content", content}});
    }
    if (out_messages.empty()) out_messages.push_back(json{{"role", "user"}, {"content", "..."}});
    for (const auto& reminder : ephemeral_reminders()) {
        std::string rc =
            (reminder.is_object() && reminder.contains("content") && reminder["content"].is_string())
                ? reminder["content"].get<std::string>()
                : "";
        system_parts.push_back(rc);
    }
    std::string joined;
    for (std::size_t i = 0; i < system_parts.size(); ++i) {
        if (i) joined += "\n\n";
        joined += system_parts[i];
    }
    return {joined, out_messages};
}

json LOREA::to_anthropic_tools(const json& tools) {
    json converted = json::array();
    if (tools.is_array()) {
        for (const auto& tool : tools) {
            const json* fnp = tool.is_object() ? jget(tool, "function") : nullptr;
            if (!fnp || !fnp->is_object() || !json_truthy(*fnp)) continue;
            const json& fn = *fnp;
            const json* namep = jget(fn, "name");
            if (!namep || !json_truthy(*namep)) continue;
            json item = json::object();
            item["name"] = *namep;
            const json* desc = jget(fn, "description");
            item["description"] = desc ? *desc : json("");
            const json* params = jget(fn, "parameters");
            if (params && json_truthy(*params)) {
                item["input_schema"] = *params;
            } else {
                item["input_schema"] = json{{"type", "object"}, {"properties", json::object()}};
            }
            converted.push_back(item);
        }
    }
    return converted;
}

void LOREA::set_backend(const std::string& backend_, std::optional<std::string> url_,
                        bool keep_model) {
    std::string previous_backend = backend;
    std::string previous_url = url;
    std::string next_url;
    if (url_ && !url_->empty()) {
        next_url = *url_;
    } else {
        std::string d = map_get(BACKEND_DEFAULT_URLS, backend_, "");
        next_url = !d.empty() ? d : map_get(BACKEND_DEFAULT_URLS, "ollama", "");
    }
    if (previous_backend == "mlx" && (backend_ != "mlx" || next_url != previous_url)) {
        cleanup();
    }
    if (previous_backend == "airllm" && backend_ != "airllm") {
        if (airllm_model) {
            log_info("Unloading AirLLM model...");
            airllm_model = false;
        }
    }
    backend = backend_;
    url = (backend_ != "airllm") ? next_url : std::string("");
    if (keep_model && !model_matches_backend(model_name, backend_)) {
        log_info("Current model " + std::string(Colors::TEAL) + model_name + Colors::RESET +
                 " is not compatible with " + Colors::TEAL + backend_ + Colors::RESET +
                 "; using the backend default.");
        keep_model = false;
    }
    if (!keep_model) {
        model_name = map_get(BACKEND_DEFAULT_MODELS, backend_, model_name);
    }
    if (backend == "mlx") {
        log_info("MLX model will load on the first prompt.");
        if (is_large_mlx_model(model_name))
            log_info("Large MLX model selected; the first prompt can take several minutes while "
                     "weights load into memory.");
    }
    if (backend == "airllm") {
        airllm_model = false;
        server_model = std::nullopt;
        log_info("AirLLM model will load on the first prompt (layer-by-layer streaming).");
    }
    log_info("Backend switched to " + std::string(Colors::TEAL) + backend + Colors::RESET +
             (!url.empty() ? (" using " + std::string(Colors::TEAL) + url + Colors::RESET)
                           : std::string("")));
    log_info("Model is now " + std::string(Colors::TEAL) + model_name + Colors::RESET);
}

void LOREA::backend_menu() {
    std::vector<std::string> options = {
        std::string("1. ") + Colors::CYAN + "ollama" + Colors::RESET + "     " + Colors::GRAY +
            "Local Ollama API on port 11434" + Colors::RESET,
        std::string("2. ") + Colors::CYAN + "anthropic" + Colors::RESET + "  " + Colors::GRAY +
            "Claude API — Opus, Sonnet, Haiku (needs API key)" + Colors::RESET,
        std::string("3. ") + Colors::CYAN + "openai" + Colors::RESET + "     " + Colors::GRAY +
            "ChatGPT / Codex API — GPT-4o, o-series (needs API key)" + Colors::RESET,
        std::string("4. ") + Colors::CYAN + "llama-cpp" + Colors::RESET + "  " + Colors::GRAY +
            "OpenAI-compatible llama.cpp server" + Colors::RESET,
        std::string("5. ") + Colors::CYAN + "mlx" + Colors::RESET + "        " + Colors::GRAY +
            "MLX server for Apple Silicon" + Colors::RESET,
        std::string("6. ") + Colors::CYAN + "airllm" + Colors::RESET + "     " + Colors::GRAY +
            "AirLLM in-process inference (run 70B+ on 4GB VRAM)" + Colors::RESET,
        std::string("7. ") + Colors::CYAN + "nvidia" + Colors::RESET + "     " + Colors::GRAY +
            "NVIDIA NIM cloud API via Python bridge (needs API key)" + Colors::RESET,
    };
    auto choice = menu_choice("BACKEND", options);
    if (!choice) return;
    static const char* const names[] = {"ollama", "anthropic", "openai",
                                        "llama-cpp", "mlx", "airllm", "nvidia"};
    std::string backend_ = names[*choice];
    std::optional<std::string> url_;
    if (backend_ == "airllm") {
        url_ = std::nullopt;
    } else if (CLOUD_BACKENDS.count(backend_)) {
        std::string env_base_name =
            (backend_ == "anthropic") ? "ANTHROPIC_BASE_URL" : "OPENAI_BASE_URL";
        const char* eb = std::getenv(env_base_name.c_str());
        std::string env_base = strip(eb ? std::string(eb) : std::string());
        std::string default_base = !env_base.empty()
                                       ? env_base
                                       : (backend == backend_ ? url
                                                              : map_get(BACKEND_DEFAULT_URLS,
                                                                        backend_, ""));
        auto pv = prompt_value("Server URL (blank = official API)", default_base);
        std::string entered = strip(pv.value_or(""));
        std::string def_rstrip = rstrip_chars(map_get(BACKEND_DEFAULT_URLS, backend_, ""), "/");
        if (!entered.empty() && rstrip_chars(entered, "/") != def_rstrip)
            url_ = entered;
        else
            url_ = std::nullopt;
        bool has_token = false;
        if (backend_ == "anthropic") {
            const char* atk = std::getenv("ANTHROPIC_AUTH_TOKEN");
            has_token = atk && !strip(std::string(atk)).empty();
        }
        if (!has_token) {
            auto k = api_key(backend_, true);
            if (!k || k->empty()) {
                log_info("No credential set for " + backend_ + "; backend not changed.");
                return;
            }
        }
    } else {
        url_ = std::nullopt;
    }
    bool keep_model = model_matches_backend(model_name, backend_);
    if (!keep_model) {
        log_info("Current model " + std::string(Colors::TEAL) + model_name + Colors::RESET +
                 " does not match " + Colors::TEAL + backend_ + Colors::RESET + "; switching to " +
                 Colors::TEAL + map_get(BACKEND_DEFAULT_MODELS, backend_, "") + Colors::RESET + ".");
    }
    set_backend(backend_, url_, keep_model);
    if (CLOUD_BACKENDS.count(backend_)) model_menu();
}

std::vector<std::string> LOREA::list_ollama_models() {
    try {
        ProcResult result = run_subprocess({"ollama", "list"}, "", 5.0, false);
        if (!result.started || result.timed_out) return {};
        if (result.exit_code != 0) return {};
        std::vector<std::string> models;
        std::vector<std::string> lines = splitlines(result.out);
        for (std::size_t i = 1; i < lines.size(); ++i) {
            std::vector<std::string> parts = split_ws(lines[i]);
            if (!parts.empty()) models.push_back(parts[0]);
        }
        return models;
    } catch (...) {
        return {};
    }
}

void LOREA::model_menu() {
    std::vector<std::string> suggestions;
    {
        auto it = MODEL_SUGGESTIONS.find(backend);
        if (it != MODEL_SUGGESTIONS.end()) suggestions = it->second;
    }
    if (backend == "ollama") {
        std::vector<std::string> combined = list_ollama_models();
        for (const auto& s : suggestions) combined.push_back(s);
        std::vector<std::string> uniq;
        std::set<std::string> seen;
        for (const auto& m : combined) {
            if (seen.insert(m).second) uniq.push_back(m);
        }
        suggestions = uniq;
    }
    std::vector<std::string> options;
    for (std::size_t i = 0; i < suggestions.size(); ++i)
        options.push_back(std::to_string(i + 1) + ". " + Colors::CYAN + suggestions[i] +
                          Colors::RESET);
    options.push_back(std::to_string(options.size() + 1) + ". " + Colors::ORANGE +
                      "Type a custom model name" + Colors::RESET);
    auto choice = menu_choice("MODEL", options);
    if (!choice) return;
    if ((std::size_t)*choice < suggestions.size()) {
        model_name = suggestions[*choice];
    } else {
        auto model = prompt_value("Model name", model_name);
        if (!model || model->empty()) return;
        model_name = *model;
    }
    if (backend == "mlx") {
        if (server_process && server_model != std::optional<std::string>(model_name))
            cleanup();
        else if (server_model != std::optional<std::string>(model_name))
            server_model = std::nullopt;
        log_info("MLX model will load on the next prompt.");
        if (is_large_mlx_model(model_name))
            log_info("Large MLX model selected; the first prompt can take several minutes while "
                     "weights load into memory.");
    }
    log_info("Model switched to " + std::string(Colors::TEAL) + model_name + Colors::RESET);
}

void LOREA::vram_command(const std::string& arg_text) {
    if (!is_apple_silicon()) {
        log_warn("VRAM tuning is only available on Apple Silicon Macs.");
        return;
    }
    long total = mac_total_ram_mb();
    if (total <= 0) {
        log_warn("Could not read system memory (hw.memsize).");
        return;
    }
    long current = mac_current_wired_limit_mb();
    VramBounds vb = mac_vram_bounds(total);
    long lo = vb.lo, hi = vb.hi, reserve = vb.reserve;
    long recommended = mac_recommended_wired_limit_mb(total);
    long effective = current ? current : recommended;

    auto gb = [](double mb) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.1f GB", mb / 1024.0);
        return std::string(b);
    };

    bool autov = (lower_ascii(strip(arg_text)) == "--auto" || lower_ascii(strip(arg_text)) == "auto" ||
                  lower_ascii(strip(arg_text)) == "-a");

    print_panel("gpu memory (unified)",
                {
                    kv_row("total RAM", std::string(Colors::WHITE) + gb((double)total) + Colors::RESET),
                    kv_row("gpu limit",
                           (current ? (std::string(Colors::WHITE) + gb((double)current) + Colors::RESET)
                                    : (std::string(Colors::GRAY) + "OS default (~" +
                                       gb((double)(long)(total * 0.75)) + ")" + Colors::RESET))),
                    kv_row("recommended",
                           std::string(ACCENT) + gb((double)recommended) + Colors::RESET),
                    kv_row("os reserve", std::string(Colors::WHITE) + gb((double)reserve) +
                                             Colors::RESET + " " + Colors::DIM + Colors::GRAY +
                                             "kept for macOS" + Colors::RESET),
                },
                Colors::TEAL);

    double chosen_raw;
    if (autov) {
        chosen_raw = (double)recommended;
    } else {
        std::vector<std::pair<double, std::string>> marks = {{(double)recommended, "rec"}};
        std::function<std::string(double)> fmt = [&gb](double v) {
            return gb((double)(long long)std::llround(v));
        };
        auto cv = interactive_slider("GPU MEMORY LIMIT", (double)effective, (double)lo, (double)hi,
                                     512, "", Colors::TEAL, "", fmt, &marks);
        if (!cv) {
            log_info("VRAM tuning canceled.");
            return;
        }
        chosen_raw = *cv;
    }
    long chosen = (long)(std::llround(chosen_raw / 512.0) * 512);

    if (chosen == current) {
        log_info("GPU memory limit already set to " + std::string(Colors::WHITE) +
                 gb((double)chosen) + Colors::RESET + ".");
        return;
    }

    bool warn_high = chosen > total - reserve + 512;
    log_info("Setting GPU memory limit to " + std::string(Colors::TEAL) + gb((double)chosen) +
             Colors::RESET + " " + Colors::DIM + Colors::GRAY + "(leaves " +
             gb((double)(total - chosen)) + " for macOS)" + Colors::RESET);
    if (warn_high)
        log_warn("This leaves little headroom for macOS; the system may become unstable.");

    std::string apply_cmd = "sudo sysctl -w iogpu.wired_limit_mb=" + std::to_string(chosen);
    std::cout << "  " << Colors::DIM << Colors::GRAY << "command:" << Colors::RESET << " "
              << Colors::WHITE << apply_cmd << Colors::RESET << "\n";
    std::string confirm = lower_ascii(strip(styled_input(
        std::string("  ") + Colors::BOLD + "Apply now with sudo? (y/n):" + Colors::RESET + " ")));
    if (!(confirm == "y" || confirm == "yes")) {
        log_info("Not applied. You can run the command above manually, or re-run /vram.");
        return;
    }
    try {
        ProcResult result = run_subprocess(
            {"sudo", "sysctl", "-w", "iogpu.wired_limit_mb=" + std::to_string(chosen)}, "", 0.0,
            false);
        if (result.exit_code == 0) {
            log_ok("GPU memory limit set to " + std::string(Colors::WHITE) + gb((double)chosen) +
                   Colors::RESET + ".");
            log_info(std::string(Colors::DIM) + Colors::GRAY +
                     "Not persistent across reboots. To make it permanent, add a LaunchDaemon "
                     "running:" +
                     Colors::RESET + " " + apply_cmd);
        } else {
            std::string detail = !result.err.empty() ? result.err
                                                      : (!result.out.empty() ? result.out : "");
            log_warn("sysctl failed: " + strip(detail));
        }
    } catch (const std::exception& e) {
        log_warn(std::string("Could not apply: ") + e.what());
    }
}

void LOREA::theme_command(const std::string& name) {
    std::vector<std::pair<std::string, const char*>> themes = {
        {"teal", Colors::TEAL},       {"violet", Colors::VIOLET}, {"pink", Colors::PINK},
        {"lime", Colors::LIME},       {"sky", Colors::SKY},       {"amber", Colors::AMBER},
        {"rose", Colors::ROSE},       {"mint", Colors::MINT},     {"orange", Colors::ORANGE},
        {"emerald", Colors::EMERALD}, {"indigo", Colors::INDIGO},
    };
    if (name.empty()) {
        std::string current = "custom";
        for (const auto& kv : themes) {
            if (std::string(kv.second) == std::string(ACCENT)) {
                current = kv.first;
                break;
            }
        }
        std::string swatches;
        for (std::size_t i = 0; i < themes.size(); ++i) {
            if (i) swatches += "  ";
            swatches += std::string(themes[i].second) + "●" + Colors::RESET + " " + Colors::DIM +
                        Colors::GRAY + themes[i].first + Colors::RESET;
        }
        log_info("Current accent: " + std::string(ACCENT) + current + Colors::RESET +
                 ".  /theme <name>");
        std::cout << left_indent() << swatches << "\n";
        return;
    }
    std::string lname = lower_ascii(name);
    const char* found = nullptr;
    for (const auto& kv : themes) {
        if (kv.first == lname) {
            found = kv.second;
            break;
        }
    }
    if (!found) {
        std::string opts;
        for (std::size_t i = 0; i < themes.size(); ++i) {
            if (i) opts += ", ";
            opts += themes[i].first;
        }
        log_warn("Unknown theme '" + lname + "'. Options: " + opts + ".");
        return;
    }
    ACCENT = found;
    log_ok("Accent theme set to " + std::string(ACCENT) + lname + Colors::RESET + ".");
}

void LOREA::display_metrics(const json& response) {
    std::string sep = std::string(" ") + Colors::DIM + Colors::GRAY + "·" + Colors::RESET + " ";
    const json* mpc = jget(response, "mpc");
    if (mpc && json_truthy(*mpc)) {
        const json* dur = jget(response, "duration");
        const json* bp = jget(response, "backend");
        std::string backend_ =
            (bp && bp->is_string() && !bp->get<std::string>().empty()) ? bp->get<std::string>()
                                                                       : backend;
        const json* mp = jget(response, "model");
        std::string model =
            (mp && mp->is_string() && !mp->get<std::string>().empty()) ? mp->get<std::string>()
                                                                       : model_name;
        std::string detail = backend_ + ":" + model;
        if (dur && dur->is_number()) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f", dur->get<double>());
            std::cout << "  " << status_label("STATS", Colors::GRAY) << " MPC " << detail << sep
                      << b << "s" << Colors::RESET << "\n";
        } else {
            std::cout << "  " << status_label("STATS", Colors::GRAY) << " MPC " << detail
                      << " request completed." << Colors::RESET << "\n";
        }
        return;
    }
    if (backend == "ollama") {
        const json* td = jget(response, "total_duration");
        double duration = (td && td->is_number() ? td->get<double>() : 0.0) / 1e9;
        const json* pe = jget(response, "prompt_eval_count");
        long p_tokens = (pe && pe->is_number()) ? pe->get<long>() : 0;
        const json* ec = jget(response, "eval_count");
        long e_tokens = (ec && ec->is_number()) ? ec->get<long>() : 0;
        const json* ed = jget(response, "eval_duration");
        double eval_duration = (ed && ed->is_number() ? ed->get<double>() : 0.0) / 1e9;
        std::string tps;
        if (eval_duration > 0 && e_tokens) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.1f", (double)e_tokens / eval_duration);
            tps = sep + b + " tok/s";
        }
        if (duration > 0) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f", duration);
            std::cout << "  " << status_label("STATS", Colors::GRAY) << " " << b << "s" << sep
                      << "↑ " << p_tokens << sep << "↓ " << e_tokens << tps << Colors::RESET
                      << "\n";
        }
    } else {
        std::cout << "  " << status_label("STATS", Colors::GRAY) << " Request completed."
                  << Colors::RESET << "\n";
    }
}

}
