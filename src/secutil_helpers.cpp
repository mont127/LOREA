#include "secutil.hpp"
#include "ansi.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace ocli {

namespace {

bool path_is_dir(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool path_is_file(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> split_str(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string lower_str(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

std::string last_segment(const std::string& s) {
    size_t p = s.rfind('/');
    return (p == std::string::npos) ? s : s.substr(p + 1);
}

const std::string CYBER_V51 = expanduser("~/lorea-ft/lorea-coder-30b-a3b-cyber-v5-1");
const std::string CYBER_V42 = expanduser("~/lorea-ft/lorea-coder-30b-a3b-cyber-v4-2");
const std::string CYBER_V41 = expanduser("~/lorea-ft/lorea-coder-30b-a3b-cyber-v4-1");
const std::string CYBER_V3  = expanduser("~/lorea-ft/lorea-coder-30b-a3b-cyber-v3");
const std::string LOREA_CYBER_DIR =
      path_is_dir(CYBER_V51) ? CYBER_V51
    : path_is_dir(CYBER_V42) ? CYBER_V42
    : path_is_dir(CYBER_V41) ? CYBER_V41
    : path_is_dir(CYBER_V3)  ? CYBER_V3
    : expanduser("~/lorea-ft/lorea-coder-30b-a3b-cyber");

const std::string CYBER_GGUF_V42_LOCAL = expanduser("~/lorea-ft/lorea-cyber-v42-q4_k_m.gguf");
const std::string CYBER_GGUF_V42_SSD   = "/Volumes/ASAFE/lorea-v42-gguf/lorea-cyber-v42-q4_k_m.gguf";
const std::string CYBER_GGUF_V41_LOCAL = expanduser("~/lorea-ft/lorea-cyber-v41-q4_k_m.gguf");
const std::string LOREA_CYBER_GGUF =
      path_is_file(CYBER_GGUF_V42_LOCAL) ? CYBER_GGUF_V42_LOCAL
    : path_is_file(CYBER_GGUF_V42_SSD)   ? CYBER_GGUF_V42_SSD
    : path_is_file(CYBER_GGUF_V41_LOCAL) ? CYBER_GGUF_V41_LOCAL
    : CYBER_GGUF_V42_LOCAL;

const std::string LOREA_DIR = expanduser("~/lorea-ft/lorea-coder-30b-a3b-v3-re");

const std::vector<std::string> OPENAI_API_MODELS = {
    "gpt-4o", "gpt-4o-mini", "gpt-4.1", "gpt-4.1-mini", "gpt-4.1-nano",
    "o3", "o3-mini", "o4-mini", "o1", "gpt-4-turbo",
};
const std::vector<std::string> ANTHROPIC_API_MODELS = {
    "claude-opus-4-20250514", "claude-sonnet-4-20250514", "claude-3-7-sonnet-latest",
    "claude-3-5-sonnet-latest", "claude-3-5-haiku-latest", "claude-3-opus-latest",
};
const std::vector<std::string> OLLAMA_MODELS = {
    "qwen3.6:27b-coding-nvfp4", "llama3.2", "llama3.1", "llama3.3", "qwen3", "qwen2.5",
    "qwen2.5vl", "qwen3-vl", "mistral", "mistral-nemo", "mistral-small", "mistral-small3.2",
    "gemma3", "gemma2", "phi4", "phi3", "deepseek-r1", "deepseek-v3", "olmo2", "olmo-3", "aya",
    "dolphin3", "dolphin-llama3", "neural-chat", "nous-hermes2", "orca-mini", "mixtral",
    "falcon3", "smollm", "smollm2", "cogito", "lfm2.5-thinking", "rnj-1", "nemotron-3-nano",
    "granite3.1-moe", "granite3.2", "granite3.3", "qwen2.5-coder", "qwen3-coder",
    "deepseek-coder", "codellama", "codegemma", "starcoder2", "stable-code", "sqlcoder",
    "wizardcoder", "yi-coder", "granite-code", "nomic-embed-text", "mxbai-embed-large",
    "bge-m3", "all-minilm", "snowflake-arctic-embed", "qwen3-embedding", "llava", "bakllava",
    "moondream", "minicpm-v", "llama3.2-vision",
};
const std::vector<std::string> MLX_MODELS = {
    LOREA_DIR, LOREA_CYBER_DIR,
    "mlx-community/Qwen3.5-0.8B-OptiQ-4bit",
    "mlx-community/Qwen3.5-2B-OptiQ-4bit",
    "mlx-community/Qwen3.5-4B-OptiQ-4bit",
    "mlx-community/Qwen2.5-Coder-7B-Instruct-4bit",
    "mlx-community/Qwen3-8B-4bit",
    "mlx-community/gemma-3-4b-it-4bit",
    "mlx-community/Qwen3.5-9B-MLX-4bit",
    "mlx-community/Qwen3.6-35B-A3B-4bit",
    "mlx-community/Qwen3.6-40B-Claude-4.6-Opus-Deckard-Heretic-Uncensored-Thinking-8bit",
    "mlx-community/Qwen3-Coder-Next-nvfp4",
    "mlx-community/Qwen3-Coder-Next-mxfp8",
    "mlx-community/Qwen3-Coder-Next-mxfp4",
    "mlx-community/gemma-4-e2b-it-OptiQ-4bit",
    "mlx-community/gemma-4-e4b-it-OptiQ-4bit",
    "mlx-community/gemma-4-26B-A4B-it-assistant-bf16",
    "mlx-community/gemma-4-31B-it-assistant-bf16",
    "mlx-community/Nemotron-Mini-4B-Instruct-4bit-mlx",
    "mlx-community/Nemotron-Mini-4B-Instruct-bf16-mlx",
    "mlx-community/granite-4.1-8b-4bit",
    "mlx-community/granite-4.1-8b-5bit",
    "mlx-community/granite-4.1-8b-6bit",
    "mlx-community/granite-4.1-8b-8bit",
    "mlx-community/granite-4.1-8b-nvfp4",
    "mlx-community/granite-4.1-8b-mxfp8",
    "mlx-community/GLM-4.5-Air-mxfp8",
    "mlx-community/GLM-4.5-Air-nvfp4",
    "mlx-community/DeepSeek-V4-Flash-4bit",
    "mlx-community/DeepSeek-V4-Flash-2bit-DQ",
};
const std::vector<std::string> GGUF_MODELS = {
    "Qwen2.5-Coder-1.5B-Instruct-GGUF", "Llama-3.1-8B-Instruct-GGUF",
    "Llama-3.2-3B-Instruct-GGUF", "Llama-3.3-70B-Instruct-GGUF",
    "Qwen2.5-Coder-7B-Instruct-GGUF", "Qwen2.5-Coder-14B-Instruct-GGUF",
    "Qwen2.5-Coder-32B-Instruct-GGUF", "Qwen3-8B-GGUF", "Qwen3-14B-GGUF",
    "Qwen3-32B-GGUF", "Qwen3-Coder-GGUF", "Mistral-7B-Instruct-GGUF",
    "Mistral-Nemo-12B-Instruct-GGUF", "Mixtral-8x7B-Instruct-GGUF",
    "Gemma-2-9B-Instruct-GGUF", "Gemma-3-GGUF", "Phi-3-mini-GGUF", "Phi-4-GGUF",
    "DeepSeek-R1-Distill-Qwen-7B-GGUF", "DeepSeek-R1-Distill-Qwen-14B-GGUF",
    "DeepSeek-R1-Distill-Qwen-32B-GGUF", "DeepSeek-Coder-V2-Lite-GGUF",
    "CodeLlama-7B-Instruct-GGUF", "CodeLlama-13B-Instruct-GGUF", "StarCoder2-7B-GGUF",
    "Yi-Coder-9B-GGUF", "Nous-Hermes-2-Mistral-7B-GGUF", "OpenHermes-2.5-Mistral-7B-GGUF",
    "Dolphin-Llama3-GGUF", "Dolphin-Mistral-GGUF", "froggeric/Qwen3.6-27B-MTP-GGUF",
    "Jiunsong/supergemma4-26b-uncensored-gguf-v2",
    "hesamation/Qwen3.6-35B-A3B-Claude-4.6-Opus-Reasoning-Distilled-GGUF",
    "kai-os/Carnice-V2-27b-GGUF", "AtomicChat/gemma-4-26B-A4B-it-assistant-GGUF",
    "LiquidAI/LFM2.5-1.2B-Thinking-GGUF", "LiquidAI/LFM2-24B-A2B-GGUF",
    "prism-ml/Bonsai-8B-gguf", "jgebbeken/gemma-4-coder-gguf", "qvac/MedPsy-4B-GGUF",
};
const std::vector<std::string> AIRLLM_MODELS = {
    "Qwen/Qwen2.5-72B-Instruct", "Qwen/Qwen2.5-Coder-32B-Instruct", "Qwen/Qwen2.5-7B-Instruct",
    "meta-llama/Llama-3.1-70B-Instruct", "meta-llama/Llama-3.1-405B-Instruct",
    "meta-llama/Llama-3-70B-Instruct", "meta-llama/Llama-2-70b-chat-hf",
    "mistralai/Mixtral-8x7B-Instruct-v0.1", "mistralai/Mistral-7B-Instruct-v0.3",
    "garage-bAInd/Platypus2-70B-instruct", "codellama/CodeLlama-70b-Instruct-hf",
    "google/gemma-2-27b-it", "deepseek-ai/DeepSeek-Coder-V2-Lite-Instruct",
};
const std::map<std::string, std::string> LLAMA_CPP_URLS = {
    {"Qwen2.5-Coder-1.5B-Instruct-GGUF",
     "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/main/"
     "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"},
};

}

const std::map<std::string, std::string> BACKEND_DEFAULT_URLS = {
    {"ollama",    "http://localhost:11434"},
    {"llama-cpp", "http://localhost:8080"},
    {"mlx",       "http://localhost:8080"},
    {"airllm",    ""},
    {"openai",    "https://api.openai.com"},
    {"anthropic", "https://api.anthropic.com"},
    {"nvidia",    "https://integrate.api.nvidia.com/v1"},
};

const std::map<std::string, std::string> BACKEND_DEFAULT_MODELS = {
    {"ollama",    "qwen3.6:27b-coding-nvfp4"},
    {"llama-cpp", LOREA_CYBER_GGUF},
    {"mlx",       LOREA_CYBER_DIR},
    {"airllm",    "Qwen/Qwen2.5-72B-Instruct"},
    {"openai",    "gpt-4o"},
    {"anthropic", "claude-sonnet-4-20250514"},
    {"nvidia",    "z-ai/glm-5.1"},
};

const std::set<std::string> CLOUD_BACKENDS = {"openai", "anthropic", "nvidia"};

const std::map<std::string, std::vector<std::string>> BACKEND_API_KEY_ENV = {
    {"openai",    {"OPENAI_API_KEY"}},
    {"anthropic", {"ANTHROPIC_API_KEY", "CLAUDE_API_KEY"}},
    {"nvidia",    {"NVIDIA_API_KEY", "NVAPI_KEY"}},
};

const std::map<std::string, std::vector<std::string>> MODEL_SUGGESTIONS = {
    {"ollama",    OLLAMA_MODELS},
    {"llama-cpp", GGUF_MODELS},
    {"mlx",       MLX_MODELS},
    {"airllm",    AIRLLM_MODELS},
    {"openai",    OPENAI_API_MODELS},
    {"anthropic", ANTHROPIC_API_MODELS},
};

const std::map<std::string, std::vector<DownloadOption>> DOWNLOAD_MODEL_OPTIONS = []() {
    std::map<std::string, std::vector<DownloadOption>> m;
    for (const auto& mdl : OLLAMA_MODELS)
        m["ollama"].push_back(DownloadOption{mdl, mdl, std::nullopt});
    for (const auto& mdl : MLX_MODELS)
        m["mlx"].push_back(DownloadOption{last_segment(mdl), mdl, std::nullopt});
    for (const auto& mdl : GGUF_MODELS) {
        auto it = LLAMA_CPP_URLS.find(mdl);
        std::optional<std::string> url =
            (it != LLAMA_CPP_URLS.end()) ? std::optional<std::string>(it->second) : std::nullopt;
        m["llama-cpp"].push_back(DownloadOption{last_segment(mdl), mdl, url});
    }
    for (const auto& mdl : AIRLLM_MODELS)
        m["airllm"].push_back(DownloadOption{last_segment(mdl), mdl, std::nullopt});
    return m;
}();

const std::vector<std::string> EFFORT_ORDER = {"basic", "tuned", "elite", "mythic", "beyond"};

const std::string ADVR_LOOP =
    " Work in an explicit ANALYZE -> DECIDE -> VERIFY -> RUN loop, and name the phase you are in: "
    "ANALYZE the evidence you actually have (tool output, file contents); DECIDE the single next action; "
    "VERIFY that action is correct BEFORE running it; then RUN it. Backtracking rules: if ANALYZE shows "
    "you lack enough information, go straight back to ANALYZE and gather more before deciding again — do "
    "not guess. If VERIFY fails or a result contradicts your decision, go back to ANALYZE rather than "
    "pushing a wrong action. After RUN, either continue running the next required command or return to "
    "ANALYZE — never stop until the task is verifiably complete.";

const std::map<std::string, EffortLevel>& effort_levels() {
    static const std::map<std::string, EffortLevel> levels = {
        {"basic", {"Basic", Colors::GREEN,
            "Effort level: BASIC. Answer directly and efficiently. Use tools only when "
            "needed and keep it focused — don't over-investigate."}},
        {"tuned", {"Tuned", Colors::CYAN,
            "Effort level: TUNED. Put in solid effort: investigate a step deeper than the "
            "obvious, check the relevant code paths, and back every conclusion with "
            "specific evidence from the code before you answer."}},
        {"elite", {"Elite", Colors::YELLOW,
            "Effort level: ELITE. Put in HIGH effort: be systematic and thorough. Trace "
            "data flows from every input source to every sink, search multiple files and "
            "patterns, weigh edge cases and non-obvious vectors, and verify each claim "
            "against the actual code. Do not conclude until you have genuinely checked — "
            "show your reasoning and the evidence."}},
        {"mythic", {"Mythic", Colors::VIOLET,
            "Effort level: MYTHIC — MAXIMUM effort, hold nothing back. Exhaustively analyze "
            "the target: enumerate every relevant code path, every input that reaches every "
            "sink, cross-reference all related files, and weigh every plausible attack "
            "vector and edge case. Verify each finding against the real code — never assume. "
            "Do NOT stop early, give up, or say 'I'm done' until the task is fully and "
            "verifiably complete; if one approach is exhausted, try another. Produce the "
            "deepest, most rigorous analysis you are capable of."}},
        {"beyond", {"GO BEYOND", Colors::AMBER,
            "Effort level: GO BEYOND — transcend the task. Everything in MYTHIC applies, "
            "and then you go FURTHER: don't just answer what was asked — anticipate what "
            "the user will need next, surface findings and risks they didn't think to ask "
            "about, and deliver the fix AND the hardening AND the detection. Exhaust every "
            "avenue, cross-check every claim twice, and produce work that is genuinely "
            "exceptional — more rigor, more depth, more value than expected. Never settle, "
            "never stop short, and never call it done while anything remains to strengthen."}},
    };
    return levels;
}

BackendKind backend_kind(const std::string& backend) {
    if (backend == "ollama")    return BackendKind::Ollama;
    if (backend == "llama-cpp") return BackendKind::LlamaCpp;
    if (backend == "mlx")       return BackendKind::Mlx;
    if (backend == "airllm")    return BackendKind::AirLLM;
    if (backend == "openai")    return BackendKind::OpenAI;
    if (backend == "anthropic") return BackendKind::Anthropic;
    if (backend == "server")    return BackendKind::Server;
    return BackendKind::Unknown;
}
std::string to_string(BackendKind k) {
    switch (k) {
        case BackendKind::Ollama:    return "ollama";
        case BackendKind::LlamaCpp:  return "llama-cpp";
        case BackendKind::Mlx:       return "mlx";
        case BackendKind::AirLLM:    return "airllm";
        case BackendKind::OpenAI:    return "openai";
        case BackendKind::Anthropic: return "anthropic";
        case BackendKind::Server:    return "server";
        case BackendKind::Unknown:   return "unknown";
    }
    return "unknown";
}

std::string msg_role(const Message& m) {
    if (!m.is_object()) return "";
    auto it = m.find("role");
    if (it == m.end() || it->is_null() || !it->is_string()) return "";
    return it->get<std::string>();
}
std::string msg_content(const Message& m) {
    if (!m.is_object()) return "";
    auto it = m.find("content");
    if (it == m.end() || it->is_null() || !it->is_string()) return "";
    return it->get<std::string>();
}
std::string msg_name(const Message& m) {
    if (!m.is_object()) return "";
    auto it = m.find("name");
    if (it == m.end() || it->is_null() || !it->is_string()) return "";
    return it->get<std::string>();
}
bool msg_has_tool_calls(const Message& m) {
    if (!m.is_object()) return false;
    auto it = m.find("tool_calls");
    if (it == m.end() || it->is_null()) return false;
    if (it->is_array() || it->is_object() || it->is_string()) return !it->empty();
    return true;
}
Message make_message(const std::string& role, const std::string& content) {
    return Message{{"role", role}, {"content", content}};
}

std::vector<std::string> shlex_split(const std::string& s) {
    static const std::string whitespace    = " \t\r\n";
    static const std::string quotes        = "'\"";
    static const std::string escape        = "\\";
    static const std::string escapedquotes = "\"";

    auto in_set = [](const std::string& set, char c) {
        return set.find(c) != std::string::npos;
    };

    const size_t n = s.size();
    size_t i = 0;
    char state = ' ';
    std::vector<std::string> result;

    auto read_token = [&]() -> std::optional<std::string> {
        std::string token;
        bool quoted = false;
        char escapedstate = 'a';
        for (;;) {
            bool eof = (i >= n);
            char nextchar = eof ? '\0' : s[i];
            if (!eof) ++i;

            if (state == '\0') {
                token.clear();
                break;
            } else if (state == ' ') {
                if (eof) { state = '\0'; break; }
                else if (in_set(whitespace, nextchar)) {
                    if (!token.empty() || quoted) break;
                    else continue;
                }
                else if (in_set(escape, nextchar)) { escapedstate = 'a'; state = nextchar; }
                else if (in_set(quotes, nextchar)) { state = nextchar; }
                else { token.push_back(nextchar); state = 'a'; }
            } else if (in_set(quotes, state)) {
                quoted = true;
                if (eof) throw ShlexError("No closing quotation");
                if (nextchar == state) { state = 'a'; }
                else if (in_set(escape, nextchar) && in_set(escapedquotes, state)) {
                    escapedstate = state; state = nextchar;
                }
                else { token.push_back(nextchar); }
            } else if (in_set(escape, state)) {
                if (eof) throw ShlexError("No escaped character");

                if (in_set(escapedquotes, escapedstate) &&
                    nextchar != state && nextchar != escapedstate) {
                    token.push_back(state);
                }
                token.push_back(nextchar);
                state = escapedstate;
            } else if (state == 'a') {
                if (eof) { state = '\0'; break; }
                else if (in_set(whitespace, nextchar)) {
                    state = ' ';
                    if (!token.empty() || quoted) break;
                    else continue;
                }
                else if (in_set(quotes, nextchar)) { state = nextchar; }
                else if (in_set(escape, nextchar)) { escapedstate = 'a'; state = nextchar; }
                else { token.push_back(nextchar); }
            }
        }
        if (!quoted && token.empty()) return std::nullopt;
        return token;
    };

    for (;;) {
        std::optional<std::string> t = read_token();
        if (!t.has_value()) break;
        result.push_back(*t);
    }
    return result;
}

std::string shlex_quote(const std::string& s) {
    if (s.empty()) return "''";
    bool safe = true;
    for (unsigned char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  (c != '\0' && std::strchr("_@%+=:,./-", c) != nullptr);
        if (!ok) { safe = false; break; }
    }
    if (safe) return s;
    std::string inner;
    inner.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'') inner += "'\"'\"'";
        else inner.push_back(c);
    }
    return "'" + inner + "'";
}

namespace {

const std::string SCHEME_CHARS =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.";

const std::set<std::string> USES_RELATIVE = {
    "", "ftp", "http", "gopher", "nntp", "imap", "wais", "file", "https", "shttp",
    "mms", "prospero", "rtsp", "rtspu", "sftp", "svn", "svn+ssh", "ws", "wss",
};
const std::set<std::string> USES_NETLOC = {
    "", "ftp", "http", "gopher", "nntp", "telnet", "imap", "wais", "file", "mms",
    "https", "shttp", "snews", "prospero", "rtsp", "rtspu", "rsync", "svn", "svn+ssh",
    "sftp", "nfs", "git", "git+ssh", "ws", "wss",
};
const std::set<std::string> USES_PARAMS = {
    "", "ftp", "hdl", "prospero", "http", "imap", "https", "shttp", "rtsp", "rtspu",
    "sip", "sips", "mms", "sftp", "tel",
};

std::string remove_unsafe_bytes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\t' && c != '\r' && c != '\n') out.push_back(c);
    return out;
}
std::string lstrip_c0_space(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (unsigned char)s[i] <= 0x20) ++i;
    return s.substr(i);
}
std::string strip_c0_space(const std::string& s) {
    size_t i = 0, j = s.size();
    while (i < j && (unsigned char)s[i] <= 0x20) ++i;
    while (j > i && (unsigned char)s[j - 1] <= 0x20) --j;
    return s.substr(i, j - i);
}

struct SplitResult {
    std::string scheme, netloc, path, query, fragment;
    bool valid = true;
};

SplitResult url_split(const std::string& url_in, const std::string& default_scheme) {
    SplitResult R;
    std::string url    = remove_unsafe_bytes(url_in);
    std::string scheme = remove_unsafe_bytes(default_scheme);
    url    = lstrip_c0_space(url);
    scheme = strip_c0_space(scheme);
    R.scheme = scheme;

    size_t colon = url.find(':');
    if (colon != std::string::npos && colon > 0) {
        bool ok = true;
        for (size_t k = 0; k < colon; ++k) {
            if (SCHEME_CHARS.find(url[k]) == std::string::npos) { ok = false; break; }
        }
        if (ok) {
            R.scheme = lower_str(url.substr(0, colon));
            url = url.substr(colon + 1);
        }
    }

    if (url.size() >= 2 && url[0] == '/' && url[1] == '/') {
        size_t end = url.size();
        for (size_t k = 2; k < url.size(); ++k) {
            char c = url[k];
            if (c == '/' || c == '?' || c == '#') { end = k; break; }
        }
        R.netloc = url.substr(2, end - 2);
        url = url.substr(end);
        bool hasOpen  = R.netloc.find('[') != std::string::npos;
        bool hasClose = R.netloc.find(']') != std::string::npos;
        if ((hasOpen && !hasClose) || (hasClose && !hasOpen)) R.valid = false;
    }

    size_t h = url.find('#');
    if (h != std::string::npos) { R.fragment = url.substr(h + 1); url = url.substr(0, h); }
    size_t q = url.find('?');
    if (q != std::string::npos) { R.query = url.substr(q + 1); url = url.substr(0, q); }

    R.path = url;
    return R;
}

struct Parse6 {
    std::string scheme, netloc, path, params, query, fragment;
    bool valid = true;
};

Parse6 url_parse6(const std::string& url, const std::string& default_scheme) {
    SplitResult s = url_split(url, default_scheme);
    Parse6 p;
    p.scheme = s.scheme; p.netloc = s.netloc; p.path = s.path;
    p.query = s.query; p.fragment = s.fragment; p.valid = s.valid;

    if (USES_PARAMS.count(p.scheme) && p.path.find(';') != std::string::npos) {

        const std::string& path = p.path;
        if (path.find('/') != std::string::npos) {
            size_t lastslash = path.rfind('/');
            size_t i = path.find(';', lastslash);
            if (i != std::string::npos) {
                p.params = path.substr(i + 1);
                p.path = path.substr(0, i);
            }

        } else {
            size_t i = path.find(';');
            p.params = path.substr(i + 1);
            p.path = path.substr(0, i);
        }
    }
    return p;
}

std::string url_unsplit(const std::string& scheme, const std::string& netloc,
                        std::string url, const std::string& query,
                        const std::string& fragment) {
    if (!netloc.empty() ||
        (!scheme.empty() && USES_NETLOC.count(scheme) && url.substr(0, 2) != "//")) {
        if (!url.empty() && url[0] != '/') url = "/" + url;
        url = "//" + netloc + url;
    }
    if (!scheme.empty()) url = scheme + ":" + url;
    if (!query.empty())  url = url + "?" + query;
    if (!fragment.empty()) url = url + "#" + fragment;
    return url;
}

std::string url_unparse6(const Parse6& p) {
    std::string url = p.path;
    if (!p.params.empty()) url = url + ";" + p.params;
    return url_unsplit(p.scheme, p.netloc, url, p.query, p.fragment);
}

}

ParsedUrl urlparse(const std::string& url) {
    ParsedUrl out;
    Parse6 p = url_parse6(url, "");
    out.valid  = p.valid;
    out.scheme = p.scheme;
    out.path   = p.path;
    out.query  = p.query;

    const std::string& netloc = p.netloc;
    size_t at = netloc.rfind('@');
    std::string userinfo, hostinfo;
    if (at != std::string::npos) { userinfo = netloc.substr(0, at); hostinfo = netloc.substr(at + 1); }
    else { hostinfo = netloc; }

    if (at != std::string::npos) {
        size_t colon = userinfo.find(':');
        if (colon != std::string::npos) {
            out.user = userinfo.substr(0, colon);
            out.password = userinfo.substr(colon + 1);
        } else {
            out.user = userinfo;
        }
    }

    std::string hostname, portstr;
    size_t br = hostinfo.find('[');
    if (br != std::string::npos) {
        std::string bracketed = hostinfo.substr(br + 1);
        size_t rb = bracketed.find(']');
        if (rb != std::string::npos) {
            hostname = bracketed.substr(0, rb);
            std::string rest = bracketed.substr(rb + 1);
            size_t pc = rest.find(':');
            if (pc != std::string::npos) portstr = rest.substr(pc + 1);
        } else {
            hostname = bracketed;
        }
    } else {
        size_t pc = hostinfo.find(':');
        if (pc != std::string::npos) { hostname = hostinfo.substr(0, pc); portstr = hostinfo.substr(pc + 1); }
        else hostname = hostinfo;
    }

    if (!hostname.empty()) {
        size_t pct = hostname.find('%');
        if (pct == std::string::npos) out.host = lower_str(hostname);
        else out.host = lower_str(hostname.substr(0, pct)) + hostname.substr(pct);
    }

    if (!portstr.empty()) {
        bool digits = true;
        for (char c : portstr) if (!(c >= '0' && c <= '9')) { digits = false; break; }
        if (digits) {
            errno = 0;
            long pv = std::strtol(portstr.c_str(), nullptr, 10);
            if (errno == 0 && pv >= 0 && pv <= 65535) out.port = (int)pv;
        }
    }
    return out;
}

std::string urljoin(const std::string& base, const std::string& ref) {
    if (base.empty()) return ref;
    if (ref.empty())  return base;

    Parse6 b = url_parse6(base, "");
    Parse6 u = url_parse6(ref, b.scheme);

    std::string scheme   = u.scheme;
    std::string netloc   = u.netloc;
    std::string path     = u.path;
    std::string params   = u.params;
    std::string query    = u.query;
    std::string fragment = u.fragment;

    if (scheme != b.scheme || USES_RELATIVE.count(scheme) == 0) return ref;

    if (USES_NETLOC.count(scheme)) {
        if (!netloc.empty()) {
            Parse6 r{scheme, netloc, path, params, query, fragment, true};
            return url_unparse6(r);
        }
        netloc = b.netloc;
    }

    if (path.empty() && params.empty()) {
        path = b.path;
        params = b.params;
        if (query.empty()) query = b.query;
        Parse6 r{scheme, netloc, path, params, query, fragment, true};
        return url_unparse6(r);
    }

    std::vector<std::string> base_parts = split_str(b.path, '/');
    if (!base_parts.empty() && !base_parts.back().empty())
        base_parts.pop_back();

    std::vector<std::string> segments;
    if (!path.empty() && path[0] == '/') {
        segments = split_str(path, '/');
    } else {
        segments = base_parts;
        std::vector<std::string> tail = split_str(path, '/');
        segments.insert(segments.end(), tail.begin(), tail.end());

        if (segments.size() >= 2) {
            std::vector<std::string> ns;
            ns.push_back(segments.front());
            for (size_t k = 1; k + 1 < segments.size(); ++k)
                if (!segments[k].empty()) ns.push_back(segments[k]);
            ns.push_back(segments.back());
            segments.swap(ns);
        }
    }

    std::vector<std::string> resolved;
    for (const std::string& seg : segments) {
        if (seg == "..") {
            if (!resolved.empty()) resolved.pop_back();
        } else if (seg == ".") {
            continue;
        } else {
            resolved.push_back(seg);
        }
    }
    if (!segments.empty() && (segments.back() == "." || segments.back() == ".."))
        resolved.push_back("");

    std::string joined;
    for (size_t k = 0; k < resolved.size(); ++k) {
        if (k) joined += "/";
        joined += resolved[k];
    }
    if (joined.empty()) joined = "/";

    Parse6 r{scheme, netloc, joined, params, query, fragment, true};
    return url_unparse6(r);
}

std::string expanduser(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    size_t i = path.find('/', 1);
    if (i == std::string::npos) i = path.size();

    std::string userhome;
    if (i == 1) {
        const char* home = std::getenv("HOME");
        if (home) {
            userhome = home;
        } else {
            struct passwd* pw = getpwuid(getuid());
            if (pw && pw->pw_dir) userhome = pw->pw_dir;
            else return path;
        }
    } else {
        std::string name = path.substr(1, i - 1);
        struct passwd* pw = getpwnam(name.c_str());
        if (pw && pw->pw_dir) userhome = pw->pw_dir;
        else return path;
    }

    while (!userhome.empty() && userhome.back() == '/') userhome.pop_back();
    std::string result = userhome + path.substr(i);
    return result.empty() ? std::string("/") : result;
}

namespace {

std::string normpath(const std::string& path_in) {
    if (path_in.empty()) return ".";
    const std::string& path = path_in;
    int initial_slashes = (path[0] == '/') ? 1 : 0;
    if (initial_slashes && path.rfind("//", 0) == 0 && path.rfind("///", 0) != 0)
        initial_slashes = 2;

    std::vector<std::string> comps = split_str(path, '/');
    std::vector<std::string> new_comps;
    for (const std::string& comp : comps) {
        if (comp.empty() || comp == ".") continue;
        if (comp != ".." ||
            (!initial_slashes && new_comps.empty()) ||
            (!new_comps.empty() && new_comps.back() == "..")) {
            new_comps.push_back(comp);
        } else if (!new_comps.empty()) {
            new_comps.pop_back();
        }
    }
    std::string out;
    for (size_t k = 0; k < new_comps.size(); ++k) {
        if (k) out += "/";
        out += new_comps[k];
    }
    if (initial_slashes) out = std::string(initial_slashes, '/') + out;
    return out.empty() ? "." : out;
}

std::string lexical_abspath(const std::string& p) {
    std::string path = p;
    bool isabs = !path.empty() && path[0] == '/';
    if (!isabs) {
        char cwdbuf[PATH_MAX];
        std::string base = (::getcwd(cwdbuf, sizeof(cwdbuf)) ? std::string(cwdbuf)
                                                             : std::string("."));
        if (path.empty()) {
            path = (!base.empty() && base.back() != '/') ? base + "/" : base;
        } else if (!base.empty() && base.back() == '/') {
            path = base + path;
        } else {
            path = base + "/" + path;
        }
    }
    return normpath(path);
}

}

std::string realpath_str(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) != nullptr) return std::string(buf);
    return lexical_abspath(path);
}

namespace {

double seq_matcher_ratio(const std::u32string& A, const std::u32string& B) {
    const int la = (int)A.size();
    const int lb = (int)B.size();
    const int T  = la + lb;
    if (T == 0) return 1.0;

    std::unordered_map<char32_t, std::vector<int>> b2j;
    for (int j = 0; j < lb; ++j) b2j[B[j]].push_back(j);

    if (lb >= 200) {
        int ntest = lb / 100 + 1;
        std::vector<char32_t> popular;
        for (const auto& kv : b2j)
            if ((int)kv.second.size() > ntest) popular.push_back(kv.first);
        for (char32_t e : popular) b2j.erase(e);
    }

    long matches = 0;
    struct Range { int alo, ahi, blo, bhi; };
    std::vector<Range> queue;
    queue.push_back({0, la, 0, lb});

    while (!queue.empty()) {
        Range r = queue.back();
        queue.pop_back();
        const int alo = r.alo, ahi = r.ahi, blo = r.blo, bhi = r.bhi;

        int besti = alo, bestj = blo, bestsize = 0;
        std::unordered_map<int, int> j2len;
        for (int i = alo; i < ahi; ++i) {
            std::unordered_map<int, int> newj2len;
            auto it = b2j.find(A[i]);
            if (it != b2j.end()) {
                for (int j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    int k = 1;
                    auto pit = j2len.find(j - 1);
                    if (pit != j2len.end()) k = pit->second + 1;
                    newj2len[j] = k;
                    if (k > bestsize) { besti = i - k + 1; bestj = j - k + 1; bestsize = k; }
                }
            }
            j2len.swap(newj2len);
        }

        while (besti > alo && bestj > blo && A[besti - 1] == B[bestj - 1]) {
            --besti; --bestj; ++bestsize;
        }
        while (besti + bestsize < ahi && bestj + bestsize < bhi &&
               A[besti + bestsize] == B[bestj + bestsize]) {
            ++bestsize;
        }

        if (bestsize) {
            matches += bestsize;
            if (alo < besti && blo < bestj)
                queue.push_back({alo, besti, blo, bestj});
            if (besti + bestsize < ahi && bestj + bestsize < bhi)
                queue.push_back({besti + bestsize, ahi, bestj + bestsize, bhi});
        }
    }
    return 2.0 * (double)matches / (double)T;
}

}

double difflib_ratio(const std::string& a, const std::string& b) {
    std::u32string A = utf8_to_u32(a);
    std::u32string B = utf8_to_u32(b);
    if (A.size() > 4000) A.resize(4000);
    if (B.size() > 4000) B.resize(4000);
    return seq_matcher_ratio(A, B);
}

namespace {

struct SigpipeIgnorer {
    SigpipeIgnorer() { ::signal(SIGPIPE, SIG_IGN); }
};
void ensure_sigpipe_ignored() { static SigpipeIgnorer once; (void)once; }

int returncode_from_status(int status) {
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

std::mutex g_status_mu;
std::map<const Subprocess*, int> g_status;

void set_nonblocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl != -1) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

ProcResult run_argv(const std::vector<std::string>& argv, const std::string& input,
                    double timeout_s, bool combine_stderr) {
    ProcResult R;
    if (argv.empty()) return R;
    ensure_sigpipe_ignored();

    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0) return R;
    if (::pipe(out_pipe) != 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); return R; }
    if (!combine_stderr) {
        if (::pipe(err_pipe) != 0) {
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            return R;
        }
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        if (!combine_stderr) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
        return R;
    }
    if (pid == 0) {

        ::signal(SIGPIPE, SIG_DFL);
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(combine_stderr ? out_pipe[1] : err_pipe[1], STDERR_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        if (!combine_stderr) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }

    R.started = true;
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    if (!combine_stderr) ::close(err_pipe[1]);

    int in_w  = in_pipe[1];
    int out_r = out_pipe[0];
    int err_r = combine_stderr ? -1 : err_pipe[0];

    set_nonblocking(in_w);
    set_nonblocking(out_r);
    if (err_r != -1) set_nonblocking(err_r);

    bool in_open  = true;
    bool out_open = true;
    bool err_open = (err_r != -1);
    size_t in_off = 0;
    if (input.empty()) { ::close(in_w); in_w = -1; in_open = false; }

    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    while (in_open || out_open || err_open) {
        std::vector<struct pollfd> pfds;
        if (in_open)  pfds.push_back({in_w,  POLLOUT, 0});
        if (out_open) pfds.push_back({out_r, POLLIN,  0});
        if (err_open) pfds.push_back({err_r, POLLIN,  0});

        int timeout_ms = -1;
        if (timeout_s > 0.0) {
            double elapsed = std::chrono::duration<double>(clock::now() - start).count();
            double remaining = timeout_s - elapsed;
            if (remaining <= 0.0) { R.timed_out = true; ::kill(pid, SIGKILL); break; }
            timeout_ms = (int)(remaining * 1000.0);
            if (timeout_ms < 0) timeout_ms = 0;
        }

        int rv = ::poll(pfds.data(), pfds.size(), timeout_ms);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv == 0) {
            if (timeout_s > 0.0) { R.timed_out = true; ::kill(pid, SIGKILL); break; }
            continue;
        }

        for (const struct pollfd& pfd : pfds) {
            if (in_open && pfd.fd == in_w &&
                (pfd.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    ::close(in_w); in_w = -1; in_open = false;
                } else {
                    size_t chunk = std::min<size_t>(65536, input.size() - in_off);
                    ssize_t w = ::write(in_w, input.data() + in_off, chunk);
                    if (w > 0) in_off += (size_t)w;
                    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        ::close(in_w); in_w = -1; in_open = false;
                    }
                    if (in_open && in_off >= input.size()) {
                        ::close(in_w); in_w = -1; in_open = false;
                    }
                }
            } else if (out_open && pfd.fd == out_r &&
                       (pfd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
                char buf[65536];
                ssize_t n = ::read(out_r, buf, sizeof(buf));
                if (n > 0) R.out.append(buf, (size_t)n);
                else if (n == 0) { ::close(out_r); out_r = -1; out_open = false; }
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ::close(out_r); out_r = -1; out_open = false;
                }
            } else if (err_open && pfd.fd == err_r &&
                       (pfd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
                char buf[65536];
                ssize_t n = ::read(err_r, buf, sizeof(buf));
                if (n > 0) R.err.append(buf, (size_t)n);
                else if (n == 0) { ::close(err_r); err_r = -1; err_open = false; }
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ::close(err_r); err_r = -1; err_open = false;
                }
            }
        }
    }

    if (in_w  != -1) ::close(in_w);
    if (out_r != -1) ::close(out_r);
    if (err_r != -1) ::close(err_r);

    int status = 0;
    pid_t wr;
    do { wr = ::waitpid(pid, &status, 0); } while (wr < 0 && errno == EINTR);
    R.exit_code = (wr == pid) ? returncode_from_status(status) : -1;
    return R;
}

}

ProcResult run_subprocess(const std::vector<std::string>& argv, const std::string& input,
                          double timeout_s, bool combine_stderr) {
    return run_argv(argv, input, timeout_s, combine_stderr);
}

ProcResult run_shell(const std::string& command, double timeout_s) {

    return run_argv({"/bin/sh", "-c", command}, "", timeout_s, true);
}

std::optional<int> Subprocess::poll() {
    if (pid <= 0) return std::nullopt;
    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        auto it = g_status.find(this);
        if (it != g_status.end()) return it->second;
    }
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == 0) return std::nullopt;
    int rc = (r == pid) ? returncode_from_status(status) : -1;
    std::lock_guard<std::mutex> lk(g_status_mu);
    g_status[this] = rc;
    return rc;
}

int Subprocess::wait(double timeout_s) {
    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        auto it = g_status.find(this);
        if (it != g_status.end()) return it->second;
    }
    if (pid <= 0) return -1;

    if (timeout_s < 0) {
        int status = 0;
        pid_t r;
        do { r = ::waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
        int rc = (r == pid) ? returncode_from_status(status) : -1;
        std::lock_guard<std::mutex> lk(g_status_mu);
        g_status[this] = rc;
        return rc;
    }

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::duration<double>(timeout_s);
    for (;;) {
        std::optional<int> p = poll();
        if (p.has_value()) return *p;
        if (clock::now() >= deadline) return INT_MIN;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void Subprocess::terminate() {
    if (pid > 0) ::kill(pid, SIGTERM);
}
void Subprocess::kill() {
    if (pid > 0) ::kill(pid, SIGKILL);
}
bool Subprocess::alive() {
    return !poll().has_value();
}

std::shared_ptr<Subprocess> spawn_process(const std::vector<std::string>& argv,
                                          bool devnull_output) {
    auto sp = std::make_shared<Subprocess>();
    if (argv.empty()) return sp;
    ensure_sigpipe_ignored();

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) return sp;
    if (pid == 0) {

        ::signal(SIGPIPE, SIG_DFL);
        if (devnull_output) {
            int dn = ::open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                ::dup2(dn, STDOUT_FILENO);
                ::dup2(dn, STDERR_FILENO);
                if (dn > STDERR_FILENO) ::close(dn);
            }
        }
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }

    sp->pid = pid;
    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        g_status.erase(sp.get());
    }
    return sp;
}

}
