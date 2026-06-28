#include "lorea.hpp"

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <system_error>
#include <iostream>
#include <cstdlib>

namespace ocli {

namespace {

namespace fs = std::filesystem;

std::string sys_executable() {
    const char* p = std::getenv("PYTHON");
    if (p && *p) return p;
    return "python3";
}

bool truthy(const std::optional<std::string>& v) {
    return v.has_value() && !v->empty();
}

std::string falsy_str(const json& v) {
    if (v.is_null()) return "";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "True" : "";
    if (v.is_number_integer()) {
        long long n = v.get<long long>();
        return n ? std::to_string(n) : "";
    }
    if (v.is_number_float()) {
        double d = v.get<double>();
        return d ? v.dump() : "";
    }
    if (v.is_array() || v.is_object()) {
        return v.empty() ? "" : v.dump();
    }
    return v.dump();
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string rstrip_slash(const std::string& s) {
    size_t b = s.size();
    while (b > 0 && s[b - 1] == '/') --b;
    return s.substr(0, b);
}

std::string last_path_segment(const std::string& s) {
    auto pos = s.rfind('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::string path_join(const std::string& a, const std::string& b) {
    return (fs::path(a) / b).string();
}
std::string path_join(const std::string& a, const std::string& b, const std::string& c) {
    return (fs::path(a) / b / c).string();
}

std::string basename_of(const std::string& s) {
    return fs::path(s).filename().string();
}

std::string abspath(const std::string& path) {
    std::error_code ec;
    fs::path p(path);
    if (!p.is_absolute()) {
        fs::path cwd = fs::current_path(ec);
        p = cwd / p;
    }
    std::string out = p.lexically_normal().string();

    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string before_query(const std::string& url) {
    auto pos = url.find('?');
    return pos == std::string::npos ? url : url.substr(0, pos);
}

std::string called_process_error(const std::vector<std::string>& argv,
                                 const ProcResult& r) {
    if (!r.started) {
        return "[Errno 2] No such file or directory: '" +
               (argv.empty() ? std::string() : argv[0]) + "'";
    }
    std::string joined = "[";
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) joined += ", ";
        joined += "'" + argv[i] + "'";
    }
    joined += "]";
    return "Command '" + joined + "' returned non-zero exit status " +
           std::to_string(r.exit_code) + ".";
}

}

void LOREA::loop_command(const std::string& goal_in) {
    std::string goal = strip(goal_in);
    if (goal.empty()) {
        log_info("Usage: /loop <goal>  — keep working autonomously until done.");
        log_info(std::string(Colors::DIM) + Colors::GRAY +
                 "The agent loops, making concrete progress each pass. Press Esc or Ctrl-C to stop." +
                 Colors::RESET);
        return;
    }

    last_user_goal = goal;
    session_turns = session_turns + 1;

    last_tool_signature = std::nullopt;
    repeated_tool_count = 0;
    last_failure_signature = std::nullopt;
    repeated_failure_count = 0;

    std::string seed =
        goal + "\n\n"
        "Work on this autonomously, one concrete step at a time, using real tool calls "
        "(write_file, run_cmd, test_cmd, read_file, …). After each step, take the next one. "
        "Only when the ENTIRE task is genuinely finished and verified, end your final message "
        "with this exact line on its own:\n" + LOOP_SENTINEL + "\n"
        "Do not write that line until everything is truly done.";
    messages.push_back(make_message("user", seed));

    std::string indent = left_indent();
    std::cout << "\n" << indent << ACCENT << Colors::BOLD << "↻ loop" << Colors::RESET
              << " " << Colors::WHITE << utf8_substr(goal, 0, 80)
              << (utf8_len(goal) > 80 ? "…" : "") << Colors::RESET << "\n";
    std::cout << indent << Colors::DIM << Colors::GRAY
              << "Working until done · press Esc or Ctrl-C to stop" << Colors::RESET << "\n\n";

    int stale = 0;
    std::optional<std::string> stopped;
    try {
        for (int iteration = 1; iteration <= LOOP_MAX_ITERATIONS; ++iteration) {
            if (interrupter.interrupted.is_set()) {
                stopped = "interrupted";
                break;
            }
            std::cout << indent << ACCENT << "↻" << Colors::RESET << "  "
                      << Colors::DIM << Colors::GRAY << "loop" << Colors::RESET
                      << "  iteration " << Colors::WHITE << iteration << Colors::RESET << "\n";
            tool_steps_this_turn = 0;
            int tools_before = 0;
            for (const auto& m : messages)
                if (msg_role(m) == "tool") ++tools_before;

            compact_history();
            process_chat();

            if (interrupter.interrupted.is_set()) {
                stopped = "interrupted";
                break;
            }

            std::string content;
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (msg_role(*it) == "assistant") {
                    if (it->contains("content"))
                        content = falsy_str((*it)["content"]);
                    break;
                }
            }
            std::string answer = visible_answer(content);
            int tools_after = 0;
            for (const auto& m : messages)
                if (msg_role(m) == "tool") ++tools_after;
            bool made_progress = tools_after > tools_before;

            if (to_lower(content).find(to_lower(LOOP_SENTINEL)) != std::string::npos) {
                stopped = "complete";
                break;
            }

            if (!answer.empty() && !made_progress &&
                std::regex_search(answer, COMPLETION_RE) &&
                !promises_next_action(answer)) {
                stopped = "complete";
                break;
            }

            stale = made_progress ? 0 : stale + 1;
            if (stale >= 3) {
                stopped = "stalled";
                break;
            }

            messages.push_back(make_message("user",
                "Continue working on the task. Original goal:\n" + goal + "\n"
                "Take the next concrete step now with a real tool call. Do not repeat a step you already did. "
                "If the task is fully complete and verified, give a short final summary and end with the line:\n" +
                LOOP_SENTINEL));
        }
        if (!stopped.has_value())
            stopped = "max";
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()) == "KeyboardInterrupt")
            stopped = "interrupted";
        else
            throw;
    }

    std::cout << "\n";
    if (stopped == "complete") {
        std::string sub = utf8_substr(goal, 0, 60) + (utf8_len(goal) > 60 ? "…" : "");
        celebrate("LOOP COMPLETE", sub.c_str());
    } else if (stopped == "interrupted") {
        log_info(std::string("Loop stopped. ") + Colors::DIM + Colors::GRAY +
                 "Type /loop <goal> to start again, or just keep chatting." + Colors::RESET);
    } else if (stopped == "stalled") {
        log_warn("Loop made no progress for 3 passes; stopping. Refine the goal and try again.");
    } else if (stopped == "max") {
        log_warn("Reached the " + std::to_string(LOOP_MAX_ITERATIONS) +
                 "-iteration loop cap; stopping.");
    }
}

std::pair<std::optional<std::string>, std::optional<std::string>>
LOREA::parse_download_args(const std::string& arg_text) {
    if (arg_text.empty())
        return {std::nullopt, std::nullopt};

    std::vector<std::string> parts;
    try {
        parts = shlex_split(arg_text);
    } catch (const ShlexError& e) {
        log_info(std::string("Could not parse download arguments: ") + e.what());
        std::string stripped = strip(arg_text);
        return {stripped, std::nullopt};
    }

    std::optional<std::string> path;
    std::vector<std::string> model_parts;
    size_t i = 0;
    while (i < parts.size()) {
        const std::string& part = parts[i];
        if (part == "--path" || part == "--dir" || part == "--local-dir" || part == "-p") {
            if (i + 1 >= parts.size()) {
                log_info("Missing path after " + part + ".");
                break;
            }
            path = parts[i + 1];
            i += 2;
            continue;
        }
        if (starts_with(part, "--path=") || starts_with(part, "--dir=") ||
            starts_with(part, "--local-dir=")) {
            path = part.substr(part.find('=') + 1);
            i += 1;
            continue;
        }
        model_parts.push_back(part);
        i += 1;
    }
    std::string joined;
    for (size_t k = 0; k < model_parts.size(); ++k) {
        if (k) joined += " ";
        joined += model_parts[k];
    }
    joined = strip(joined);
    std::optional<std::string> model = joined.empty() ? std::nullopt
                                                      : std::optional<std::string>(joined);
    return {model, path};
}

std::string LOREA::model_download_default_dir(const std::string& model_name,
                                              const std::string& url) {
    if (backend == "mlx") {
        std::string base = model_name.empty() ? "model" : model_name;
        std::string name = last_path_segment(rstrip_slash(base));
        if (name.empty()) name = "model";
        return path_join("models", "mlx", name);
    }
    if (backend == "airllm") {
        std::string base = model_name.empty() ? "model" : model_name;
        std::string name = last_path_segment(rstrip_slash(base));
        if (name.empty()) name = "model";
        return path_join("models", "airllm", name);
    }
    if (backend == "llama-cpp") {
        if (!model_name.empty() && model_name.find('/') != std::string::npos && url.empty()) {
            std::string name = last_path_segment(rstrip_slash(model_name));
            if (name.empty()) name = "model";
            return path_join("llama.cpp", "models", name);
        }
        return path_join("llama.cpp", "models");
    }
    return "";
}

std::optional<std::string> LOREA::normalize_download_dir(const std::string& path) {
    if (path.empty())
        return std::nullopt;
    return abspath(expanduser(path));
}

std::optional<std::string> LOREA::prompt_download_dir(const std::string& model_name,
                                                      const std::string& url) {
    if (backend == "ollama") {
        log_info("Ollama stores models in its configured model directory; set OLLAMA_MODELS before starting Ollama to change it.");
        return std::nullopt;
    }
    std::string def = model_download_default_dir(model_name, url);
    std::optional<std::string> path = prompt_value("Download path", def);
    return normalize_download_dir(path.value_or(""));
}

std::optional<std::string> LOREA::run_model_download(const std::string& model_name,
                                                     std::optional<std::string> url,
                                                     std::optional<std::string> download_dir,
                                                     bool ask_for_path) {
    if (model_name.empty())
        return std::nullopt;
    log_info(std::string("Download target: ") + Colors::TEAL + model_name + Colors::RESET);

    if (backend == "ollama") {
        if (truthy(download_dir))
            log_info("Ollama does not support a per-download path; using the active Ollama model store.");
        run_cmd("ollama pull " + shlex_quote(model_name));
    } else if (backend == "mlx") {
        if (ask_for_path && !truthy(download_dir))
            download_dir = prompt_download_dir(model_name, url.value_or(""));
        std::string result = download_mlx_model(model_name, download_dir.value_or(""));
        if (truthy(download_dir)) {
            std::string norm = normalize_download_dir(*download_dir).value_or("");
            log_info(std::string("To use this local MLX model, run ") + Colors::TEAL +
                     "/model " + norm + Colors::RESET);
        }
        return result;
    } else if (backend == "airllm") {
        if (ask_for_path && !truthy(download_dir))
            download_dir = prompt_download_dir(model_name, url.value_or(""));
        download_dir = normalize_download_dir(download_dir.value_or(""));
        if (truthy(download_dir)) {
            std::error_code ec;
            fs::create_directories(*download_dir, ec);
            std::string result = run_cmd("hf download " + shlex_quote(model_name) +
                                         " --local-dir " + shlex_quote(*download_dir));
            log_info(std::string("To use this local AirLLM model, run ") + Colors::TEAL +
                     "/model " + *download_dir + Colors::RESET);
            return result;
        }
        return run_cmd("hf download " + shlex_quote(model_name));
    } else if (backend == "llama-cpp") {
        if (ask_for_path && !truthy(download_dir))
            download_dir = prompt_download_dir(model_name, url.value_or(""));
        std::string chosen = truthy(download_dir)
                                 ? *download_dir
                                 : model_download_default_dir(model_name, url.value_or(""));
        std::string dir = normalize_download_dir(chosen).value_or("");
        if (!truthy(url) && model_name.find('/') != std::string::npos) {
            std::error_code ec;
            fs::create_directories(dir, ec);
            run_cmd("hf download " + shlex_quote(model_name) +
                    " --include '*.gguf' --local-dir " + shlex_quote(dir));
            return std::nullopt;
        }
        if (!truthy(url))
            url = prompt_value("GGUF download URL");
        if (!truthy(url)) {
            log_info("Download canceled.");
            return std::nullopt;
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string filename;
        if (ends_with(model_name, ".gguf")) {
            filename = model_name;
        } else {
            filename = basename_of(before_query(*url));
            if (filename.empty()) filename = model_name + ".gguf";
        }
        run_cmd("curl -L -o " + shlex_quote(path_join(dir, filename)) + " " + shlex_quote(*url));
    } else {
        log_info(std::string("Model download is not configured for backend ") + Colors::TEAL +
                 backend + Colors::RESET + ".");
    }
    return std::nullopt;
}

void LOREA::download_model_menu(std::optional<std::string> model_name,
                                std::optional<std::string> download_dir) {
    if (truthy(model_name)) {
        run_model_download(*model_name, std::nullopt, download_dir);
        return;
    }

    std::vector<DownloadOption> choices;
    auto it = DOWNLOAD_MODEL_OPTIONS.find(backend);
    if (it != DOWNLOAD_MODEL_OPTIONS.end())
        choices = it->second;

    std::vector<std::string> options;
    for (size_t i = 0; i < choices.size(); ++i) {
        options.push_back(std::to_string(i + 1) + ". " + Colors::CYAN + choices[i].label +
                          Colors::RESET + " " + Colors::GRAY + choices[i].value + Colors::RESET);
    }
    options.push_back(std::to_string(options.size() + 1) + ". " + Colors::ORANGE +
                      "Type a custom model or URL" + Colors::RESET);

    std::optional<int> choice = menu_choice("DOWNLOAD MODEL", options);
    if (!choice.has_value())
        return;
    if (*choice < static_cast<int>(choices.size())) {
        const DownloadOption& opt = choices[*choice];
        run_model_download(opt.value, opt.url, download_dir);
        return;
    }
    if (backend == "llama-cpp") {
        std::optional<std::string> url = prompt_value("GGUF download URL");
        if (!truthy(url)) {
            log_info("Download canceled.");
            return;
        }
        std::string def = basename_of(before_query(*url));
        if (def.empty()) def = "model.gguf";
        std::optional<std::string> save_as = prompt_value("Save as", def);
        run_model_download(save_as.value_or(""), url, download_dir);
        return;
    }
    std::optional<std::string> mn = prompt_value("Model to download", this->model_name);
    run_model_download(mn.value_or(""), std::nullopt, download_dir);
}

std::string LOREA::setup_llama_cpp() {
    log_info("Starting llama.cpp Automation Setup for macOS...");

    {
        std::vector<std::string> argv = {"clang", "--version"};
        ProcResult r = run_subprocess(argv);
        if (!r.started || r.exit_code != 0) {
            std::cout << "  " << status_label("ERROR", Colors::RED)
                      << " Xcode Command Line Tools not found.\n";
            std::cout << "  " << status_label("FIX", Colors::ORANGE) << " Run: "
                      << Colors::WHITE << "xcode-select --install" << Colors::RESET << "\n";
            return "Setup aborted: Xcode tools missing.";
        }
    }

    {
        ProcResult brew_check = run_subprocess({"which", "brew"});
        if (brew_check.exit_code != 0) {
            log_info("Installing Homebrew...");
            run_cmd("/bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"");
        }
    }

    log_info("Installing dependencies (git, cmake)...");
    run_cmd("brew install git cmake");

    {
        std::error_code ec;
        if (!fs::exists("llama.cpp", ec)) {
            log_info("Cloning llama.cpp repository...");
            run_cmd("git clone https://github.com/ggerganov/llama.cpp");
        }
    }

    log_info("Building llama.cpp with Metal acceleration...");
    std::string build_cmd =
        "cd llama.cpp && cmake -B build -DGGML_METAL=ON && cmake --build build --config Release";
    run_cmd(build_cmd);

    log_info("Setting up models directory and downloading a starter model...");
    std::string starter_filename = "qwen2.5-coder-1.5b.gguf";
    std::string starter_dir =
        normalize_download_dir(
            prompt_value("Starter model download path", path_join("llama.cpp", "models"))
                .value_or(""))
            .value_or("");
    std::string starter_path = path_join(starter_dir, starter_filename);
    std::string starter_url =
        "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/main/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf";
    std::string model_setup = "mkdir -p " + shlex_quote(starter_dir) + " && curl -L -o " +
                              shlex_quote(starter_path) + " " + shlex_quote(starter_url);
    run_cmd(model_setup);

    log_info("llama.cpp setup complete!");
    std::cout << "\n  " << status_label("SUCCESS", Colors::GREEN) << " llama.cpp is ready!\n";
    std::cout << "  " << Colors::CYAN << "To start the server:" << Colors::RESET
              << " cd llama.cpp && ./build/bin/llama-server -m " << starter_path << "\n";
    std::cout << "  " << Colors::CYAN << "To use with LOREA:" << Colors::RESET
              << " python3 OCLI.py --backend llama-cpp --model " << starter_path << "\n";
    return "llama.cpp setup successful.";
}

std::string LOREA::setup_mlx() {
    log_info("Starting MLX Automation Setup for macOS...");
    {
        std::vector<std::string> argv = {sys_executable(), "-m", "pip", "install", "-U",
                                         "mlx-lm", "huggingface_hub", "--break-system-packages"};
        ProcResult r = run_subprocess(argv);
        if (!r.started || r.exit_code != 0) {
            std::cout << "  " << status_label("ERROR", Colors::RED)
                      << " Failed to install mlx-lm: " << called_process_error(argv, r) << "\n";
            return "Setup failed.";
        }
    }

    log_info("Downloading recommended MLX model (Qwen2.5-Coder-7B-Instruct)...");
    std::string model_name = "mlx-community/Qwen2.5-Coder-7B-Instruct-4bit";
    std::string model_dir =
        normalize_download_dir(
            prompt_value("Recommended model download path",
                         path_join("models", "mlx", last_path_segment(model_name)))
                .value_or(""))
            .value_or("");
    try {
        std::error_code ec;
        bool created = fs::create_directories(model_dir, ec);
        if (!created && ec)
            throw std::runtime_error(ec.message());
        run_cmd(shlex_quote(sys_executable()) +
                " -m huggingface_hub.commands.cli download " + shlex_quote(model_name) +
                " --local-dir " + shlex_quote(model_dir));
    } catch (const std::exception& e) {
        log_info(std::string("Model download check failed: ") + e.what());
    }

    log_info("MLX setup complete!");
    std::cout << "\n  " << status_label("SUCCESS", Colors::GREEN) << " MLX (mlx-lm) is ready!\n";
    std::cout << "  " << Colors::CYAN << "To start the server:" << Colors::RESET
              << " python3 -m mlx_lm.server --model " << model_dir << "\n";
    std::cout << "  " << Colors::CYAN << "To use with LOREA:" << Colors::RESET
              << " python3 OCLI.py --backend mlx --model " << model_dir << "\n";
    return "MLX setup successful.";
}

}
