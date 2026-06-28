#include "lorea.hpp"
#include "terminal.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ocli {

namespace {

class CoutSink {
public:
    explicit CoutSink(std::ostream& target)
        : old_(std::cout.rdbuf(target.rdbuf())) {}
    ~CoutSink() { std::cout.rdbuf(old_); }
    CoutSink(const CoutSink&) = delete;
    CoutSink& operator=(const CoutSink&) = delete;
private:
    std::streambuf* old_;
};

bool jtruthy(const json& v) {
    if (v.is_null())            return false;
    if (v.is_boolean())        return v.get<bool>();
    if (v.is_string())         return !v.get<std::string>().empty();
    if (v.is_number_integer()) return v.get<long long>() != 0;
    if (v.is_number_unsigned())return v.get<unsigned long long>() != 0;
    if (v.is_number_float())   return v.get<double>() != 0.0;
    if (v.is_array() || v.is_object()) return !v.empty();
    return true;
}

std::string jstr(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null())   return "";
    return v.dump();
}

json jget(const json& obj, const char* key) {
    if (obj.is_object() && obj.contains(key)) return obj.at(key);
    return json(nullptr);
}

std::string strip(const std::string& s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\f' || c == '\v';
    };
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_upper_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::string to_lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}

int run_spawn_agent_worker() {
    json request;
    try {
        request = json::parse(std::cin);
    } catch (const std::exception& e) {
        json err = {
            {"status",   "error"},
            {"response", std::string("Invalid worker payload: ") + e.what()},
            {"log",      ""},
        };
        std::cout << err.dump() << std::endl;
        return 1;
    }

    json vname = jget(request, "name");
    std::string name = jtruthy(vname) ? jstr(vname) : std::string("agent");
    name = strip(name);
    name = utf8_substr(name, 0, 80);
    if (name.empty()) name = "agent";

    json vtask = jget(request, "task");
    std::string task = strip(jtruthy(vtask) ? jstr(vtask) : std::string(""));

    json vctx = jget(request, "context");
    std::string context = strip(jtruthy(vctx) ? jstr(vctx) : std::string(""));

    json vshared = jget(request, "shared_context");
    std::string shared_context = strip(jtruthy(vshared) ? jstr(vshared) : std::string(""));

    json vbackend = jget(request, "backend");
    std::string backend = jtruthy(vbackend) ? jstr(vbackend) : std::string("ollama");

    json vmodel = jget(request, "model_name");
    std::string model_name;
    if (jtruthy(vmodel)) {
        model_name = jstr(vmodel);
    } else if (BACKEND_DEFAULT_MODELS.count(backend)) {
        model_name = BACKEND_DEFAULT_MODELS.at(backend);
    } else {
        model_name = BACKEND_DEFAULT_MODELS.at("ollama");
    }

    json vurl = jget(request, "url");
    std::optional<std::string> url;
    if (!vurl.is_null()) url = jstr(vurl);

    json vta = jget(request, "tool_access");
    std::string tool_access = "read_only";
    if (vta.is_string()) {
        std::string s = vta.get<std::string>();
        if (s == "read_only" || s == "full") tool_access = s;
    }

    int max_steps = 3;
    try {
        json vms = jget(request, "max_steps");
        long long iv;
        if (!jtruthy(vms)) {
            iv = 3;
        } else if (vms.is_boolean()) {
            iv = vms.get<bool>() ? 1 : 0;
        } else if (vms.is_number()) {
            iv = static_cast<long long>(vms.get<double>());
        } else if (vms.is_string()) {
            iv = std::stoll(strip(vms.get<std::string>()));
        } else {
            throw std::runtime_error("max_steps not int-coercible");
        }
        if (iv < 1) iv = 1;
        if (iv > 8) iv = 8;
        max_steps = static_cast<int>(iv);
    } catch (...) {
        max_steps = 3;
    }

    std::ostringstream log_buffer;
    std::string status = "ok";
    std::string response_text;

    try {
        std::unique_ptr<LOREA> agent;
        {
            CoutSink sink(log_buffer);

            agent = std::make_unique<LOREA>(model_name, true, backend, url);
            agent->tool_access = tool_access;
            agent->allow_spawn_agents = false;
            agent->non_interactive = true;

            std::string sys0;
            if (!agent->messages.empty() && agent->messages[0].is_object() &&
                agent->messages[0].contains("content") &&
                agent->messages[0]["content"].is_string()) {
                sys0 = agent->messages[0]["content"].get<std::string>();
            }
            sys0 += std::string(
                " You are spawned worker agent `") + name + "`. You have your own tool loop and memory. "
                "You are not alone in the workspace; avoid broad edits and do not overwrite unrelated changes. "
                "If tool_access is read_only, inspect and report only. If tool_access is full, edit or run commands only when the task clearly requires it. "
                "Before making claims about the current workspace, inspect it with an available tool. "
                "Do not spawn more agents. Finish with concise findings, exact files touched if any, and remaining risks.";
            if (!agent->messages.empty()) agent->messages[0]["content"] = sys0;

            std::vector<std::string> user_parts;
            if (!shared_context.empty())
                user_parts.push_back("Shared context:\n" + shared_context);
            if (!context.empty())
                user_parts.push_back("Agent-specific context:\n" + context);
            user_parts.push_back("Task:\n" + task);
            user_parts.push_back("Tool access: " + tool_access);

            agent->last_user_goal = task;

            std::string joined;
            for (std::size_t i = 0; i < user_parts.size(); ++i) {
                if (i) joined += "\n\n";
                joined += user_parts[i];
            }
            agent->messages.push_back(json{{"role", "user"}, {"content", joined}});

            for (int step = 0; step < max_steps; ++step) {
                agent->compact_history();
                agent->process_chat();

                std::string last_content;
                for (auto it = agent->messages.rbegin(); it != agent->messages.rend(); ++it) {
                    if (!it->is_object()) continue;
                    if (it->value("role", std::string("")) != "assistant") continue;
                    json cv = it->contains("content") ? (*it)["content"] : json(nullptr);
                    if (jtruthy(cv)) { last_content = jstr(cv); break; }
                }

                if (to_upper_ascii(last_content).find("CONTINUE") == std::string::npos ||
                    step == max_steps - 1) {
                    break;
                }
                agent->messages.push_back(json{
                    {"role", "user"},
                    {"content", "Continue the assigned worker task. Use tools only if they materially advance the task; otherwise provide the final worker summary."}});
            }

            agent->cleanup();
        }

        for (auto it = agent->messages.rbegin(); it != agent->messages.rend(); ++it) {
            if (!it->is_object()) continue;
            if (it->value("role", std::string("")) != "assistant") continue;
            json cv = it->contains("content") ? (*it)["content"] : json(nullptr);
            std::string cs = strip(jstr(cv));
            if (cs.empty()) continue;
            bool has_tc = it->contains("tool_calls") && jtruthy((*it)["tool_calls"]);
            if (has_tc) continue;
            response_text = cs;
            break;
        }

        if (response_text.empty()) {
            status = "no_final";
            response_text = "Worker completed without a final assistant summary.";
        }
    } catch (const std::exception& e) {
        status = "error";
        response_text = e.what();
    }

    json out = {
        {"name",        name},
        {"status",      status},
        {"tool_access", tool_access},
        {"response",    truncate_output(response_text, 5000)},
        {"log",         truncate_output(clean_ansi(log_buffer.str()), 5000)},
    };
    std::cout << out.dump() << std::endl;

    return status != "error" ? 0 : 1;
}

int run_main(const std::vector<std::string>& argv) {
    const std::string prog = to_lower_ascii(std::string(CLI_COMMAND_NAME));
    static const std::vector<std::string> backend_choices = {
        "ollama", "llama-cpp", "mlx", "airllm", "openai", "anthropic"};

    const std::string choices_str =
        "ollama, llama-cpp, mlx, airllm, openai, anthropic";
    const std::string usage =
        "usage: " + prog + " [-h] [--model MODEL] [--auto]\n"
        "        [--backend {" + choices_str + "}]\n"
        "        [--url URL] [--skip-install]";

    auto fail = [&](const std::string& msg) -> void {
        std::cerr << usage << "\n" << prog << ": error: " << msg << "\n";
        std::exit(2);
    };

    std::string model;
    bool        auto_mode = false;
    std::string backend = "mlx";
    std::string url;
    bool        has_url = false;
    bool        skip_install = false;
    bool        spawn_worker = false;

    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];

        std::string key = a, inlineval;
        bool has_inline = false;
        if (a.rfind("--", 0) == 0) {
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                key = a.substr(0, eq);
                inlineval = a.substr(eq + 1);
                has_inline = true;
            }
        }

        auto need_value = [&](const std::string& optname) -> std::string {
            if (has_inline) return inlineval;
            if (i + 1 >= argv.size()) {
                fail("argument " + optname + ": expected one argument");
            }
            return argv[++i];
        };

        if (key == "-h" || key == "--help") {
            std::cout << usage << "\n";
            std::exit(0);
        } else if (key == "--model") {
            model = need_value("--model");
        } else if (key == "--auto") {
            auto_mode = true;
        } else if (key == "--backend") {
            backend = need_value("--backend");
            if (std::find(backend_choices.begin(), backend_choices.end(), backend) ==
                backend_choices.end()) {
                fail("argument --backend: invalid choice: '" + backend +
                     "' (choose from " + choices_str + ")");
            }
        } else if (key == "--url") {
            url = need_value("--url");
            has_url = true;
        } else if (key == "--skip-install") {
            skip_install = true;
        } else if (key == "--spawn-agent-worker") {
            spawn_worker = true;
        } else {
            fail("unrecognized arguments: " + a);
        }
    }

    if (skip_install) {
        ::setenv("LOREA_SKIP_INSTALL", "1", 1);
    }
    if (spawn_worker) {
        return run_spawn_agent_worker();
    }

    install_cli_launcher();

    install_resize_handler();

    std::string chosen_model = !model.empty()
        ? model
        : BACKEND_DEFAULT_MODELS.at(backend);

    std::optional<std::string> url_opt;
    if (has_url) url_opt = url;

    LOREA agent(chosen_model, auto_mode, backend, url_opt);
    agent.run();
    return 0;
}

}
