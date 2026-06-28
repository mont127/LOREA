#include "lorea.hpp"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <sstream>
#include <regex>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <future>
#include <thread>
#include <memory>
#include <iostream>
#include <filesystem>

namespace ocli {

std::string current_executable_path();

namespace {

namespace fs = std::filesystem;

const char* const G_DOT    = "\xc2\xb7";
const char* const G_DASH   = "\xe2\x80\x94";
const char* const G_TRI    = "\xe2\x96\xb8";
const char* const G_CARET  = "\xe2\x9d\xaf";
const char* const G_PLAY   = "\xe2\x96\xb6";
const char* const G_CHECK  = "\xe2\x9c\x93\xef\xb8\x8e";
const char* const G_CIRCLE = "\xe2\x97\x8b";
const char* const G_SPARK  = "\xe2\x9c\xa6";

std::string py_str(const json& v) {
    if (v.is_string())  return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_null())    return "None";
    return v.dump();
}

bool py_truthy(const json& v) {
    if (v.is_null())             return false;
    if (v.is_boolean())          return v.get<bool>();
    if (v.is_string())           return !v.get<std::string>().empty();
    if (v.is_number_integer())   return v.get<long long>() != 0;
    if (v.is_number_unsigned())  return v.get<unsigned long long>() != 0;
    if (v.is_number_float())     return v.get<double>() != 0.0;
    if (v.is_array())            return !v.empty();
    if (v.is_object())           return !v.empty();
    return true;
}

json jget(const json& o, const std::string& k) {
    if (o.is_object()) {
        auto it = o.find(k);
        if (it != o.end()) return *it;
    }
    return json(nullptr);
}

bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string py_strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws((unsigned char)s[a])) ++a;
    while (b > a && is_ws((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return r;
}

std::string replace_all(const std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string r;
    size_t pos = 0, found;
    while ((found = s.find(from, pos)) != std::string::npos) {
        r.append(s, pos, found - pos);
        r += to;
        pos = found + from.size();
    }
    r.append(s, pos, std::string::npos);
    return r;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::optional<long long> py_int(const std::string& s) {
    std::string t = py_strip(s);
    if (t.empty()) return std::nullopt;
    size_t i = 0;
    if (t[i] == '+' || t[i] == '-') ++i;
    if (i >= t.size()) return std::nullopt;
    for (size_t k = i; k < t.size(); ++k) {
        if (t[k] < '0' || t[k] > '9') return std::nullopt;
    }
    try {
        return std::stoll(t);
    } catch (...) {
        return std::nullopt;
    }
}

bool is_digit_str(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string collapse_ws(const std::string& s) {
    static const std::regex re(R"(\s+)");
    return std::regex_replace(s, re, " ");
}

long py_round(double x) {
    return (long)std::nearbyint(x);
}

std::string rjust2(int n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%2d", n);
    return std::string(buf);
}

const std::string SPAWN_RESULT_PREFIX = "Spawned agent results:\n";

std::string sys_executable_hint() {
    const char* p = std::getenv("PYTHON");
    if (p && *p) return p;
    return "python3";
}

HttpResponse post_json(const std::string& url, const json& body, long timeout_ms) {
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.headers.push_back({"Content-Type", "application/json"});
    req.body = body.dump();
    req.timeout_ms = timeout_ms;
    req.follow_redirects = true;
    return http_perform(req);
}

}

std::vector<Task> LOREA::parse_plan_tasks(const std::string& plan) {
    std::vector<Task> tasks;
    std::set<std::string> seen;

    static const std::regex re_checkbox(R"(^[-*]\s*\[[ xX]?\]\s*(.+)$)");
    static const std::regex re_numbered(R"(^\d+[.)]\s+(.+)$)");

    static const std::regex re_bullet(R"(^(?:[-*]|\xe2\x80\xa2)\s+(.+)$)");

    for (const std::string& line : split_lines(plan)) {
        std::string stripped = py_strip(line);
        if (stripped.empty()) continue;

        std::smatch m;
        bool matched = std::regex_match(stripped, m, re_checkbox);
        if (!matched) matched = std::regex_match(stripped, m, re_numbered);
        if (!matched) matched = std::regex_match(stripped, m, re_bullet);
        if (!matched) continue;

        std::string text = collapse_ws(py_strip(m[1].str()));
        std::string key = to_lower(text);
        if (!text.empty() && seen.find(key) == seen.end()) {
            seen.insert(key);
            tasks.push_back(Task{text, "todo"});
        }
    }

    if (tasks.empty()) {
        for (const std::string& line : split_lines(plan)) {
            std::string text = collapse_ws(py_strip(line));
            std::string key = to_lower(text);
            if (!text.empty() && seen.find(key) == seen.end()) {
                seen.insert(key);
                tasks.push_back(Task{text, "todo"});
            }
        }
    }
    return tasks;
}

void LOREA::print_tasks() {
    if (tasks.empty()) return;
    std::string indent = left_indent();
    int total = (int)tasks.size();
    int done = 0, doing = 0;
    for (const Task& t : tasks) {
        if (t.status == "done") ++done;
        else if (t.status == "doing") ++doing;
    }
    int pct = total ? (int)py_round(done * 100.0 / total) : 0;

    std::string sub = std::to_string(done) + "/" + std::to_string(total) + " done " +
                      G_DOT + " " + std::to_string(pct) + "%";
    std::cout << "\n" << indent
              << frame_title("PLAN PROGRESS", Colors::TEAL, sub.c_str()) << "\n";

    std::string bar = progress_bar((double)done, (double)total, 22);
    std::cout << indent << "  " << bar << " " << Colors::DIM << Colors::GRAY
              << "(" << done << " done " << G_DOT << " " << doing << " active " << G_DOT
              << " " << (total - done - doing) << " todo)" << Colors::RESET << "\n";

    for (int i = 0; i < total; ++i) {
        const Task& task = tasks[i];
        const std::string& status = task.status;
        std::string icon, text;
        if (status == "done") {
            icon = std::string(Colors::GREEN) + Colors::BOLD + G_CHECK + Colors::RESET;
            text = std::string(Colors::DIM) + Colors::GRAY + task.text + Colors::RESET;
        } else if (status == "doing") {
            icon = std::string(Colors::AMBER) + Colors::BOLD + G_PLAY + Colors::RESET;
            text = std::string(Colors::WHITE) + Colors::BOLD + task.text + Colors::RESET;
        } else {
            icon = std::string(Colors::DIM) + Colors::GRAY + G_CIRCLE + Colors::RESET;
            text = std::string(Colors::SLATE) + task.text + Colors::RESET;
        }
        std::string number = std::string(Colors::DIM) + Colors::GRAY + rjust2(i + 1) + "." + Colors::RESET;
        std::cout << indent << "  " << icon << " " << number << " " << text << "\n";
    }
    std::cout << indent << frame_bottom(Colors::TEAL) << "\n\n";
}

std::string LOREA::create_plan(const std::string& plan) {
    try {
        tasks = parse_plan_tasks(plan);
        if (tasks.empty()) {
            return "Could not parse any steps from the plan. Provide a plan as a numbered or bulleted list, one discrete step per line.";
        }
        std::string indent = left_indent();
        std::string sub = std::to_string(tasks.size()) + " steps";
        std::cout << "\n" << indent
                  << frame_title("IMPLEMENTATION PLAN", Colors::GREEN, sub.c_str()) << "\n";
        print_tasks();

        if (non_interactive || !can_use_terminal_keys()) {
            log_ok("Plan created " + std::string(G_DOT) + " " + std::to_string(tasks.size()) + " steps");
            return "Plan created. Mark the first task 'doing' with update_task, then make real tool calls. Mark each task 'done' as you finish it.";
        }
        std::cout << indent << Colors::AMBER << Colors::BOLD << G_TRI << " Review the plan"
                  << Colors::RESET << " " << Colors::DIM << Colors::GRAY << G_DASH
                  << " press Enter to approve, or type changes to revise." << Colors::RESET << "\n";
        std::string feedback = py_strip(styled_input(
            indent + ACCENT + Colors::BOLD + G_CARET + Colors::RESET + " "));
        if (feedback.empty()) {
            log_ok("Plan approved " + std::string(G_DOT) + " " + std::to_string(tasks.size()) + " steps");
            return "Plan approved. Mark the first task 'doing' with update_task, then make real tool calls. Mark each task 'done' as you finish it.";
        }
        log_info("Plan revision requested");
        return "User feedback on the plan: " + feedback +
               ". Revise the plan with create_plan or address the feedback before proceeding.";
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "KeyboardInterrupt") throw;
        return std::string("Error: ") + e.what();
    }
}

std::string LOREA::update_task(const json& index, const std::string& status_in) {
    try {
        std::string status = to_lower(py_strip(status_in));
        if (status != "todo" && status != "doing" && status != "done") {
            return "Invalid status '" + status + "'. Use one of: todo, doing, done.";
        }
        if (tasks.empty()) {
            return "No active plan. Call create_plan first.";
        }
        std::string index_str = py_str(index);
        std::string digits;
        for (char c : index_str) {
            if (c >= '0' && c <= '9') digits += c;
        }
        if (digits.empty()) {
            return "Invalid task index: " + index_str;
        }
        long long idx = std::stoll(digits) - 1;
        if (!(idx >= 0 && idx < (long long)tasks.size())) {
            return "Invalid task index: " + index_str + " (plan has " +
                   std::to_string(tasks.size()) + " tasks).";
        }
        tasks[(size_t)idx].status = status;
        print_tasks();
        int done = 0;
        for (const Task& t : tasks) if (t.status == "done") ++done;
        int total = (int)tasks.size();
        if (status == "done" && done < total) {

            std::cout << left_indent() << Colors::GREEN << Colors::BOLD << G_SPARK << Colors::RESET
                      << " " << Colors::WHITE << "step " << (idx + 1) << " done" << Colors::RESET
                      << " " << Colors::DIM << Colors::GRAY << G_DOT << " " << done << "/" << total
                      << " " << G_DOT << " keep going" << Colors::RESET << "\n";
        }
        if (done == total) {
            std::string celeb_sub = "all " + std::to_string(total) + " steps finished";
            celebrate("PLAN COMPLETE", celeb_sub.c_str(), Colors::GREEN);
            return "Task " + std::to_string(idx + 1) + " marked " + status + ". All " +
                   std::to_string(total) +
                   " tasks are done — give the user a concise final summary of what changed and how to use it.";
        }
        return "Task " + std::to_string(idx + 1) + " marked " + status + ". Progress: " +
               std::to_string(done) + "/" + std::to_string(total) +
               " done. Continue with the next task.";
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "KeyboardInterrupt") throw;
        return std::string("Error: ") + e.what();
    }
}

json LOREA::spawn_agent_chat(const json& agent, const std::string& shared_context,
                             int timeout_seconds, int max_steps,
                             const std::string& tool_access) {

    json nv = jget(agent, "name");
    std::string name_base = py_truthy(nv) ? py_str(nv) : "agent";
    std::string name = utf8_substr(py_strip(name_base), 0, 80);
    if (name.empty()) name = "agent";

    json tv = jget(agent, "task");
    std::string task_src;
    if (py_truthy(tv)) task_src = py_str(tv);
    else {
        json pv = jget(agent, "prompt");
        task_src = py_truthy(pv) ? py_str(pv) : "";
    }
    std::string task = py_strip(task_src);

    json cv = jget(agent, "context");
    std::string agent_context = py_strip(py_truthy(cv) ? py_str(cv) : "");

    if (task.empty()) {
        return json{{"name", name}, {"status", "error"}, {"response", "Missing agent task."}};
    }

    json payload = {
        {"name", name},
        {"task", task},
        {"context", agent_context},
        {"shared_context", shared_context},
        {"model_name", model_name},
        {"backend", backend},
        {"url", url},
        {"tool_access", tool_access},
        {"max_steps", max_steps},
    };

    std::vector<std::string> cmd = {current_executable_path(), "--spawn-agent-worker", "--skip-install"};

    try {
        ProcResult completed = run_subprocess(cmd, payload.dump(),
                                              (double)(timeout_seconds + 30), false);
        if (completed.timed_out) {
            return json{{"name", name}, {"status", "timeout"},
                        {"response", "Timed out after " + std::to_string(timeout_seconds) + "s."}};
        }

        bool decoded = true;
        json data;
        try {
            data = json::parse(completed.out);
        } catch (const json::parse_error&) {
            decoded = false;
        }
        if (decoded) {
            data["name"] = name;
            return data;
        }
        if (completed.exit_code != 0) {
            std::string body = !completed.err.empty()
                                   ? completed.err
                                   : (!completed.out.empty()
                                          ? completed.out
                                          : ("Worker exited with code " + std::to_string(completed.exit_code)));
            return json{{"name", name}, {"status", "error"},
                        {"response", truncate_output(body, 4000)}};
        }
        return json{{"name", name}, {"status", "error"},
                    {"response", "Worker completed without JSON output."}};
    } catch (const json::parse_error& e) {
        return json{{"name", name}, {"status", "error"},
                    {"response", std::string("Worker returned invalid JSON: ") + e.what()}};
    } catch (const std::exception& e) {
        return json{{"name", name}, {"status", "error"}, {"response", std::string(e.what())}};
    }
}

std::string LOREA::spawn_agents(const json& agents_in, const std::string& shared_context,
                                int timeout_seconds, int max_steps,
                                const std::string& tool_access_in) {
    try {
        json agents = agents_in;
        if (agents.is_string()) {
            json one = json{{"name", "agent_1"}, {"task", agents.get<std::string>()}};
            agents = json::array();
            agents.push_back(one);
        } else if (agents.is_object()) {
            if (agents.contains("agents")) {
                agents = agents.at("agents");
            } else {
                json one = agents;
                agents = json::array();
                agents.push_back(one);
            }
        }
        if (!agents.is_array() || agents.empty()) {
            return "Error: agents must be a non-empty list of task objects.";
        }

        std::vector<json> normalized;
        int count4 = 0;
        for (const json& agent : agents) {
            if (count4 >= 4) break;
            int index = count4 + 1;
            if (agent.is_string()) {
                normalized.push_back(json{{"name", "agent_" + std::to_string(index)},
                                          {"task", agent.get<std::string>()}});
            } else if (agent.is_object()) {
                json nv = jget(agent, "name");
                std::string name = py_truthy(nv) ? py_str(nv) : ("agent_" + std::to_string(index));
                json tv = jget(agent, "task");
                std::string task;
                if (py_truthy(tv)) task = py_str(tv);
                else {
                    json pv = jget(agent, "prompt");
                    task = py_truthy(pv) ? py_str(pv) : "";
                }
                json cv = jget(agent, "context");
                std::string ctx = py_truthy(cv) ? py_str(cv) : "";
                normalized.push_back(json{{"name", name}, {"task", task}, {"context", ctx}});
            }
            ++count4;
        }
        if (normalized.empty()) {
            return "Error: no valid agent tasks were provided.";
        }

        int timeout_seconds_c = (timeout_seconds != 0) ? timeout_seconds : 120;
        timeout_seconds_c = std::max(10, std::min(timeout_seconds_c, 600));
        int max_steps_c = (max_steps != 0) ? max_steps : 3;
        max_steps_c = std::max(1, std::min(max_steps_c, 8));
        std::string tool_access = (tool_access_in == "read_only" || tool_access_in == "full")
                                      ? tool_access_in : "read_only";

        ensure_local_server();
        log_tool("SPAWN_AGENTS: " + std::to_string(normalized.size()) + " agents " +
                 G_DOT + " " + tool_access);

        size_t n = normalized.size();
        std::vector<json> results(n, json(nullptr));

        std::vector<std::future<json>> futures;
        futures.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            auto prom = std::make_shared<std::promise<json>>();
            futures.push_back(prom->get_future());
            json agent_copy = normalized[i];
            std::string wname = py_str(normalized[i].at("name"));
            std::string sc = shared_context;
            std::thread([this, prom, agent_copy, sc, timeout_seconds_c, max_steps_c, tool_access, wname]() {
                json r;
                try {
                    r = this->spawn_agent_chat(agent_copy, sc, timeout_seconds_c, max_steps_c, tool_access);
                } catch (const std::exception& e) {
                    r = json{{"name", wname}, {"status", "error"}, {"response", std::string(e.what())}};
                }
                prom->set_value(r);
            }).detach();
        }

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_seconds_c + 5);
        for (size_t i = 0; i < n; ++i) {
            if (futures[i].wait_until(deadline) == std::future_status::ready) {
                try {
                    results[i] = futures[i].get();
                } catch (const std::exception& e) {
                    results[i] = json{{"name", py_str(normalized[i].at("name"))},
                                      {"status", "error"}, {"response", std::string(e.what())}};
                }
            }
        }

        for (size_t i = 0; i < n; ++i) {
            if (results[i].is_null()) {
                results[i] = json{{"name", py_str(normalized[i].at("name"))},
                                  {"status", "timeout"},
                                  {"response", "Timed out after " + std::to_string(timeout_seconds_c) + "s."}};
            }
        }

        std::vector<std::string> summary_lines;
        for (const json& result : results) {
            summary_lines.push_back(
                std::string(Colors::CYAN) + py_str(result.at("name")) + Colors::RESET + " " +
                Colors::GRAY + "(" + py_str(result.at("status")) + ")" + Colors::RESET + ": " +
                truncate_output(py_str(result.at("response")), 500));
        }
        print_panel("SPAWNED AGENTS", summary_lines, Colors::CYAN);

        json arr = results;
        return SPAWN_RESULT_PREFIX + arr.dump(2);
    } catch (const std::exception& e) {
        if (std::string(e.what()) == "KeyboardInterrupt") throw;
        return std::string("Error: ") + e.what();
    }
}

AgentOptions LOREA::parse_agent_args(const std::string& arg_text) {
    AgentOptions options;
    options.count = 3;
    options.timeout_seconds = 180;
    options.max_steps = 3;
    options.tool_access = "read_only";
    options.goal = "";

    if (arg_text.empty()) {
        return options;
    }

    std::vector<std::string> parts;
    try {
        parts = shlex_split(arg_text);
    } catch (const ShlexError& e) {
        log_info(std::string("Could not parse /agent arguments: ") + e.what());
        options.goal = py_strip(arg_text);
        return options;
    }

    std::vector<std::string> goal_parts;
    size_t i = 0;
    while (i < parts.size()) {
        const std::string& part = parts[i];
        std::string lowered = to_lower(part);

        if (lowered == "--full" || lowered == "--write") {
            options.tool_access = "full";
            i += 1;
            continue;
        }
        if (lowered == "--read-only" || lowered == "--readonly" || lowered == "--ro") {
            options.tool_access = "read_only";
            i += 1;
            continue;
        }
        if (lowered == "--count" || lowered == "-n" || lowered == "--agents") {
            if (i + 1 < parts.size()) {
                auto v = py_int(parts[i + 1]);
                if (v) options.count = (int)*v;
                else log_info("Invalid agent count: " + parts[i + 1]);
                i += 2;
                continue;
            }
        }
        if (lowered.rfind("--count=", 0) == 0 || lowered.rfind("--agents=", 0) == 0) {
            std::string val = part.substr(part.find('=') + 1);
            auto v = py_int(val);
            if (v) options.count = (int)*v;
            else log_info("Invalid agent count: " + part);
            i += 1;
            continue;
        }
        if (lowered == "--timeout" || lowered == "-t") {
            if (i + 1 < parts.size()) {
                auto v = py_int(parts[i + 1]);
                if (v) options.timeout_seconds = (int)*v;
                else log_info("Invalid timeout: " + parts[i + 1]);
                i += 2;
                continue;
            }
        }
        if (lowered.rfind("--timeout=", 0) == 0) {
            std::string val = part.substr(part.find('=') + 1);
            auto v = py_int(val);
            if (v) options.timeout_seconds = (int)*v;
            else log_info("Invalid timeout: " + part);
            i += 1;
            continue;
        }
        if (lowered == "--steps" || lowered == "-s") {
            if (i + 1 < parts.size()) {
                auto v = py_int(parts[i + 1]);
                if (v) options.max_steps = (int)*v;
                else log_info("Invalid step count: " + parts[i + 1]);
                i += 2;
                continue;
            }
        }
        if (lowered.rfind("--steps=", 0) == 0) {
            std::string val = part.substr(part.find('=') + 1);
            auto v = py_int(val);
            if (v) options.max_steps = (int)*v;
            else log_info("Invalid step count: " + part);
            i += 1;
            continue;
        }
        if (goal_parts.empty() && is_digit_str(part)) {
            auto v = py_int(part);
            if (v) options.count = (int)*v;
            i += 1;
            continue;
        }
        goal_parts.push_back(part);
        i += 1;
    }

    std::string joined;
    for (size_t k = 0; k < goal_parts.size(); ++k) {
        if (k) joined += " ";
        joined += goal_parts[k];
    }
    options.goal = py_strip(joined);
    options.count = std::max(1, std::min(options.count, 4));
    options.timeout_seconds = std::max(10, std::min(options.timeout_seconds, 600));
    options.max_steps = std::max(1, std::min(options.max_steps, 8));
    return options;
}

std::vector<json> LOREA::build_agent_team(const std::string& goal, int count) {
    static const std::vector<std::pair<std::string, std::string>> templates = {
        {"scout", "Inspect the workspace and current context. Identify relevant files, constraints, and the safest path forward. Do not make edits unless explicitly allowed by tool access and clearly necessary."},
        {"builder", "Work out the concrete implementation or solution path. If tool access is full and edits are necessary, keep changes tightly scoped and report exact files touched."},
        {"reviewer", "Review the goal and any likely implementation for bugs, missing tests, edge cases, and user-facing regressions. Focus on actionable risks."},
        {"tester", "Design or run verification steps appropriate to the goal. Report what passed, what could not be checked, and the remaining risk."},
    };
    std::vector<json> agents;
    int n = (int)templates.size();
    if (count < n) n = count;
    if (n < 0) n = 0;
    for (int idx = 0; idx < n; ++idx) {
        const std::string& name = templates[idx].first;
        const std::string& role_task = templates[idx].second;
        agents.push_back(json{
            {"name", name},
            {"task", role_task + "\n\nShared goal:\n" + goal},
            {"context", "Role: " + name + ". Coordinate through the final coordinator by returning concise findings and concrete next steps."},
        });
    }
    return agents;
}

std::string LOREA::agent_completion_once(const std::vector<Message>& messages,
                                         double temperature, int max_tokens) {
    json body_messages = messages;

    if (backend == "ollama") {

        json body = {
            {"model", model_name},
            {"messages", body_messages},
            {"stream", false},
            {"options", {{"temperature", temperature}, {"num_predict", max_tokens}}},
        };
        HttpResponse resp = post_json(url + "/api/chat", body, 120000);
        if (resp.network_error) {
            throw std::runtime_error(resp.error.empty() ? "Connection error" : resp.error);
        }
        if (resp.status >= 400 || resp.status == 0) {
            throw HttpStatusError(resp.status, std::to_string(resp.status) + " from /api/chat");
        }
        json data = json::parse(resp.body);
        return py_strip(data.at("message").at("content").get<std::string>());
    }

    if (backend == "airllm") {

        std::string exe = sys_executable_hint();
        throw std::runtime_error("AirLLM or PyTorch is not installed for " + exe +
                                 ". Run: " + exe + " -m pip install airllm torch");
    }

    json body = {
        {"model", model_name},
        {"messages", body_messages},
        {"temperature", temperature},
        {"max_tokens", max_tokens},
    };
    HttpResponse resp = post_json(url + "/v1/chat/completions", body, 120000);
    if (resp.network_error) {
        throw std::runtime_error(resp.error.empty() ? "Connection error" : resp.error);
    }
    if (resp.status >= 400 || resp.status == 0) {

        throw HttpStatusError(resp.status, std::to_string(resp.status) +
                              " from /v1/chat/completions");
    }
    json data = json::parse(resp.body);
    return py_strip(data.at("choices").at(0).at("message").at("content").get<std::string>());
}

std::string LOREA::coordinate_agent_results(const std::string& goal,
                                            const std::vector<json>& results) {
    std::vector<json> compact_results;
    for (const json& result : results) {
        json response_val = jget(result, "response");
        std::string response = py_truthy(response_val) ? py_str(response_val) : "";

        compact_results.push_back(json{
            {"name", jget(result, "name")},
            {"status", jget(result, "status")},
            {"tool_access", jget(result, "tool_access")},
            {"response", truncate_output(response, 1800)},
        });
    }

    json compact_arr = compact_results;
    std::string prompt =
        "You are the coordinator for a parallel multi-agent LOREA run. "
        "Merge the worker results into one concise, practical answer for the user. "
        "Prefer concrete findings, files touched, commands run, risks, and next steps. "
        "Do not invent work that the workers did not report.\n\n"
        "User goal:\n" + goal + "\n\n"
        "Worker results JSON:\n" + compact_arr.dump(2);

    try {
        std::vector<Message> msgs = {
            json{{"role", "system"}, {"content", "You coordinate multiple coding agents and produce a single actionable final answer."}},
            json{{"role", "user"}, {"content", prompt}},
        };
        return agent_completion_once(msgs, 0.2, 1800);
    } catch (const std::exception& e) {
        log_info(std::string("Coordinator pass failed: ") + e.what());
        std::vector<std::string> fallback;
        fallback.push_back("Coordinator pass failed; raw worker results:");
        for (const json& result : compact_results) {
            fallback.push_back("\n[" + py_str(result.at("name")) + " " + G_DOT + " " +
                               py_str(result.at("status")) + "]\n" + py_str(result.at("response")));
        }
        std::string out;
        for (size_t k = 0; k < fallback.size(); ++k) {
            if (k) out += "\n";
            out += fallback[k];
        }
        return out;
    }
}

void LOREA::agent_command(const std::string& arg_text) {
    AgentOptions opts = parse_agent_args(arg_text);

    std::optional<std::string> goalv;
    if (!opts.goal.empty()) goalv = opts.goal;
    else goalv = prompt_value("Agent goal");
    if (!goalv || goalv->empty()) {
        log_info("Agent run canceled.");
        return;
    }
    std::string goal = *goalv;

    if (opts.goal.empty()) {
        std::optional<std::string> count_value = prompt_value("Agents", std::to_string(opts.count));

        std::optional<long long> parsed;
        if (count_value) parsed = py_int(*count_value);
        if (parsed) {
            opts.count = std::max(1, std::min((int)*parsed, 4));
        } else {
            std::string shown = count_value ? *count_value : "None";
            log_info("Invalid agent count '" + shown + "', using " + std::to_string(opts.count) + ".");
        }

        std::optional<std::string> av = prompt_value("Tool access", opts.tool_access);
        std::string access_value = replace_all(to_lower(py_strip(av ? *av : "")), "-", "_");
        if (access_value == "full" || access_value == "write") {
            opts.tool_access = "full";
        } else if (access_value == "read_only" || access_value == "readonly" || access_value == "ro") {
            opts.tool_access = "read_only";
        }
    }

    if (opts.tool_access == "full") {
        log_info("Full-access agents may run commands or edit files. Keep tasks scoped to avoid overlapping changes.");
    }

    std::vector<json> agents = build_agent_team(goal, opts.count);

    std::error_code ec;
    std::string cwd = fs::current_path(ec).string();
    if (ec) cwd = "";
    std::string shared_context =
        "Workspace: " + cwd + "\n"
        "Backend: " + backend + "\n"
        "Model: " + model_name + "\n"
        "Tool access: " + opts.tool_access + "\n"
        "Workers should avoid duplicating each other and return concise results for a final coordinator pass.";

    json agents_json = agents;
    std::string result_text = spawn_agents(agents_json, shared_context,
                                           opts.timeout_seconds, opts.max_steps, opts.tool_access);

    std::vector<json> results;
    if (result_text.rfind(SPAWN_RESULT_PREFIX, 0) == 0) {

        size_t nl = result_text.find('\n');
        std::string rest = (nl == std::string::npos) ? std::string() : result_text.substr(nl + 1);
        try {
            json parsed = json::parse(rest);
            if (parsed.is_array()) {
                for (const json& r : parsed) results.push_back(r);
            }
        } catch (const json::parse_error& e) {
            log_info(std::string("Could not parse spawned agent results: ") + e.what());
        }
    }

    if (results.empty()) {
        print_panel("agent coordinator", {truncate_output(result_text, 2000)}, Colors::CYAN);
        return;
    }

    log_tool("AGENT_COORDINATOR");
    std::string final = coordinate_agent_results(goal, results);
    std::cout << "\n" << left_indent() << frame_title("agent coordinator", Colors::CYAN) << "\n";
    std::cout << render_text(final) << "\n";
    std::cout << left_indent() << frame_bottom(Colors::CYAN) << "\n\n";
    messages.push_back(json{{"role", "user"}, {"content", "/agent " + goal}});
    messages.push_back(json{{"role", "assistant"}, {"content", final}});
}

}
