#include "interrupt.hpp"

#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

namespace ocli {

namespace {

bool tc_getattr(int fd, struct termios& out) {
    return ::tcgetattr(fd, &out) == 0;
}

bool set_cbreak(int fd) {
    struct termios mode{};
    if (::tcgetattr(fd, &mode) != 0) return false;
    mode.c_lflag &= ~(static_cast<tcflag_t>(ECHO | ICANON));
    mode.c_cc[VMIN]  = 1;
    mode.c_cc[VTIME] = 0;
    return ::tcsetattr(fd, TCSAFLUSH, &mode) == 0;
}

bool tc_setattr_drain(int fd, const struct termios& settings) {
    return ::tcsetattr(fd, TCSADRAIN, &settings) == 0;
}

}

void InterruptionManager::start_listening() {
    std::lock_guard<std::mutex> guard(lock_);
    if (active_) return;
    interrupted.clear();

    if (!tc_getattr(STDIN_FILENO, old_settings_)) return;
    have_old_settings_ = true;
    if (!set_cbreak(STDIN_FILENO)) return;
    active_ = true;

    listener_ = std::thread(&InterruptionManager::listen_loop, this);
    listener_.detach();
}

void InterruptionManager::stop_listening() {
    std::lock_guard<std::mutex> guard(lock_);
    if (!active_) return;
    active_ = false;

    if (have_old_settings_) {
        tc_setattr_drain(STDIN_FILENO, old_settings_);
    }
}

void InterruptionManager::listen_loop() {
    while (true) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (!active_) break;
        }

        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(STDIN_FILENO, &rset);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 20000;
        int rc = ::select(STDIN_FILENO + 1, &rset, nullptr, nullptr, &tv);
        if (rc < 0) break;
        if (rc > 0 && FD_ISSET(STDIN_FILENO, &rset)) {
            unsigned char key = 0;
            ssize_t n = ::read(STDIN_FILENO, &key, 1);

            if (n <= 0) break;
            if (key == 27) {
                interrupted.set();
                break;
            }
        }
    }
}

InterruptionManager interrupter;

}
