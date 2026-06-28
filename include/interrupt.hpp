#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <termios.h>

namespace ocli {

struct Event {
    std::atomic<bool> v{false};
    bool is_set() const { return v.load(); }
    void set()          { v.store(true); }
    void clear()        { v.store(false); }
};

class InterruptionManager {
public:
    InterruptionManager() = default;
    void start_listening();
    void stop_listening();

    Event interrupted;

private:
    void listen_loop();

    struct termios old_settings_{};
    bool           have_old_settings_ = false;
    bool           active_ = false;
    std::mutex     lock_;
    std::thread    listener_;
};

extern InterruptionManager interrupter;

}
