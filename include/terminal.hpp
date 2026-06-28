#pragma once

#include <string>
#include <optional>
#include <functional>
#include <deque>
#include <mutex>
#include <csignal>

#include "types.hpp"

namespace ocli {

extern volatile std::sig_atomic_t RESIZE_FLAG;
void _on_resize(int signum);
void install_resize_handler();

extern std::deque<unsigned char> INPUT_PUSHBACK;
extern std::mutex                INPUT_PUSHBACK_MUTEX;

extern const std::vector<std::pair<std::string, std::string>> SLASH_COMMANDS;

bool        can_use_terminal_keys();
bool        input_ready(int fd, double timeout);
void        wait_for_key_or_resize(int fd, const std::function<void()>& on_resize,
                                    double debounce = 0.12);
std::string read_input_byte(int fd);
std::string read_key();
std::optional<MouseEvent> parse_mouse(const std::string& sequence);
std::string read_bracketed_paste(int fd);

}
