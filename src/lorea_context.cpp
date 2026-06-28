#include "lorea.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ocli {

namespace {

const char* const PY_WS = " \t\n\r\f\v";

std::string strip_ws(const std::string& s) {
    std::size_t b = s.find_first_not_of(PY_WS);
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(PY_WS);
    return s.substr(b, e - b + 1);
}
std::string rstrip_ws(const std::string& s) {
    std::size_t e = s.find_last_not_of(PY_WS);
    if (e == std::string::npos) return "";
    return s.substr(0, e + 1);
}
std::string lstrip_ws(const std::string& s) {
    std::size_t b = s.find_first_not_of(PY_WS);
    if (b == std::string::npos) return "";
    return s.substr(b);
}

std::string strip_chars(const std::string& s, const std::string& chars) {
    std::size_t b = s.find_first_not_of(chars);
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(chars);
    return s.substr(b, e - b + 1);
}

std::string ascii_lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
std::string ascii_upper(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}

std::string collapse_ws(const std::string& s) {
    std::string out;
    bool pending_space = false;
    for (unsigned char ch : s) {
        bool is_ws = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v');
        if (is_ws) {
            if (!out.empty()) pending_space = true;
        } else {
            if (pending_space) { out += ' '; pending_space = false; }
            out += static_cast<char>(ch);
        }
    }
    return out;
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

const json* find_key(const json& o, const char* key) {
    if (o.is_object()) {
        auto it = o.find(key);
        if (it != o.end()) return &(*it);
    }
    return nullptr;
}

std::string py_repr(const json& v);

std::string py_repr_str(const std::string& s) {
    bool has_single = s.find('\'') != std::string::npos;
    bool has_double = s.find('"') != std::string::npos;
    char quote = (has_single && !has_double) ? '"' : '\'';
    std::string out;
    out += quote;
    std::u32string cp = utf8_to_u32(s);
    char buf[16];
    for (char32_t c : cp) {
        if (c == static_cast<char32_t>('\\')) out += "\\\\";
        else if (c == static_cast<char32_t>(quote)) { out += '\\'; out += quote; }
        else if (c == U'\n') out += "\\n";
        else if (c == U'\r') out += "\\r";
        else if (c == U'\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += u32_to_utf8(std::u32string(1, c));
        }
    }
    out += quote;
    return out;
}

std::string py_repr(const json& v) {
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return v.dump();
    if (v.is_string()) return py_repr_str(v.get<std::string>());
    if (v.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& e : v) { if (!first) out += ", "; first = false; out += py_repr(e); }
        out += "]";
        return out;
    }
    if (v.is_object()) {
        std::string out = "{";
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) out += ", "; first = false;
            out += py_repr_str(it.key());
            out += ": ";
            out += py_repr(it.value());
        }
        out += "}";
        return out;
    }
    return v.dump();
}

std::string py_str(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    return py_repr(v);
}

std::string str_or_empty(const json& v) {
    if (!json_truthy(v)) return "";
    return py_str(v);
}

std::string content_or_empty(const json& m) {
    const json* c = find_key(m, "content");
    return str_or_empty(c ? *c : json(""));
}

std::string role_of(const json& m) {
    const json* r = find_key(m, "role");
    if (r && r->is_string()) return r->get<std::string>();
    return "";
}

std::string json_escape_py(const std::string& s) {
    std::u32string cp = utf8_to_u32(s);
    std::string out;
    out.reserve(cp.size() + 2);
    char buf[16];
    for (char32_t c : cp) {
        switch (c) {
            case U'"':  out += "\\\""; break;
            case U'\\': out += "\\\\"; break;
            case U'\b': out += "\\b"; break;
            case U'\f': out += "\\f"; break;
            case U'\n': out += "\\n"; break;
            case U'\r': out += "\\r"; break;
            case U'\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x80) {
                    if (c > 0xFFFF) {
                        char32_t v = c - 0x10000;
                        unsigned hi = 0xD800u + static_cast<unsigned>(v >> 10);
                        unsigned lo = 0xDC00u + static_cast<unsigned>(v & 0x3FF);
                        std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", hi, lo);
                        out += buf;
                    } else {
                        std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void dumps_rec(const json& v, const char* item_sep, const char* kv_sep, std::string& out) {
    if (v.is_null()) { out += "null"; return; }
    if (v.is_boolean()) { out += v.get<bool>() ? "true" : "false"; return; }
    if (v.is_number_integer()) { out += std::to_string(v.get<long long>()); return; }
    if (v.is_number_unsigned()) { out += std::to_string(v.get<unsigned long long>()); return; }
    if (v.is_number_float()) { out += v.dump(); return; }
    if (v.is_string()) { out += '"'; out += json_escape_py(v.get<std::string>()); out += '"'; return; }
    if (v.is_array()) {
        out += '[';
        bool first = true;
        for (const auto& e : v) { if (!first) out += item_sep; first = false; dumps_rec(e, item_sep, kv_sep, out); }
        out += ']';
        return;
    }
    if (v.is_object()) {

        out += '{';
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) out += item_sep; first = false;
            out += '"'; out += json_escape_py(it.key()); out += '"';
            out += kv_sep;
            dumps_rec(it.value(), item_sep, kv_sep, out);
        }
        out += '}';
        return;
    }
    out += v.dump();
}

std::string py_json_dumps(const json& v, bool compact) {
    std::string out;
    dumps_rec(v, compact ? "," : ", ", compact ? ":" : ": ", out);
    return out;
}

std::vector<Message> vslice(const std::vector<Message>& v, std::size_t start, std::size_t end) {
    if (start > v.size()) start = v.size();
    if (end > v.size()) end = v.size();
    if (start >= end) return {};
    return std::vector<Message>(v.begin() + start, v.begin() + end);
}
std::vector<Message> vlast(const std::vector<Message>& v, std::size_t n) {
    if (v.size() <= n) return v;
    return std::vector<Message>(v.end() - n, v.end());
}

std::string lower_key(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

HttpResponse do_post_json(const std::string& url, const json& body,
                          const std::map<std::string, std::string>& headers, long timeout_ms) {
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    bool has_ct = false;
    for (const auto& kv : headers) {
        req.headers.push_back({kv.first, kv.second});
        if (lower_key(kv.first) == "content-type") has_ct = true;
    }
    if (!has_ct) req.headers.push_back({"Content-Type", "application/json"});
    req.body = body.dump();
    req.timeout_ms = timeout_ms;
    return http_perform(req);
}

void raise_for_status(const HttpResponse& resp) {
    if (!resp.error.empty()) throw std::runtime_error("ConnectionError: " + resp.error);
    if (resp.status >= 400)
        throw HttpStatusError(resp.status, std::to_string(resp.status) + " HTTP Error for url");
}

}

std::string LOREA::compact_message_text(const Message& message) {
    std::string role = "unknown";
    if (const json* rp = find_key(message, "role"))
        role = rp->is_string() ? rp->get<std::string>() : py_str(*rp);

    std::string name;
    {
        const json* np = find_key(message, "name");
        json nv = np ? *np : json(nullptr);
        if (json_truthy(nv)) {
            name = py_str(nv);
        } else {
            const json* tp = find_key(message, "tool_call_id");
            json tv = tp ? *tp : json(nullptr);
            name = json_truthy(tv) ? py_str(tv) : std::string("");
        }
    }

    std::string content = content_or_empty(message);
    const json* tcp = find_key(message, "tool_calls");
    bool has_tc = tcp && json_truthy(*tcp);
    std::string header = ascii_upper(role) + (name.empty() ? std::string("") : (" " + name));

    if (has_tc) {
        std::vector<std::string> calls;
        if (tcp->is_array()) {
            for (const auto& call : *tcp) {
                json fn = json::object();
                if (call.is_object()) {
                    const json* fp = find_key(call, "function");
                    fn = fp ? *fp : json::object();
                }
                std::string fname = "unknown";
                std::string argstr = "{}";
                if (fn.is_object()) {
                    const json* nmp = find_key(fn, "name");
                    fname = nmp ? py_str(*nmp) : std::string("unknown");
                    const json* ap = find_key(fn, "arguments");
                    argstr = py_str(ap ? *ap : json::object());
                }
                calls.push_back(fname + "(" + argstr + ")");
            }
        }
        std::string joined;
        for (std::size_t i = 0; i < calls.size(); ++i) { if (i) joined += "; "; joined += calls[i]; }
        content = content + "\nTool calls: " + joined;
    }

    content = collapse_ws(content);
    content = head_tail_trim(content, COMPACT_MAX_MESSAGE_CHARS);
    return content.empty() ? (header + ": [empty]") : (header + ": " + content);
}

std::optional<std::string> LOREA::objective_text() {
    std::string goal = strip_ws(last_user_goal);
    if (goal.empty()) return std::nullopt;
    return std::string("PRIMARY OBJECTIVE — the user's request; keep working toward exactly ")
         + "this and do not drift to a sub-task:\n" + head_tail_trim(goal, 1500);
}

std::string LOREA::compact_source_text(const std::vector<Message>& messages) {
    std::vector<std::string> parts;
    for (const auto& message : messages) {
        std::string c = content_or_empty(message);
        if (c.rfind("Working memory summary #", 0) == 0 || c.rfind("PRIMARY OBJECTIVE", 0) == 0)
            continue;
        parts.push_back(compact_message_text(message));
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) { if (i) out += "\n"; out += parts[i]; }
    return out;
}

std::vector<Message> LOREA::recent_context_slice() {
    std::vector<Message> body = vslice(messages, 1, messages.size());
    std::vector<Message> recent = vlast(body, static_cast<std::size_t>(COMPACT_RECENT_MESSAGES));
    std::size_t i = 0;
    while (i < recent.size() && role_of(recent[i]) == "tool") ++i;
    recent.erase(recent.begin(), recent.begin() + i);
    return recent;
}

std::vector<Message> LOREA::trim_recent_context(const std::vector<Message>& messages) {
    std::vector<Message> trimmed;
    for (const auto& message : messages) {
        std::string content = content_or_empty(message);
        int limit = (role_of(message) == "tool") ? COMPACT_RECENT_TOOL_CHARS : COMPACT_RECENT_TEXT_CHARS;
        if (static_cast<int>(utf8_len(content)) > limit) {
            Message clone = message;
            clone["content"] = head_tail_trim(content, static_cast<std::size_t>(limit));
            trimmed.push_back(std::move(clone));
        } else {
            trimmed.push_back(message);
        }
    }
    return trimmed;
}

int LOREA::estimate_context_tokens() {
    long long total = 0;
    for (const auto& message : messages) {
        total += static_cast<long long>(utf8_len(content_or_empty(message)));
        const json* tcp = find_key(message, "tool_calls");
        if (tcp && tcp->is_array()) {
            for (const auto& call : *tcp) {
                json fn = json::object();
                if (call.is_object()) {
                    const json* fp = find_key(call, "function");
                    fn = fp ? *fp : json::object();
                }
                json av = json("");
                if (fn.is_object()) {
                    const json* ap = find_key(fn, "arguments");
                    if (ap) av = *ap;
                }
                total += static_cast<long long>(utf8_len(py_str(av)));
            }
        }
    }
    return static_cast<int>(total / 4);
}

std::string LOREA::norm_tool_sig(const std::string& name, const json& args) {
    if ((name == "run_cmd" || name == "test_cmd") && args.is_object()) {
        const json* cp = find_key(args, "command");
        std::string cmd = collapse_ws(py_str(cp ? *cp : json("")));
        std::string filtered;
        for (char ch : cmd) if (ch != '"' && ch != '\'' && ch != '\\') filtered += ch;
        cmd = strip_ws(filtered);
        return name + ":" + cmd;
    }
    try {
        return name + ":" + py_json_dumps(args, false);
    } catch (...) {
        return name + ":" + py_str(args);
    }
}

std::string LOREA::fallback_compaction_summary(const std::vector<Message>& messages) {
    std::vector<std::string> users;
    std::vector<std::string> tools;
    for (const auto& m : messages) {
        std::string role = role_of(m);
        if (role == "user") {
            const json* cp = find_key(m, "content");
            users.push_back(cp ? py_str(*cp) : std::string(""));
        }
        if (role == "tool") {
            const json* np = find_key(m, "name");
            json nv = np ? *np : json(nullptr);
            if (json_truthy(nv)) tools.push_back(py_str(nv));
        }
    }
    std::string base_goal = !last_user_goal.empty()
                                ? last_user_goal
                                : (users.empty() ? std::string("") : users.back());
    std::string last_goal = strip_ws(base_goal);

    std::string tool_list;
    if (tools.empty()) {
        tool_list = "none";
    } else {
        std::vector<std::string> last12 = (tools.size() > 12)
                                              ? std::vector<std::string>(tools.end() - 12, tools.end())
                                              : tools;
        std::set<std::string> seen;
        std::vector<std::string> dedup;
        for (const auto& t : last12) if (seen.insert(t).second) dedup.push_back(t);
        for (std::size_t i = 0; i < dedup.size(); ++i) { if (i) tool_list += ", "; tool_list += dedup[i]; }
    }

    std::string goal_field = last_goal.empty() ? std::string("Continue the current task.") : last_goal;
    return "Goal: " + goal_field + "\n"
           "State: Conversation was compacted after " + std::to_string(messages.size()) + " older messages.\n"
           "Files: Preserve details from the recent context below.\n"
           "Commands: Recent tool usage included " + tool_list + ".\n"
           "Decisions: Continue from the preserved recent messages and avoid repeating blocked calls.\n"
           "Next Steps: Inspect current context, continue the user's latest request, and verify changes.";
}

std::string LOREA::request_compaction_summary(const std::vector<Message>& messages,
                                              const std::string& prior_summary) {
    std::string source = compact_source_text(messages);
    std::string base;
    if (!prior_summary.empty()) {
        base = "Existing working-memory summary to refine and extend. Keep every still-relevant fact, "
               "fold in the new events, and drop only what is now obsolete:\n"
             + prior_summary + "\n\n";
    }
    std::string goal_line;
    std::string goal = strip_ws(last_user_goal);
    if (!goal.empty()) {
        goal_line = "The user's exact request for this task is below. Copy it VERBATIM into the Goal line — "
                    "do not generalize it, rephrase it, shorten it, or replace it with whatever sub-step is "
                    "currently in progress. The overall objective must survive intact:\n"
                    "\"\"\"\n" + head_tail_trim(goal, 1200) + "\n\"\"\"\n\n";
    }
    std::string prompt =
        "Create a compact working-memory summary for an autonomous coding CLI. "
        "Preserve only durable facts needed to continue accurately. "
        "Include these labels exactly: Goal, State, Files, Commands, Decisions, Failures, Next Steps. "
        "The Goal line must restate the user's overall objective, not the current sub-task. "
        "Mention concrete filenames, commands, test results, backend/model changes, and unresolved risks when present. "
        "Do not include hidden reasoning or generic narration.\n\n"
        + goal_line + base + "New conversation events to fold in:\n" + source;

    if (mpc_url && !mpc_url->empty()) {
        json um = json::object(); um["role"] = "user"; um["content"] = prompt;
        json msgs = json::array(); msgs.push_back(um);
        json body = json::object();
        body["backend"] = backend;
        body["model"] = model_name;
        body["messages"] = msgs;
        body["tools"] = json::array();
        json data = mpc_request("POST", "/chat", &body, nullptr, nullptr, 180);
        const json* cp = data.is_object() ? find_key(data, "content") : nullptr;
        std::string c = strip_ws(cp ? str_or_empty(*cp) : std::string(""));
        return !c.empty() ? c : fallback_compaction_summary(messages);
    }

    if (backend == "ollama") {
        json um = json::object(); um["role"] = "user"; um["content"] = prompt;
        json msgs = json::array(); msgs.push_back(um);
        json opts = json::object(); opts["temperature"] = 0.1; opts["num_predict"] = COMPACT_SUMMARY_MAX_TOKENS;
        json body = json::object();
        body["model"] = model_name;
        body["messages"] = msgs;
        body["options"] = opts;
        body["stream"] = false;
        HttpResponse resp = do_post_json(url + "/api/chat", body, {}, 600000);
        raise_for_status(resp);
        json rj = json::parse(resp.body);
        return strip_ws(rj.at("message").at("content").get<std::string>());
    } else if (backend == "anthropic") {
        auto headers = anthropic_auth_headers();
        if (!headers || headers->empty()) return fallback_compaction_summary(messages);
        json um = json::object(); um["role"] = "user"; um["content"] = prompt;
        json msgs = json::array(); msgs.push_back(um);
        json body = json::object();
        body["model"] = model_name;
        body["max_tokens"] = COMPACT_SUMMARY_MAX_TOKENS;
        body["temperature"] = 0.1;
        body["messages"] = msgs;
        HttpResponse resp = do_post_json(cloud_base() + "/v1/messages", body, *headers, 120000);
        raise_for_status(resp);
        json rj = json::parse(resp.body);
        json blocks = (rj.is_object() && rj.contains("content")) ? rj["content"] : json(nullptr);
        if (!json_truthy(blocks)) blocks = json::array();
        std::string out;
        if (blocks.is_array()) {
            for (const auto& b : blocks) {
                std::string type = (b.is_object() && b.contains("type") && b["type"].is_string())
                                       ? b["type"].get<std::string>()
                                       : std::string("");
                if (type == "text") {
                    const json* tp = b.is_object() ? find_key(b, "text") : nullptr;
                    out += (tp && tp->is_string()) ? tp->get<std::string>() : std::string("");
                }
            }
        }
        std::string s = strip_ws(out);
        return !s.empty() ? s : fallback_compaction_summary(messages);
    } else if (backend == "openai") {
        auto key = api_key("openai");
        if (!key || key->empty()) return fallback_compaction_summary(messages);
        json um = json::object(); um["role"] = "user"; um["content"] = prompt;
        json msgs = json::array(); msgs.push_back(um);
        json body = json::object();
        body["model"] = model_name;
        body["messages"] = msgs;
        body["temperature"] = 0.1;
        body["max_tokens"] = COMPACT_SUMMARY_MAX_TOKENS;
        std::map<std::string, std::string> h;
        h["Authorization"] = "Bearer " + *key;
        HttpResponse resp = do_post_json(cloud_base() + "/v1/chat/completions", body, h, 120000);
        raise_for_status(resp);
        json rj = json::parse(resp.body);
        return strip_ws(rj.at("choices").at(0).at("message").at("content").get<std::string>());
    } else if (backend == "airllm") {
        if (!airllm_model) return fallback_compaction_summary(messages);

        log_info("AirLLM compaction failed: in-process AirLLM is not supported in this build");
        return fallback_compaction_summary(messages);
    }

    json um = json::object(); um["role"] = "user"; um["content"] = prompt;
    json msgs = json::array(); msgs.push_back(um);
    json body = json::object();
    body["model"] = model_name;
    body["messages"] = msgs;
    body["temperature"] = 0.1;
    body["max_tokens"] = COMPACT_SUMMARY_MAX_TOKENS;
    HttpResponse resp = do_post_json(url + "/v1/chat/completions", body, {}, 60000);
    raise_for_status(resp);
    json rj = json::parse(resp.body);
    return strip_ws(rj.at("choices").at(0).at("message").at("content").get<std::string>());
}

bool LOREA::effort_deep() {
    return effort_level == "elite" || effort_level == "mythic" || effort_level == "beyond";
}

std::vector<std::string> LOREA::extract_referenced_files() {
    std::set<std::string> files;
    for (const auto& f : session_files_touched) files.insert(f);

    static const std::regex FILE_REF_RE(
        R"((?:~|\.{0,2})?/[\w./\-]+\.\w{1,8}|\b[\w\-]+\.(?:py|sh|js|ts|tsx|md|json|jsonl|ya?ml|txt|c|cc|cpp|h|hpp|go|rs|rb|java|cfg|conf|toml|ini|html|css|sql|gguf|safetensors)\b)");
    static const std::string STRIP_SET = std::string("`") + "\"" + "," + "'" + ")" + ".";

    for (const auto& m : messages) {
        const json* cp = find_key(m, "content");
        if (cp && cp->is_string()) {
            std::string c = cp->get<std::string>();
            if (utf8_len(c) < 20000) {
                for (auto it = std::sregex_iterator(c.begin(), c.end(), FILE_REF_RE);
                     it != std::sregex_iterator(); ++it) {
                    std::string h = strip_chars(strip_ws(it->str()), STRIP_SET);
                    std::size_t L = utf8_len(h);
                    if (3 < L && L < 200) files.insert(h);
                }
            }
        }
        const json* tcp = find_key(m, "tool_calls");
        if (tcp && tcp->is_array()) {
            for (const auto& tc : *tcp) {
                json args = json(nullptr);
                if (tc.is_object()) {
                    const json* fp = find_key(tc, "function");
                    json fn = (fp && json_truthy(*fp)) ? *fp : json::object();
                    if (fn.is_object()) {
                        const json* ap = find_key(fn, "arguments");
                        if (ap) args = *ap;
                    }
                }
                if (args.is_object()) {
                    for (const char* k : {"path", "file", "filename", "file_path"}) {
                        const json* v = find_key(args, k);
                        if (v && v->is_string()) files.insert(v->get<std::string>());
                    }
                }
            }
        }
    }

    std::vector<std::string> out;
    for (const auto& f : files) {
        if (f.empty()) continue;
        out.push_back(f);
        if (out.size() >= 60) break;
    }
    return out;
}

std::vector<std::string> LOREA::model_output_log(const std::vector<Message>& messages) {
    std::vector<std::string> outs;
    for (const auto& m : messages) {
        if (role_of(m) != "assistant") continue;
        const json* cp = find_key(m, "content");
        std::string raw;
        if (cp && cp->is_string()) raw = cp->get<std::string>();
        else if (cp) raw = py_str(*cp);
        std::string v = strip_ws(visible_answer(raw));
        if (!v.empty()) {
            std::string entry = utf8_substr(v, 0, 700);
            if (utf8_len(v) > 700) entry += "…";
            outs.push_back(entry);
        }
    }
    return outs;
}

void LOREA::persist_outputs(const std::vector<std::string>& outs) {
    try {
        std::string d = expanduser("~/.ocli/outputs");
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        long long stamp = static_cast<long long>(session_started_at);
        std::string p = (std::filesystem::path(d) / ("outputs_" + std::to_string(stamp) + ".jsonl")).string();
        std::ofstream f(p, std::ios::app);
        if (f) {
            for (const auto& o : outs) {
                json rec = json::object();
                rec["output"] = o;
                f << py_json_dumps(rec, false) << "\n";
            }
        }
    } catch (...) {

    }
}

void LOREA::compact_history() {
    int estimated = estimate_context_tokens();
    if (messages.size() <= static_cast<std::size_t>(HISTORY_THRESHOLD) && estimated <= COMPACT_TOKEN_BUDGET)
        return;
    std::string reason = (estimated > COMPACT_TOKEN_BUDGET) ? "token budget" : "message count";
    log_info("Compacting history (~" + std::to_string(estimated) + " tokens, " +
             std::to_string(messages.size()) + " messages, trigger: " + reason + ")...");

    Message system_msg = messages[0];
    std::vector<Message> recent_context = trim_recent_context(recent_context_slice());
    std::size_t middle_end = messages.size() - recent_context.size();
    std::vector<Message> middle_messages = vslice(messages, 1, middle_end);

    if (middle_messages.empty()) {
        std::vector<Message> body = trim_recent_context(vslice(messages, 1, messages.size()));
        bool still_oversized = (messages.size() > static_cast<std::size_t>(HISTORY_THRESHOLD)) ||
                               (estimate_context_tokens() > COMPACT_TOKEN_BUDGET);
        if (still_oversized && body.size() > 4) {
            std::size_t keep_tail = std::max<std::size_t>(4, static_cast<std::size_t>(COMPACT_RECENT_MESSAGES / 2));
            std::vector<Message> tail = vlast(body, keep_tail);
            std::size_t i = 0;
            while (i < tail.size() && role_of(tail[i]) == "tool") ++i;
            tail.erase(tail.begin(), tail.begin() + i);
            std::vector<Message> older = vslice(body, 0, body.size() - tail.size());
            middle_messages = older;
            recent_context = tail;
        } else {
            std::vector<Message> pinned;
            if (auto obj = objective_text()) {
                json p = json::object(); p["role"] = "assistant"; p["content"] = *obj;
                pinned.push_back(std::move(p));
            }
            if (auto plan = plan_state_text()) {
                json p = json::object(); p["role"] = "assistant"; p["content"] = *plan;
                pinned.push_back(std::move(p));
            }
            std::vector<Message> nm;
            nm.push_back(system_msg);
            for (auto& x : pinned) nm.push_back(std::move(x));
            for (auto& x : body) nm.push_back(std::move(x));
            messages = std::move(nm);
            log_info("Trimmed oversized recent context. Preserved " + std::to_string(body.size()) + " messages.");
            return;
        }
    }

    std::string summary;
    try {
        summary = request_compaction_summary(middle_messages, last_summary);
    } catch (const std::exception& e) {
        log_info(std::string("Compaction model failed (") + e.what() + "). Using local summary.");
        summary = fallback_compaction_summary(middle_messages);
    }
    compaction_count += 1;
    last_summary = summary;

    std::vector<std::string> parts;
    if (auto obj = objective_text()) parts.push_back(*obj);
    if (auto plan = plan_state_text()) parts.push_back(*plan);
    parts.push_back("Working memory summary #" + std::to_string(compaction_count) + ":\n" + summary);

    if (effort_deep()) {
        std::vector<std::string> outs = model_output_log(middle_messages);
        if (!outs.empty()) {
            persist_outputs(outs);
            std::vector<std::string> last24 = (outs.size() > 24)
                                                  ? std::vector<std::string>(outs.end() - 24, outs.end())
                                                  : outs;
            std::string logstr;
            for (std::size_t i = 0; i < last24.size(); ++i) {
                if (i) logstr += "\n---\n";
                logstr += "[" + std::to_string(i + 1) + "] " + last24[i];
            }
            parts.push_back("Your prior outputs (decision log — your own reasoning, preserved across "
                            "compaction; continue from here, do not repeat work already done):\n" + logstr);
        }
        std::vector<std::string> files = extract_referenced_files();
        if (!files.empty()) {
            std::string fstr;
            for (std::size_t i = 0; i < files.size(); ++i) { if (i) fstr += "\n"; fstr += "- " + files[i]; }
            parts.push_back("Files referenced so far (PATHS ONLY — contents are NOT included here; "
                            "re-read any with read_file when you need it):\n" + fstr);
        }
    }

    parts.push_back("Current backend: " + backend + "\n"
                    "Current model: " + model_name + "\n"
                    "Auto mode: " + std::string(auto_mode ? "ON" : "OFF") + "\n"
                    "Planning: " + std::string(planning_enabled ? "ENABLED" : "DISABLED"));

    std::string memory;
    for (std::size_t i = 0; i < parts.size(); ++i) { if (i) memory += "\n\n"; memory += parts[i]; }

    std::vector<Message> nm;
    nm.push_back(system_msg);
    {
        json mm = json::object(); mm["role"] = "assistant"; mm["content"] = memory;
        nm.push_back(std::move(mm));
    }
    for (auto& x : recent_context) nm.push_back(std::move(x));
    messages = std::move(nm);
    log_info("Compaction #" + std::to_string(compaction_count) + " done (~" +
             std::to_string(estimate_context_tokens()) + " tokens, preserved " +
             std::to_string(recent_context.size()) + " recent messages).");
}

std::string LOREA::stall_decision(const std::string& content, int attempt) {
    std::string sig = ascii_lower(collapse_ws(content));
    std::string prev = stall_last_sig;
    if (!sig.empty()) stall_last_sig = sig;
    bool near_repeat = false;
    if (!prev.empty() && !sig.empty()) {
        if (sig == prev) {
            near_repeat = true;
        } else if (std::min(utf8_len(sig), utf8_len(prev)) >= 40 && difflib_ratio(sig, prev) >= 0.85) {
            near_repeat = true;
        }
    }
    if (near_repeat) return "stop";
    if (attempt > 3) return "finalize";
    return "remind";
}

int LOREA::prune_repeated_assistant(const std::string& sig) {
    std::vector<Message> keep;
    int removed = 0;
    for (const auto& m : messages) {
        const json* tcp = find_key(m, "tool_calls");
        bool no_tc = !(tcp && json_truthy(*tcp));
        if (role_of(m) == "assistant" && no_tc) {
            const json* cp = find_key(m, "content");
            std::string c = ascii_lower(collapse_ws(visible_answer(str_or_empty(cp ? *cp : json(nullptr)))));
            if (!c.empty() && utf8_len(c) >= 40 && difflib_ratio(c, sig) >= 0.85) {
                ++removed;
                continue;
            }
        }
        keep.push_back(m);
    }
    if (removed) messages = std::move(keep);
    return removed;
}

std::string LOREA::tool_reminder_text() {
    std::string names = "run_cmd, test_cmd, read_file, write_file, list_files, search_files, "
                        "find_files, grep, web_search, read_url, http_request, git_status, git_diff";
    if (allow_spawn_agents) names += ", spawn_agents";
    return std::string(
        "[tool protocol — reminder] To take an action, output exactly ONE tool call and nothing else: "
        "<tools>{\"name\":\"TOOL\",\"arguments\":{...}}</tools> "
        "(e.g. <tools>{\"name\":\"grep\",\"arguments\":{\"pattern\":\"eval(\",\"path\":\"app.py\"}}</tools>). "
        "Provide EVERY required argument as real JSON — required args per tool: "
        "read_file{path}, list_files{path}, grep{pattern,path}, search_files/find_files{query,path}, "
        "run_cmd{command}, test_cmd{command}, write_file{path,content}, web_search{query}, read_url{url}, "
        "http_request{url,method,data}, git_status{}, git_diff{}. Give a COMPLETE command (never a partial like `grep -n` with no pattern), "
        "and NEVER repeat a call you already made this turn — its output is already in the conversation above. "
        "Never describe an action in prose instead of calling the tool, and never invent tool output. "
        "IMPORTANT: when you ALREADY have what the request needs — for example you have read the file you were "
        "asked to analyze — do NOT keep calling tools. Write your complete answer now in plain natural language. "
        "Available tools: ") + names + ".";
}

bool LOREA::has_open_plan() {
    if (tasks.empty()) return false;
    for (const auto& t : tasks) if (t.status != "done") return true;
    return false;
}

bool LOREA::in_working_context() {
    if (auto_mode || has_open_plan()) return true;
    for (const auto& m : messages) {
        const json* tcp = find_key(m, "tool_calls");
        if (tcp && json_truthy(*tcp)) return true;
        if (role_of(m) == "tool") return true;
    }
    return false;
}

std::optional<Message> LOREA::tool_reminder_message() {
    if (tool_access == "none" || !in_working_context()) return std::nullopt;
    json m = json::object(); m["role"] = "system"; m["content"] = tool_reminder_text();
    return m;
}

std::optional<std::string> LOREA::plan_state_text() {
    if (!planning_enabled || !has_open_plan()) return std::nullopt;
    int total = static_cast<int>(tasks.size());
    int done = 0;
    for (const auto& t : tasks) if (t.status == "done") ++done;

    std::vector<std::string> lines;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const std::string& st = tasks[i].status;
        std::string mark = (st == "done") ? "[x]" : (st == "doing") ? "[>]" : (st == "todo") ? "[ ]" : "[ ]";
        lines.push_back("  " + mark + " " + std::to_string(i + 1) + ". " + tasks[i].text);
    }
    std::optional<int> doing;
    for (std::size_t i = 0; i < tasks.size(); ++i)
        if (tasks[i].status == "doing") { doing = static_cast<int>(i) + 1; break; }
    std::optional<int> first_open;
    for (std::size_t i = 0; i < tasks.size(); ++i)
        if (tasks[i].status != "done") { first_open = static_cast<int>(i) + 1; break; }

    std::string nxt;
    if (doing) {
        nxt = "You are on step #" + std::to_string(*doing) +
              ". The moment it is finished call update_task(index=" + std::to_string(*doing) +
              ", status=done), then update_task the next step to 'doing'. Keep exactly one step 'doing'.";
    } else {
        std::string fo = first_open ? std::to_string(*first_open) : "None";
        nxt = "No step is marked 'doing'. Call update_task(index=" + fo +
              ", status=doing) before you work on it.";
    }
    std::string body;
    for (std::size_t i = 0; i < lines.size(); ++i) { if (i) body += "\n"; body += lines[i]; }
    return "[active plan — " + std::to_string(done) + "/" + std::to_string(total) +
           " done] Keep working this plan to completion; do not skip steps, re-plan, or stop early.\n" +
           body + "\n" + nxt;
}

std::optional<Message> LOREA::plan_state_message() {
    auto text = plan_state_text();
    if (!text || text->empty()) return std::nullopt;
    json m = json::object(); m["role"] = "system"; m["content"] = *text;
    return m;
}

std::optional<Message> LOREA::effort_message() {
    const auto& levels = effort_levels();
    auto it = levels.find(effort_level);
    if (it == levels.end()) return std::nullopt;
    json m = json::object(); m["role"] = "system"; m["content"] = it->second.directive;
    return m;
}

std::optional<Message> LOREA::advr_reminder_message() {
    if (!effort_deep() || !in_working_context()) return std::nullopt;
    json m = json::object();
    m["role"] = "system";
    m["content"] = std::string("[reasoning loop — reminder]") + ADVR_LOOP;
    return m;
}

std::vector<Message> LOREA::ephemeral_reminders() {
    std::vector<Message> out;
    if (auto m = tool_reminder_message()) out.push_back(*m);
    if (auto m = plan_state_message()) out.push_back(*m);
    if (auto m = recent_tool_results_message()) out.push_back(*m);
    if (auto m = advr_reminder_message()) out.push_back(*m);
    if (auto m = effort_message()) out.push_back(*m);
    return out;
}

std::vector<Message> LOREA::server_messages() {
    std::vector<Message> prepared;
    for (const auto& message : messages) {

        std::string role;
        const json* rp = find_key(message, "role");
        if (!rp) role = "user";
        else if (rp->is_string()) role = rp->get<std::string>();
        else role = "";

        std::string content = content_or_empty(message);

        if (role == "tool") {
            const json* np = find_key(message, "name");
            std::string name = np ? str_or_empty(*np) : std::string("");
            if (name.empty()) name = "tool";
            json out = json::object();
            out["role"] = "user";
            out["content"] = "Tool result from " + name + ":\n" + content;
            prepared.push_back(std::move(out));
        } else if (role == "system" || role == "user" || role == "assistant") {
            if (role == "assistant") {
                const json* tcp = find_key(message, "tool_calls");
                bool has_tc = tcp && json_truthy(*tcp);
                if (has_tc && strip_ws(content).empty()) {
                    std::vector<std::string> parts;
                    if (tcp->is_array()) {
                        for (const auto& c : *tcp) {
                            json fn = json::object();
                            if (c.is_object()) {
                                const json* fp = find_key(c, "function");
                                fn = fp ? *fp : json::object();
                            }
                            json a = json::object();
                            std::string fname = "tool";
                            if (fn.is_object()) {
                                const json* ap = find_key(fn, "arguments");
                                if (ap) a = *ap;
                                const json* nmp = find_key(fn, "name");
                                fname = nmp ? py_str(*nmp) : std::string("tool");
                            }
                            std::string as;
                            try { as = py_json_dumps(a, true); }
                            catch (...) { as = py_str(a); }
                            parts.push_back(fname + "(" + as + ")");
                        }
                    }
                    std::string joined;
                    for (std::size_t i = 0; i < parts.size(); ++i) { if (i) joined += "; "; joined += parts[i]; }
                    content = "[I called: " + joined + "]";
                }
            }
            json out = json::object();
            out["role"] = role;
            out["content"] = content;
            prepared.push_back(std::move(out));
        } else {
            json out = json::object();
            out["role"] = "user";
            out["content"] = content;
            prepared.push_back(std::move(out));
        }
    }
    for (auto& r : ephemeral_reminders()) prepared.push_back(std::move(r));
    return prepared;
}

std::string LOREA::latest_tool_result_text() {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const Message& message = *it;
        if (role_of(message) == "tool") {
            const json* cp = find_key(message, "content");
            std::string cstr = str_or_empty(cp ? *cp : json(nullptr));
            if (!strip_ws(cstr).empty()) {
                const json* np = find_key(message, "name");
                std::string name = np ? str_or_empty(*np) : std::string("");
                if (name.empty()) name = "tool";
                return "Latest tool result from " + name + ":\n" + py_str(*cp);
            }
        }
    }
    return "";
}

std::optional<Message> LOREA::recent_tool_results_message(int limit, int per_result_chars) {
    std::vector<std::string> collected;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const Message& message = *it;
        if (role_of(message) != "tool") continue;
        const json* cp = find_key(message, "content");
        std::string content = strip_ws(str_or_empty(cp ? *cp : json(nullptr)));
        if (content.empty()) continue;
        const json* np = find_key(message, "name");
        std::string name = np ? str_or_empty(*np) : std::string("");
        if (name.empty()) name = "tool";
        if (static_cast<int>(utf8_len(content)) > per_result_chars) {
            int half = per_result_chars / 2;
            std::size_t clen = utf8_len(content);
            std::string head = rstrip_ws(utf8_substr(content, 0, static_cast<std::size_t>(half)));
            std::string tail = lstrip_ws(utf8_substr(content, clen - static_cast<std::size_t>(half)));
            content = head + "\n...[trimmed]...\n" + tail;
        }
        collected.push_back(name + " returned:\n" + content);
        if (static_cast<int>(collected.size()) >= limit) break;
    }
    if (collected.empty()) return std::nullopt;
    std::reverse(collected.begin(), collected.end());
    std::string body;
    for (std::size_t i = 0; i < collected.size(); ++i) {
        if (i) body += "\n\n";
        body += "[" + std::to_string(i + 1) + "] " + collected[i];
    }
    std::string guidance =
        "GROUND TRUTH — these are the ACTUAL outputs of your last " +
        std::to_string(collected.size()) +
        " tool call(s), oldest first, most recent LAST. Base every "
        "factual claim ONLY on what these outputs literally show. Do NOT assert a "
        "result they do not contain: if a login response says 'Invalid username or "
        "password', that attempt FAILED; if a search produced no output, there were "
        "NO matches; you have a flag/secret only if an output literally prints it. If "
        "an output contradicts what you were about to say, correct course instead of "
        "restating the belief.";
    json m = json::object();
    m["role"] = "system";
    m["content"] = guidance + "\n\n" + body;
    return m;
}

std::string LOREA::last_assistant_text() {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const Message& message = *it;
        if (role_of(message) == "assistant") {
            const json* cp = find_key(message, "content");
            std::string txt = visible_answer(str_or_empty(cp ? *cp : json(nullptr)));
            if (!txt.empty()) return txt;
        }
    }
    return "";
}

}
