#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <atomic>
#include <thread>
#include <chrono>
#include <regex>

#include "types.hpp"
#include "ansi.hpp"

namespace ocli {

extern const std::regex CODE_FENCE_RE;
std::string render_inline(const std::string& text);
std::string render_text(const std::string& text);

void fake_loading(const std::string& msg, double duration = 0.6);
void log_tool(const std::string& msg);
void log_info(const std::string& msg);
void log_ok(const std::string& msg);
void log_warn(const std::string& msg);

extern const std::map<std::string, std::string> TOOL_ICONS;
std::string summarize_tool_args(const json& args);
void        print_tool_call(const std::string& name, const json& args);

extern const std::regex TOOL_MARKUP_RE;
std::string strip_tool_markup(const std::string& text);
std::string visible_answer(const std::string& text);

void print_diff(const std::vector<std::string>& diff_lines);

std::string gold_gradient(const std::string& text);
void        glow_text(const std::string& text, int cycles = 3, double delay = 0.05);

extern const std::vector<std::string> LOGO_PHRASES;
std::optional<std::string> pick_logo_phrase();

std::string              center_pad_logo(int cols, int text_len);
int                      term_rows(int def = 24);
std::vector<std::string> lorea_full_lines(int cols);
int                      flower_width();
int                      lockup_width();
int                      flower_color_code(int row, int col);
std::string              color_flower_line(const std::string& fline, int row);
std::vector<std::string> logo_lockup_lines(int cols);
std::vector<std::string> logo_lines(const std::optional<std::string>& phrase = std::nullopt);
void                     print_logo(const std::optional<std::string>& phrase = std::nullopt);

class Spinner {
public:
    explicit Spinner(std::string msg = "AI is thinking");
    ~Spinner();
    void start();
    void stop();
private:
    void spin();
    std::string                            msg_;
    std::atomic<bool>                      running_{false};
    std::thread                            thread_;
    std::chrono::steady_clock::time_point  started_at_{};
    std::string                            current_pulse_;
    int                                    last_len_ = 0;
};

}
