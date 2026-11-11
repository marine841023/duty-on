#pragma once
#ifndef _WIN32

#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>

namespace dutyon {

// Keep an unreaped PID until waitpid confirms exit. In particular, SIGKILL
// does not wake a task stuck in uninterruptible driver I/O; never wait forever
// or spawn unlimited replacements for it.
inline bool reapChild(pid_t& pid) {
    if (pid <= 0) return true;
    const pid_t result = ::waitpid(pid, nullptr, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
        pid = -1;
        return true;
    }
    return false;
}

inline bool terminateChild(pid_t& pid, int grace_ms = 200) {
    if (reapChild(pid)) return true;
    ::kill(pid, SIGTERM);
    for (int ms = 0; ms < grace_ms; ms += 10) {
        if (reapChild(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(pid, SIGKILL);
    for (int ms = 0; ms < 300; ms += 10) {
        if (reapChild(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return reapChild(pid);
}

} // namespace dutyon
#endif
