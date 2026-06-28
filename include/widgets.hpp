#pragma once

#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <functional>

#include "types.hpp"
#include "ansi.hpp"

namespace ocli {

std::pair<std::vector<std::string>, std::vector<std::pair<std::string, std::string>>>
slash_palette_lines(const std::string& query, int selected,
                    const char* style = Colors::VIOLET, int limit = 8);

std::string styled_input(const std::string& prompt,
                         const std::string& default_value = "",
                         std::function<std::string()> header_fn = nullptr,
                         std::function<std::string()> prompt_fn = nullptr,
                         const std::vector<std::string>* history = nullptr);

std::vector<std::string> menu_lines(const std::string& title,
                                    const std::vector<std::string>& options,
                                    int selected, const char* style = Colors::VIOLET,
                                    int offset = 0, int limit = 0 );

std::optional<int> interactive_menu(const std::string& title,
                                    const std::vector<std::string>& options,
                                    const char* style = Colors::VIOLET);

std::optional<double> interactive_slider(
    const std::string& title, double value, double minimum, double maximum,
    double step = 1, const std::string& unit = "", const char* style = Colors::TEAL,
    const std::string& hint = "",
    std::function<std::string(double)> fmt = nullptr,
    const std::vector<std::pair<double, std::string>>* marks = nullptr);

}
