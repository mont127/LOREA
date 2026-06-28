#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ocli {

bool live_begin(std::function<std::string()> status_fn);

std::string live_read_line(const std::vector<std::string>* history);

void live_end();

bool live_active();

void live_set_status_line(const std::string& line);

std::string live_take_terminal_context();

bool live_suspend();

void live_resume();

struct LiveSuspendGuard {
  bool suspended;
  LiveSuspendGuard() : suspended(live_suspend()) {}
  ~LiveSuspendGuard() {
    if (suspended) live_resume();
  }
  LiveSuspendGuard(const LiveSuspendGuard&) = delete;
  LiveSuspendGuard& operator=(const LiveSuspendGuard&) = delete;
};

}
