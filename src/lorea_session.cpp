#include "lorea.hpp"

#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <cmath>
#include <cfenv>
#include <exception>

namespace ocli {

namespace fs = std::filesystem;

namespace {

bool jtruthy(const json& v) {
    if (v.is_null())    return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<double>() != 0.0;
    if (v.is_string())  return !v.get<std::string>().empty();
    if (v.is_array() || v.is_object()) return !v.empty();
    return true;
}

bool meta_truthy(const json& meta, const std::string& key) {
    auto it = meta.find(key);
    return it != meta.end() && jtruthy(*it);
}

long python_round(double x) {
    return static_cast<long>(std::nearbyint(x));
}

std::string format_local(std::time_t t, const char* fmt) {
    std::tm tmv {};
    ::localtime_r(&t, &tmv);
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tmv);
    return std::string(buf);
}

}

std::string LOREA::save_session(const std::string& path) {
    try {
        std::string target;
        if (!path.empty()) {
            target = resolve_session_path(path);
        } else {
            std::string stamp = format_local(std::time(nullptr), "%Y%m%d_%H%M%S");
            target = (fs::path(ensure_sessions_dir()) /
                      (std::string("session_") + stamp + ".json")).string();
        }
        fs::path parent = fs::absolute(target).parent_path();
        std::error_code ec;
        fs::create_directories(parent, ec);

        std::string saved_at = format_local(std::time(nullptr), "%Y-%m-%dT%H:%M:%S");

        json envelope = json::object();
        envelope["version"]           = SESSION_FORMAT_VERSION;
        envelope["saved_at"]          = saved_at;
        envelope["backend"]           = backend;
        envelope["model"]             = model_name;
        envelope["url"]               = url;
        if (mpc_url.has_value()) envelope["mpc_url"] = *mpc_url;
        else                     envelope["mpc_url"] = nullptr;
        envelope["auto_mode"]         = auto_mode;
        envelope["planning_enabled"]  = planning_enabled;
        envelope["compaction_count"]  = compaction_count;
        envelope["last_summary"]      = last_summary;

        json tasks_json = json::array();
        for (const auto& t : tasks)
            tasks_json.push_back(json{{"text", t.text}, {"status", t.status}});
        envelope["tasks"]             = tasks_json;

        envelope["message_count"]     = static_cast<long>(messages.size());

        json messages_json = json::array();
        for (const auto& m : messages)
            messages_json.push_back(m);
        envelope["messages"]          = messages_json;

        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("could not open " + target + " for writing");
        f << envelope.dump(2, ' ', true, json::error_handler_t::replace);
        f.close();
        if (!f) throw std::runtime_error("write failed for " + target);

        log_ok(std::string("Session saved ") + Colors::DIM + Colors::GRAY + "\xE2\x86\x92" + Colors::RESET +
               " " + Colors::WHITE + target + Colors::RESET +
               " " + Colors::DIM + Colors::GRAY + "(" + std::to_string(messages.size()) +
               " messages)" + Colors::RESET);
        return "Session saved to " + target;
    } catch (const std::exception& e) {
        log_warn(std::string("Error saving session: ") + e.what());
        return std::string("Error saving session: ") + e.what();
    }
}

std::optional<std::string> LOREA::session_menu() {
    std::vector<SessionInfo> sessions = list_saved_sessions();
    if (sessions.empty()) {
        log_info(std::string("No saved sessions in ") + SESSIONS_DIR);
        return std::nullopt;
    }
    std::vector<std::string> labels;
    for (const auto& item : sessions) {
        std::string when = format_local(static_cast<std::time_t>(item.mtime), "%Y-%m-%d %H:%M");
        long kb = std::max<long>(1, python_round(item.size / 1024.0));
        labels.push_back(item.name + "  " + Colors::DIM + Colors::GRAY + when + " \xC2\xB7 " +
                         std::to_string(kb) + " KB" + Colors::RESET);
    }
    std::optional<int> idx = interactive_menu("load session", labels, Colors::VIOLET);
    if (!idx.has_value())
        return std::nullopt;
    return sessions[static_cast<size_t>(*idx)].path;
}

std::string LOREA::load_session(const std::string& path) {
    try {
        std::string target;
        if (!path.empty()) {
            target = resolve_session_path(path);
            std::error_code ec;
            if (!fs::exists(target, ec) && fs::exists(path, ec))
                target = path;
        } else {
            std::optional<std::string> picked = session_menu();
            if (!picked.has_value() || picked->empty())
                return "Load canceled.";
            target = *picked;
        }
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            log_warn("Session not found: " + target);
            return "File not found: " + target;
        }

        json data;
        {
            std::ifstream f(target, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            data = json::parse(ss.str());
        }

        json msgs;
        json meta = json::object();
        if (data.is_array()) {
            msgs = data;
            meta = json::object();
        } else if (data.is_object() && data.contains("messages") && data["messages"].is_array()) {
            msgs = data["messages"];
            meta = data;
        } else {
            log_warn("Unrecognized session file format.");
            return "Unrecognized session format.";
        }

        if (msgs.empty() || !msgs[0].is_object()) {
            log_warn("Session file contains no usable messages.");
            return "Session file contains no usable messages.";
        }

        messages = std::vector<Message>(msgs.begin(), msgs.end());

        std::vector<std::string> restored;
        if (meta_truthy(meta, "model")) {
            model_name = meta["model"].get<std::string>();
            restored.push_back("model");
        }
        if (meta_truthy(meta, "backend") && meta["backend"].get<std::string>() != backend) {
            backend = meta["backend"].get<std::string>();
            server_model = std::nullopt;
            restored.push_back("backend");
        }
        if (meta_truthy(meta, "url")) {
            url = meta["url"].get<std::string>();
        }
        if (meta_truthy(meta, "mpc_url")) {
            mpc_url = meta["mpc_url"].get<std::string>();
            restored.push_back("mpc");

            try {
                json status = mpc_request("GET", "/status");
                if (status.contains("features") && status["features"].is_object())
                    mpc_features = status["features"];
                else
                    mpc_features = json::object();
                if (status.contains("version") && status["version"].is_string())
                    mpc_version = status["version"].get<std::string>();
                else
                    mpc_version = std::nullopt;
            } catch (...) {
                mpc_features = json::object();
                mpc_version = std::nullopt;
            }
        }
        if (meta.contains("auto_mode")) {
            auto_mode = jtruthy(meta["auto_mode"]);
            restored.push_back("auto");
        }
        if (meta.contains("planning_enabled")) {
            planning_enabled = jtruthy(meta["planning_enabled"]);
            restored.push_back("planning");
        }
        if (meta.contains("compaction_count") && meta["compaction_count"].is_number_integer()) {
            compaction_count = meta["compaction_count"].get<int>();
        }
        if (meta.contains("last_summary")) {
            const json& v = meta["last_summary"];
            last_summary = v.is_string() ? v.get<std::string>() : std::string("");
        }
        if (meta.contains("tasks") && meta["tasks"].is_array()) {
            std::vector<Task> nt;
            for (const auto& it : meta["tasks"]) {
                Task t;
                t.text   = (it.is_object() && it.contains("text")   && it["text"].is_string())
                               ? it["text"].get<std::string>()   : std::string("");
                t.status = (it.is_object() && it.contains("status") && it["status"].is_string())
                               ? it["status"].get<std::string>() : std::string("");
                nt.push_back(std::move(t));
            }
            tasks = std::move(nt);
        }

        log_ok(std::string("Session loaded ") + Colors::DIM + Colors::GRAY + "\xE2\x86\x90" + Colors::RESET +
               " " + Colors::WHITE + target + Colors::RESET +
               " " + Colors::DIM + Colors::GRAY + "(" + std::to_string(messages.size()) +
               " messages)" + Colors::RESET);
        if (!restored.empty()) {
            std::string joined;
            for (size_t i = 0; i < restored.size(); ++i) {
                if (i) joined += ", ";
                joined += restored[i];
            }
            log_info(std::string("Restored ") + joined + " " + Colors::DIM + Colors::GRAY + "\xC2\xB7" +
                     Colors::RESET + " " + Colors::WHITE + backend + ":" + model_name + Colors::RESET);
        }
        return "Session loaded from " + target;
    } catch (const json::parse_error& e) {
        log_warn(std::string("Session file is not valid JSON: ") + e.what());
        return std::string("Invalid session file: ") + e.what();
    } catch (const std::exception& e) {
        log_warn(std::string("Error loading session: ") + e.what());
        return std::string("Error loading session: ") + e.what();
    }
}

void LOREA::print_sessions() {
    std::vector<SessionInfo> sessions = list_saved_sessions();
    if (sessions.empty()) {
        log_info(std::string("No saved sessions in ") + SESSIONS_DIR);
        return;
    }
    std::vector<std::string> rows;
    rows.push_back(std::string(Colors::DIM) + Colors::GRAY + SESSIONS_DIR + Colors::RESET);
    rows.push_back("");
    for (const auto& item : sessions) {
        std::string when = format_local(static_cast<std::time_t>(item.mtime), "%Y-%m-%d %H:%M");
        long kb = std::max<long>(1, python_round(item.size / 1024.0));
        rows.push_back(std::string(Colors::WHITE) + item.name + Colors::RESET + "  " +
                       Colors::DIM + Colors::GRAY + when + " \xC2\xB7 " +
                       std::to_string(kb) + " KB" + Colors::RESET);
    }
    print_panel("sessions", rows, MUTED);
}

}
