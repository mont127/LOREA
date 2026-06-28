#pragma once

#include <cstddef>
#include <string>

namespace ocli {

class PtySession {
 public:
  static PtySession& instance();
  bool start();
  bool alive();
  void write_input(const std::string& bytes);
  std::string read_since(std::size_t& cursor);
  std::string read_available(std::size_t& cursor);
  std::string snapshot();
  void resize(int rows, int cols);
  std::string run_and_capture(const std::string& command, double timeout_s = 60.0);
  void shutdown();

 private:
  PtySession() = default;
  ~PtySession();
  PtySession(const PtySession&) = delete;
  PtySession& operator=(const PtySession&) = delete;
};

PtySession& terminal_session();

extern bool g_shared_terminal_active;

}  // namespace ocli
