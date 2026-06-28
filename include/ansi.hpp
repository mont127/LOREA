#pragma once

#include <string>
#include <vector>
#include <cstddef>

#include "types.hpp"

namespace ocli {

inline constexpr int LEFT_MARGIN = 2;

struct Colors {
    static constexpr const char* BLACK     = "\033[30m";
    static constexpr const char* BLUE      = "\033[94m";
    static constexpr const char* GREEN     = "\033[92m";
    static constexpr const char* YELLOW    = "\033[93m";
    static constexpr const char* GRAY      = "\033[90m";
    static constexpr const char* CYAN      = "\033[96m";
    static constexpr const char* RED       = "\033[91m";
    static constexpr const char* MAGENTA   = "\033[95m";
    static constexpr const char* WHITE     = "\033[97m";
    static constexpr const char* TEAL      = "\033[38;5;46m";
    static constexpr const char* VIOLET    = "\033[38;5;141m";
    static constexpr const char* PINK      = "\033[38;5;213m";
    static constexpr const char* ORANGE    = "\033[38;5;208m";
    static constexpr const char* LIME      = "\033[38;5;118m";
    static constexpr const char* SKY       = "\033[38;5;117m";
    static constexpr const char* EMERALD   = "\033[38;5;78m";
    static constexpr const char* AMBER     = "\033[38;5;220m";
    static constexpr const char* ROSE      = "\033[38;5;204m";
    static constexpr const char* SLATE     = "\033[38;5;245m";
    static constexpr const char* INDIGO    = "\033[38;5;105m";
    static constexpr const char* MINT      = "\033[38;5;121m";
    static constexpr const char* BG_DARK   = "\033[48;5;236m";
    static constexpr const char* BG_PANEL  = "\033[48;5;234m";
    static constexpr const char* BG_BADGE  = "\033[48;5;238m";
    static constexpr const char* BG_ACCENT = "\033[48;5;54m";
    static constexpr const char* BG_ADD    = "\033[48;5;22m";
    static constexpr const char* BG_DEL    = "\033[48;5;52m";
    static constexpr const char* DIFF_ADD  = "\033[38;5;120m";
    static constexpr const char* DIFF_DEL  = "\033[38;5;210m";
    static constexpr const char* CODE      = "\033[38;5;252m";
    static constexpr const char* BOLD      = "\033[1m";
    static constexpr const char* DIM       = "\033[2m";
    static constexpr const char* ITALIC    = "\033[3m";
    static constexpr const char* UNDERLINE = "\033[4m";
    static constexpr const char* RESET     = "\033[0m";
};

extern const char* ACCENT;
extern const char* MUTED;

extern const std::vector<int>          FLAIR_RAMP;
extern const std::vector<std::string>  CELEBRATION_ICONS;

std::size_t              utf8_len(const std::string& s);

std::vector<std::string> utf8_chars(const std::string& s);

std::u32string           utf8_to_u32(const std::string& s);

std::string              u32_to_utf8(const std::u32string& s);

std::string              utf8_substr(const std::string& s, std::size_t start,
                                     std::size_t count = std::string::npos);

std::string clean_ansi(const std::string& text);
std::size_t clean_len(const std::string& text);

int         term_cols(int def = 100);
int         term_width(int def = 92);
std::string left_indent();
std::string center_pad(int content_len);

std::string badge(const std::string& text, const char* fg = Colors::WHITE,
                  const char* bg = Colors::BG_BADGE);
std::string frame_title(const std::string& title, const char* style = nullptr,
                        const char* subtitle = nullptr);
std::string frame_bottom(const char* style = nullptr, const char* hint = nullptr);
std::string status_label(const std::string& text, const char* style = nullptr);
std::string gradient_text(const std::string& text, const std::vector<int>* ramp = nullptr,
                          bool bold = true);
std::string progress_bar(double done, double total, int slots = 22, bool animate = false);
std::string sparkle_line(int width = -1);
void        celebrate(const std::string& title, const char* subtitle = nullptr,
                      const char* style = nullptr);
std::string mode_value(bool enabled, const std::string& on = "on", const std::string& off = "off");
std::string soft_rule(const char* style = nullptr);
std::string kv_row(const std::string& label, const std::string& value, int label_width = 11);
std::string truncate_visible(const std::string& text, int limit);

std::vector<std::string> panel_lines(const std::string& title,
                                     const std::vector<std::string>& lines,
                                     const char* style = Colors::CYAN);
void print_panel(const std::string& title, const std::vector<std::string>& lines,
                 const char* style = Colors::CYAN);
void print_frame_line(const std::string& text = "", const char* style = Colors::MAGENTA);
void print_frame_text(const std::string& text, const char* style = Colors::MAGENTA);

bool        can_use_terminal_keys();
std::string raw_text(const std::string& text);

}
