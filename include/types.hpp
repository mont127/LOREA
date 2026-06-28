#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <utility>
#include <stdexcept>
#include <cstdint>

#include "json.hpp"

namespace ocli {

using json = nlohmann::json;

using Message = json;

using ToolCall = json;

using ToolResult = std::string;

enum class BackendKind { Ollama, LlamaCpp, Mlx, AirLLM, OpenAI, Anthropic, Server, Unknown };
BackendKind backend_kind(const std::string& backend);
std::string to_string(BackendKind k);

enum class RiskLevel { Normal, Dangerous, Catastrophic };

struct EffortLevel {
    std::string label;
    std::string color;
    std::string directive;
};

struct Task {
    std::string text;
    std::string status;
};

struct UndoEntry {
    std::string path;
    bool        existed = false;
    std::string old_content;
};

struct MouseEvent {
    int  button = 0;
    int  col    = 0;
    int  row    = 0;
    bool pressed = false;
};

struct Unit {
    std::string display;
    std::string actual;
};

struct SessionInfo {
    std::string name;
    std::string path;
    double      mtime = 0.0;
    long        size  = 0;
};

struct PathSafety {
    bool        ok = false;
    std::string reason;
    std::string abspath;
    bool        has_abspath = false;
};

struct VramBounds {
    long lo = 0, hi = 0, reserve = 0;
};

struct ConnectOpts {
    std::optional<std::string> url;
    std::optional<std::string> token;
    bool status     = false;
    bool disconnect = false;
    bool no_menu    = false;
};

struct AgentOptions {
    int         count = 3;
    int         timeout_seconds = 180;
    int         max_steps = 3;
    std::string tool_access = "read_only";
    std::string goal;
};

struct SigCacheEntry {
    std::string result;
    int         count = 0;
};

struct DownloadOption {
    std::string                label;
    std::string                value;
    std::optional<std::string> url;
};

struct MPCRetryable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MpcNoConnection : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MpcUnauthorized : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct HttpStatusError : std::runtime_error {
    long status = 0;
    HttpStatusError(long s, const std::string& msg) : std::runtime_error(msg), status(s) {}
};

struct ShlexError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline constexpr int    MAX_OUTPUT_LENGTH        = 10000;
inline constexpr int    MAX_URL_OUTPUT_LENGTH    = 1500000;
inline constexpr int    HISTORY_THRESHOLD        = 36;
inline constexpr int    COMPACT_RECENT_MESSAGES  = 12;
inline constexpr int    COMPACT_MAX_MESSAGE_CHARS= 2200;
inline constexpr int    COMPACT_TOKEN_BUDGET     = 16000;
inline constexpr int    COMPACT_RECENT_TOOL_CHARS= 4000;
inline constexpr int    COMPACT_RECENT_TEXT_CHARS= 8000;
inline constexpr int    COMPACT_SUMMARY_MAX_TOKENS = 700;
inline constexpr int    SESSION_FORMAT_VERSION   = 1;
inline constexpr const char* MPC_DEFAULT_URL     = "http://127.0.0.1:8765";
inline constexpr int    MPC_REQUEST_TIMEOUT      = 20;
inline constexpr double MPC_DOWNLOAD_POLL_INTERVAL = 2.0;
inline constexpr int    MPC_CHAT_TIMEOUT         = 600;
inline constexpr int    MPC_CHAT_MAX_RETRIES     = 2;
inline constexpr const char* ANTHROPIC_VERSION   = "2023-06-01";
inline constexpr const char* CLI_COMMAND_NAME    = "OCLI";

extern const std::string                                       SESSIONS_DIR;
extern const std::map<std::string, std::string>               BACKEND_DEFAULT_URLS;
extern const std::map<std::string, std::string>               BACKEND_DEFAULT_MODELS;
extern const std::set<std::string>                            CLOUD_BACKENDS;
extern const std::map<std::string, std::vector<std::string>>  BACKEND_API_KEY_ENV;
extern const std::map<std::string, std::vector<std::string>>  MODEL_SUGGESTIONS;
extern const std::map<std::string, std::vector<DownloadOption>> DOWNLOAD_MODEL_OPTIONS;
extern const std::vector<std::string>                         CLI_INSTALL_PATHS;
extern const std::vector<std::string>                         ALLOWED_SEARCH_DOMAINS;

extern const std::vector<std::string>                         EFFORT_ORDER;
const std::map<std::string, EffortLevel>&                     effort_levels();
extern const std::string                                      ADVR_LOOP;

std::string  msg_role(const Message& m);
std::string  msg_content(const Message& m);
std::string  msg_name(const Message& m);
bool         msg_has_tool_calls(const Message& m);
Message      make_message(const std::string& role, const std::string& content);

}
