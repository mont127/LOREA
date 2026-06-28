#include "toolparse.hpp"
#include "secutil.hpp"
#include "ansi.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace ocli {

const std::set<std::string> NUMERIC_TOOL_ARGS = {"num_results", "index"};

const std::vector<std::string> KNOWN_TOOL_NAMES = {
    "run_cmd", "test_cmd", "read_file", "write_file", "list_files", "search_files",
    "find_files", "grep", "web_search", "read_url", "http_request", "git_status",
    "git_diff", "send_input", "download_mlx_model", "create_plan", "update_task",
    "spawn_agents",
};

const std::map<std::string, std::string> PY_REQUIRED_ARG = {
    {"run_cmd", "command"}, {"test_cmd", "command"}, {"send_input", "text"},
    {"read_file", "path"},  {"write_file", "path"},  {"list_files", "path"},
    {"grep", "pattern"},    {"search_files", "query"}, {"find_files", "query"},
    {"web_search", "query"}, {"read_url", "url"},     {"http_request", "url"},
    {"download_mlx_model", "repo_id"}, {"create_plan", "plan"}, {"update_task", "index"},
};

const std::set<std::string> ZERO_ARG_TOOLS = {"git_status", "git_diff"};

const std::map<std::string, std::string> TOOL_PRIMARY_ARG = {
    {"create_plan", "plan"}, {"run_cmd", "command"}, {"test_cmd", "command"},
    {"read_file", "path"},   {"write_file", "content"}, {"web_search", "query"},
    {"read_url", "url"},     {"grep", "pattern"},    {"list_files", "path"},
    {"search_files", "pattern"}, {"find_files", "pattern"}, {"http_request", "url"},
};

const std::regex COMPLETION_RE(
    R"RE(\b(done|completed|finished|all set|here'?s the|here is the|to summarize|in summary|summary:|the (?:code|file|implementation|task|app) is (?:now )?(?:complete|ready|done)|let me know|hope this helps|feel free|that'?s it|you can now|is ready|in conclusion|overall|to recap)\b)RE",
    std::regex::ECMAScript | std::regex::icase);

const std::regex CONTINUATION_PROMISE_RE(
    R"RE((?:let me|let'?s|i'?ll|i will|i'?m going to|i am going to|i need to|i should|i have to|i want to|i can|going to|next step is to|next,? i'?ll|first,? i'?ll|then i'?ll|now i'?ll|now let me|also,? i'?ll|let me also)\s+(?:to\s+|now\s+|also\s+|then\s+|first\s+|quickly\s+|go\s+ahead\s+and\s+)*(?:create|write|add|implement|run|test|fix|update|modify|edit|build|install|generate|define|refactor|delete|remove|rename|move|configure|set ?up|make|start|check|search|read|inspect|execute|look|examine|view|explore|open|find|list|investigate|analyze|review|scan|verify|gather|fetch|download|browse|search)\b)RE",
    std::regex::ECMAScript | std::regex::icase);

namespace {

inline bool is_py_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string py_strip(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_py_space(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_py_space(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string py_rstrip(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && is_py_space(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(0, e);
}

std::string strip_chars(const std::string& s, const std::string& chars) {
    size_t b = 0, e = s.size();
    while (b < e && chars.find(s[b]) != std::string::npos) ++b;
    while (e > b && chars.find(s[e - 1]) != std::string::npos) --e;
    return s.substr(b, e - b);
}

std::string to_lower_ascii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

bool contains_any(const std::string& hay, const std::vector<std::string>& needles) {
    for (const auto& nd : needles) {
        if (hay.find(nd) != std::string::npos) return true;
    }
    return false;
}

std::string py_str(const json& v) {
    if (v.is_string())          return v.get<std::string>();
    if (v.is_boolean())         return v.get<bool>() ? "True" : "False";
    if (v.is_null())            return "None";
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        std::string d = v.dump();
        return d;
    }
    return v.dump();
}

std::optional<long long> py_int(const std::string& s) {
    if (s.empty()) return std::nullopt;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = (s[i] == '-');
        ++i;
    }
    if (i >= s.size()) return std::nullopt;
    std::string digits;
    bool last_was_digit = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '_') {
            if (!last_was_digit) return std::nullopt;
            last_was_digit = false;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        digits.push_back(c);
        last_was_digit = true;
    }
    if (!last_was_digit || digits.empty()) return std::nullopt;
    try {
        long long v = std::stoll(digits);
        return neg ? -v : v;
    } catch (...) {
        return std::nullopt;
    }
}

std::string re_escape(const std::string& s) {
    static const std::string special = "()[]{}?*+-|^$\\.&~# \t\n\r\v\f";
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (special.find(c) != std::string::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}

std::vector<std::string> extract_json_objects(const std::string& text) {
    std::vector<std::string> objects;
    long start = -1;
    int depth = 0;
    bool in_string = false;
    bool escape = false;

    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            if (depth == 0) start = static_cast<long>(i);
            depth += 1;
        } else if (ch == '}') {
            if (depth > 0) {
                depth -= 1;
                if (depth == 0 && start != -1) {
                    objects.push_back(text.substr(static_cast<size_t>(start),
                                                  i + 1 - static_cast<size_t>(start)));
                    start = -1;
                }
            }
        }
    }
    return objects;
}

json coerce_arg(const std::string& key, const json& value) {
    if (NUMERIC_TOOL_ARGS.count(key)) {

        std::string s = py_strip(py_str(value));
        std::optional<long long> parsed = py_int(s);
        if (parsed) return json(*parsed);
        return value;
    }
    return value;
}

std::vector<json> parse_xml_tool_calls(const std::string& text) {
    std::vector<json> calls;

    static const std::regex function_pattern(
        R"RE(<function\s*=\s*([A-Za-z_][\w-]*)\s*>([\s\S]*?)(?:</function>|$))RE");

    static const std::regex param_pattern(
        R"RE(<parameter\s*=\s*([A-Za-z_][\w-]*)\s*>([\s\S]*?)(?:</parameter>|<parameter\s*=|</function>|$))RE");

    for (std::sregex_iterator fn(text.begin(), text.end(), function_pattern), fend;
         fn != fend; ++fn) {
        std::string name = py_strip((*fn)[1].str());
        std::string body = (*fn)[2].str();
        json args = json::object();
        for (std::sregex_iterator p(body.begin(), body.end(), param_pattern), pend;
             p != pend; ++p) {
            std::string key = py_strip((*p)[1].str());
            std::string value = (*p)[2].str();

            if (!value.empty() && value.front() == '\n') value.erase(value.begin());
            if (!value.empty() && value.back() == '\n') value.pop_back();
            value = py_strip(value);
            if (!value.empty()) args[key] = coerce_arg(key, json(value));
        }
        if (!name.empty() && !args.empty()) {
            json call;
            call["function"]["name"] = name;
            call["function"]["arguments"] = args;
            calls.push_back(std::move(call));
        }
    }
    return calls;
}

std::vector<json> parse_named_xml_tool_calls(const std::string& text,
                                             const std::vector<std::string>* known_tools) {
    std::vector<json> calls;

    std::vector<std::string> names;
    if (known_tools) {
        names = *known_tools;
    } else {

        static const std::vector<std::string> primary_order = {
            "create_plan", "run_cmd", "test_cmd", "read_file", "write_file", "web_search",
            "read_url", "grep", "list_files", "search_files", "find_files", "http_request",
        };
        names = primary_order;
    }

    std::stable_sort(names.begin(), names.end(),
                     [](const std::string& a, const std::string& b) {
                         return a.size() > b.size();
                     });

    static const std::regex child_pattern(
        R"RE(<([A-Za-z_][\w-]*)\s*>([\s\S]*?)(?:</\1\s*>|$))RE");

    for (const std::string& name : names) {
        std::string esc = re_escape(name);

        std::regex block_pattern("<" + esc + R"RE(\s*>([\s\S]*?)</)RE" + esc + R"RE(\s*>)RE");
        for (std::sregex_iterator b(text.begin(), text.end(), block_pattern), bend;
             b != bend; ++b) {
            std::string body = (*b)[1].str();
            json args = json::object();
            for (std::sregex_iterator c(body.begin(), body.end(), child_pattern), cend;
                 c != cend; ++c) {
                std::string key = py_strip((*c)[1].str());
                std::string value = (*c)[2].str();
                if (!value.empty() && value.front() == '\n') value.erase(value.begin());
                if (!value.empty() && value.back() == '\n') value.pop_back();
                value = py_strip(value);
                if (!value.empty()) args[key] = coerce_arg(key, json(value));
            }
            if (args.empty()) {
                auto it = TOOL_PRIMARY_ARG.find(name);
                std::string inner = py_strip(body);
                if (it != TOOL_PRIMARY_ARG.end() && !it->second.empty() && !inner.empty()) {
                    args[it->second] = coerce_arg(it->second, json(inner));
                }
            }
            if (!args.empty()) {
                json call;
                call["function"]["name"] = name;
                call["function"]["arguments"] = args;
                calls.push_back(std::move(call));
            }
        }
    }
    return calls;
}

long balanced_paren_end(const std::string& s, size_t open_idx) {
    int depth = 0;
    for (size_t i = open_idx; i < s.size(); ++i) {
        if (s[i] == '(') {
            depth += 1;
        } else if (s[i] == ')') {
            depth -= 1;
            if (depth == 0) return static_cast<long>(i);
        }
    }
    return -1;
}

std::optional<json> coerce_call_args(const std::string& inner, const std::string& name) {
    std::string s = py_strip(inner);
    if (s.empty()) {
        if (ZERO_ARG_TOOLS.count(name)) return json::object();
        return std::nullopt;
    }

    size_t brace = s.find('{');
    if (brace != std::string::npos) {
        size_t end = s.rfind('}');
        if (end != std::string::npos && end > brace) {
            try {
                json obj = json::parse(s.substr(brace, end + 1 - brace));
                if (obj.is_object()) {
                    if (obj.contains("arguments") && obj["arguments"].is_object()) {
                        return obj["arguments"];
                    }
                    json result = json::object();
                    for (auto it = obj.begin(); it != obj.end(); ++it) {
                        if (it.key() != "name") result[it.key()] = it.value();
                    }
                    return result;
                }
            } catch (...) {

            }
        }
    }

    json kw = json::object();
    static const std::regex dq_kw(R"RE((\w+)\s*=\s*"((?:\\.|[^"\\])*)")RE");
    static const std::regex sq_kw(R"RE((\w+)\s*=\s*'((?:\\.|[^'\\])*)')RE");
    for (std::sregex_iterator it(s.begin(), s.end(), dq_kw), e; it != e; ++it) {
        kw[(*it)[1].str()] = (*it)[2].str();
    }
    for (std::sregex_iterator it(s.begin(), s.end(), sq_kw), e; it != e; ++it) {
        std::string k = (*it)[1].str();
        if (!kw.contains(k)) kw[k] = (*it)[2].str();
    }
    if (!kw.empty()) return kw;

    if (s[0] == '"' || s[0] == '\'') {
        std::string pos = strip_chars(s, "\"'");
        auto rit = PY_REQUIRED_ARG.find(name);
        if (rit != PY_REQUIRED_ARG.end() && !rit->second.empty() && !pos.empty()) {
            json r = json::object();
            r[rit->second] = pos;
            return r;
        }
    }
    return std::nullopt;
}

std::vector<json> parse_pythonic_tool_calls(const std::string& text,
                                            const std::vector<std::string>& known_tools) {
    std::vector<json> calls;
    static const std::regex call_re(R"RE(\b([a-z_]+)\s*\()RE");
    for (std::sregex_iterator it(text.begin(), text.end(), call_re), end; it != end; ++it) {
        const std::smatch& m = *it;
        std::string name = m[1].str();
        if (std::find(known_tools.begin(), known_tools.end(), name) == known_tools.end()) {
            continue;
        }
        size_t mend = static_cast<size_t>(m.position(0) + m.length(0));
        long endp = balanced_paren_end(text, mend - 1);
        if (endp < 0) continue;
        std::optional<json> args =
            coerce_call_args(text.substr(mend, static_cast<size_t>(endp) - mend), name);
        if (!args) continue;
        auto rit = PY_REQUIRED_ARG.find(name);
        if (rit != PY_REQUIRED_ARG.end()) {
            const std::string& req = rit->second;
            if (!args->contains(req) && !ZERO_ARG_TOOLS.count(name)) continue;
        }
        json call;
        call["function"]["name"] = name;
        call["function"]["arguments"] = *args;
        calls.push_back(std::move(call));
    }
    return calls;
}

std::optional<json> recover_tool_call_from_text(const std::string& text) {
    std::vector<json> xml_calls = parse_xml_tool_calls(text);
    if (!xml_calls.empty()) return xml_calls[0];
    std::vector<json> named_calls = parse_named_xml_tool_calls(text);
    if (!named_calls.empty()) return named_calls[0];
    std::vector<json> pythonic = parse_pythonic_tool_calls(text);
    if (!pythonic.empty()) return pythonic[0];

    static const std::regex name_re(
        R"RE(["']?name["']?\s*:\s*["']?([A-Za-z_][\w-]*)["']?)RE");
    std::smatch nm;
    bool name_matched = std::regex_search(text, nm, name_re);
    std::string name_g1 = name_matched ? nm[1].str() : std::string();

    json args = json::object();
    static const std::vector<std::string> arg_keys = {
        "command", "path", "content", "plan", "query", "text",
        "url", "pattern", "repo_id", "index", "status", "key", "value",
    };
    for (const std::string& key : arg_keys) {

        std::string pat = std::string(R"RE(["']?)RE") + key +
            R"RE(["']?\s*:\s*("((?:\\.|[^"\\])*)"|'((?:\\.|[^'\\])*)'|([^,}\n]+)))RE";
        std::regex kre(pat);
        std::smatch km;
        if (std::regex_search(text, km, kre)) {

            std::string raw_value;
            if (km[2].matched)      raw_value = km[2].str();
            else if (km[3].matched) raw_value = km[3].str();
            else if (km[4].matched) raw_value = km[4].str();
            else                    raw_value = "";
            raw_value = py_strip(raw_value);

            try {
                args[key] = unicode_escape_decode(raw_value);
            } catch (...) {
                args[key] = raw_value;
            }
        }
    }
    static const std::regex num_re(R"RE(["']?num_results["']?\s*:\s*(\d+))RE");
    std::smatch num_m;
    if (std::regex_search(text, num_m, num_re)) {
        std::optional<long long> n = py_int(num_m[1].str());
        if (n) args["num_results"] = json(*n);
        else   args["num_results"] = json(num_m[1].str());
    }

    if (args.empty()) return std::nullopt;

    std::string name;
    if (name_matched) {
        name = name_g1;
    } else if (args.contains("command")) {

        name = "run_cmd";
    } else {
        return std::nullopt;
    }

    static const std::map<std::string, std::string> REQUIRED_ARG = {
        {"run_cmd", "command"}, {"test_cmd", "command"},
        {"read_file", "path"},  {"write_file", "path"}, {"list_files", "path"},
        {"grep", "pattern"},    {"search_files", "query"}, {"find_files", "name"},
        {"web_search", "query"}, {"read_url", "url"},
    };
    auto rit = REQUIRED_ARG.find(name);
    if (rit != REQUIRED_ARG.end() && !args.contains(rit->second)) {
        return std::nullopt;
    }
    json call;
    call["function"]["name"] = name;
    call["function"]["arguments"] = args;
    return call;
}

json normalize_tool_call(const json& tc) {

    if (tc.is_object()) return tc;

    json out;
    out["function"]["name"] = nullptr;
    out["function"]["arguments"] = json::object();
    return out;
}

std::optional<std::string> direct_command_from_user(const std::string& text) {
    std::string cleaned = py_strip(text);

    static const std::regex re1(
        R"RE(^(?:(?:please|can you|could you|would you)\s+)?(?:run|execute)\s+(?:the\s+)?(?:command\s+)?(.+?)[?.!]*$)RE",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex re2(
        R"RE(^(?:(?:please|can you|could you|would you)\s+)?use\s+(?:the\s+)?command\s+(.+?)[?.!]*$)RE",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex re3(
        R"RE(^(?:(?:please|can you|could you|would you)\s+)?command\s+(.+?)[?.!]*$)RE",
        std::regex::ECMAScript | std::regex::icase);

    std::smatch m;
    bool matched = std::regex_search(cleaned, m, re1);
    if (!matched) matched = std::regex_search(cleaned, m, re2);
    if (!matched) matched = std::regex_search(cleaned, m, re3);
    if (!matched) return std::nullopt;

    std::string command = py_strip(m[1].str());
    if (command.empty()) return std::nullopt;
    static const std::regex trail_punct(R"RE([?.!]+$)RE");
    command = std::regex_replace(command, trail_punct, "");
    command = py_strip(command);
    command = strip_chars(command, "`'\" ");

    std::vector<std::string> parts;
    try {
        parts = shlex_split(command);
    } catch (...) {
        return std::nullopt;
    }
    if (parts.empty()) return std::nullopt;
    const std::string& first = parts[0];

    static const std::set<std::string> allowed = {
        "pwd", "ls", "find", "rg", "grep", "cat", "sed", "awk", "head", "tail", "wc",
        "git", "python", "python3", "pytest", "pip", "pip3", "uv", "npm", "node",
        "pnpm", "yarn", "bun", "make", "cargo", "go", "java", "javac", "curl",
        "wget", "hf", "ollama", "which", "whoami", "date", "uname", "df", "du",
        "ps", "env", "printenv",
    };
    if (allowed.count(first)) return command;
    return std::nullopt;
}

bool wants_system_and_ip(const std::string& text) {
    std::string lowered = to_lower_ascii(text);
    bool wants_system = contains_any(
        lowered, {"system", "os", "operating system", "machine", "computer"});
    bool wants_ip = contains_any(
        lowered, {"ip address", "ip adress", "ip addr", "my ip", "network address"});
    return wants_system && wants_ip;
}

std::string system_and_ip_command() {

    return "printf 'System:\\n'; "
           "uname -a; "
           "printf '\\nIP addresses:\\n'; "
           "(hostname -I 2>/dev/null || ip -o -4 addr show scope global 2>/dev/null | "
           "awk '{print $4}' || ifconfig 2>/dev/null | "
           "awk '/inet / && $2 != \"127.0.0.1\" {print $2}')";
}

bool is_continue_request(const std::string& text) {

    std::string lowered = to_lower_ascii(text);
    std::string cleaned;
    for (char c : lowered) {
        if (c >= 'a' && c <= 'z') cleaned.push_back(c);
    }
    static const std::set<std::string> conts = {
        "c", "continue", "coninue", "contine", "continie", "contnue"};
    return conts.count(cleaned) > 0;
}

bool implies_web_search(const std::string& text) {
    std::string lowered = to_lower_ascii(text);
    static const std::vector<std::string> search_phrases = {
        "let me search",
        "i need to search",
        "i should search",
        "i will search",
        "i'll search",
        "search for more",
        "do a focused search",
        "gather more",
        "more specific information",
        "need more detailed",
        "need more current",
    };
    return contains_any(lowered, search_phrases);
}

bool promises_next_action(const std::string& text) {
    std::string body = py_strip(text);
    if (body.empty()) return false;
    if (std::regex_search(body, COMPLETION_RE)) return false;
    if (std::regex_search(body, CONTINUATION_PROMISE_RE)) return true;

    std::string stripped = py_rstrip(body);
    if (!stripped.empty() && stripped.back() == ':' && utf8_len(stripped) <= 200) {
        return true;
    }

    static const std::regex future_re(
        R"RE(\b(?:i'?ll|i will|i'?m going to|i am going to|i'?m about to)\b)RE",
        std::regex::ECMAScript | std::regex::icase);
    if (std::regex_search(body, future_re)) return true;
    return false;
}

std::string infer_web_search_query(const std::string& text, const std::string& goal,
                                   int prior_searches) {

    static const std::regex ws_re(R"RE(\s+)RE");
    std::string content = py_strip(std::regex_replace(text, ws_re, " "));

    static const std::regex p1(
        R"RE(search for (?:more specific information about |information about |more )?(.+?)(?:[.:]|$))RE",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex p2(
        R"RE(focused search (?:on|for) (.+?)(?:[.:]|$))RE",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex p3(
        R"RE(gather more (?:comprehensive |detailed |current |specific )?information about (.+?)(?:[.:]|$))RE",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex p4(
        R"RE(more specific information about (.+?)(?:[.:]|$))RE",
        std::regex::ECMAScript | std::regex::icase);
    const std::regex* patterns[] = {&p1, &p2, &p3, &p4};
    for (const std::regex* pat : patterns) {
        std::smatch m;
        if (std::regex_search(content, m, *pat)) {
            std::string query = strip_chars(m[1].str(), " .:-");
            size_t L = utf8_len(query);
            if (4 <= L && L <= 160) return query;
        }
    }

    std::string goal_lower = to_lower_ascii(goal);
    if (goal_lower.find("us politics") != std::string::npos ||
        goal_lower.find("u.s. politics") != std::string::npos ||
        goal_lower.find("american politics") != std::string::npos) {
        static const std::vector<std::string> queries = {
            "current US political leadership 2026 Trump administration Congress Supreme Court",
            "US politics 2026 key issues economy immigration healthcare foreign policy",
            "2026 US midterm elections House Senate governors key races polling",
            "US politics 2026 recent developments Congress White House courts",
        };
        int last = static_cast<int>(queries.size()) - 1;
        int idx = std::min(prior_searches, last);
        if (idx < 0) idx = 0;
        return queries[static_cast<size_t>(idx)];
    }

    std::string base = !goal.empty() ? goal : (!content.empty() ? content : "current news");
    return utf8_substr(py_strip(base), 0, 180);
}

json spawn_agents_tool_schema() {
    json item_props = json::object();
    item_props["name"] = json{{"type", "string"}};
    item_props["task"] = json{{"type", "string"}};
    item_props["context"] = json{{"type", "string"}};

    json items = json::object();
    items["type"] = "object";
    items["properties"] = item_props;
    items["required"] = json::array({"task"});

    json agents = json::object();
    agents["type"] = "array";
    agents["items"] = items;

    json props = json::object();
    props["agents"] = agents;
    {
        json sc = json::object();
        sc["type"] = "string";
        sc["default"] = "";
        props["shared_context"] = sc;
    }
    {
        json t = json::object();
        t["type"] = "integer";
        t["default"] = 120;
        props["timeout_seconds"] = t;
    }
    {
        json ms = json::object();
        ms["type"] = "integer";
        ms["default"] = 3;
        props["max_steps"] = ms;
    }
    {
        json ta = json::object();
        ta["type"] = "string";
        ta["enum"] = json::array({"read_only", "full"});
        ta["default"] = "read_only";
        props["tool_access"] = ta;
    }

    json params = json::object();
    params["type"] = "object";
    params["properties"] = props;
    params["required"] = json::array({"agents"});

    json func = json::object();
    func["name"] = "spawn_agents";
    func["description"] =
        "Spawn up to 4 independent autonomous LOREA worker agents in parallel. Each child "
        "gets its own tool loop and memory. Use read_only for investigation; use full only "
        "when agents have clear ownership and may run commands or edit files.";
    func["parameters"] = params;

    json schema = json::object();
    schema["type"] = "function";
    schema["function"] = func;
    return schema;
}

std::string unicode_escape_decode(const std::string& s) {

    auto hexdig = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::u32string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c != '\\') {
            out.push_back(static_cast<char32_t>(c));
            ++i;
            continue;
        }
        if (i + 1 >= n) {
            out.push_back(static_cast<char32_t>('\\'));
            ++i;
            continue;
        }
        char e = s[i + 1];
        switch (e) {
            case '\n': i += 2; break;
            case '\\': out.push_back('\\'); i += 2; break;
            case '\'': out.push_back('\''); i += 2; break;
            case '"':  out.push_back('"');  i += 2; break;
            case 'a':  out.push_back(static_cast<char32_t>(0x07)); i += 2; break;
            case 'b':  out.push_back(static_cast<char32_t>(0x08)); i += 2; break;
            case 'f':  out.push_back(static_cast<char32_t>(0x0C)); i += 2; break;
            case 'n':  out.push_back(static_cast<char32_t>(0x0A)); i += 2; break;
            case 'r':  out.push_back(static_cast<char32_t>(0x0D)); i += 2; break;
            case 't':  out.push_back(static_cast<char32_t>(0x09)); i += 2; break;
            case 'v':  out.push_back(static_cast<char32_t>(0x0B)); i += 2; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                int val = 0, cnt = 0;
                size_t j = i + 1;
                while (j < n && cnt < 3 && s[j] >= '0' && s[j] <= '7') {
                    val = val * 8 + (s[j] - '0');
                    ++j;
                    ++cnt;
                }
                out.push_back(static_cast<char32_t>(val));
                i = j;
                break;
            }
            case 'x': {
                int h1 = (i + 2 < n) ? hexdig(s[i + 2]) : -1;
                int h2 = (i + 3 < n) ? hexdig(s[i + 3]) : -1;
                if (h1 < 0 || h2 < 0) throw std::runtime_error("truncated \\xXX escape");
                out.push_back(static_cast<char32_t>(h1 * 16 + h2));
                i += 4;
                break;
            }
            case 'u': {
                int hv = 0;
                for (int k = 0; k < 4; ++k) {
                    int h = (i + 2 + k < n) ? hexdig(s[i + 2 + k]) : -1;
                    if (h < 0) throw std::runtime_error("truncated \\uXXXX escape");
                    hv = hv * 16 + h;
                }
                out.push_back(static_cast<char32_t>(hv));
                i += 6;
                break;
            }
            case 'U': {
                long hv = 0;
                for (int k = 0; k < 8; ++k) {
                    int h = (i + 2 + k < n) ? hexdig(s[i + 2 + k]) : -1;
                    if (h < 0) throw std::runtime_error("truncated \\UXXXXXXXX escape");
                    hv = hv * 16 + h;
                }
                if (hv > 0x10FFFF) throw std::runtime_error("illegal \\U codepoint");
                out.push_back(static_cast<char32_t>(hv));
                i += 10;
                break;
            }
            case 'N':
                throw std::runtime_error("unsupported \\N{name} escape");
            default:

                out.push_back(static_cast<char32_t>('\\'));
                out.push_back(static_cast<char32_t>(static_cast<unsigned char>(e)));
                i += 2;
                break;
        }
    }
    return u32_to_utf8(out);
}

}
