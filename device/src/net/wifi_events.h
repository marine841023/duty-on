#pragma once
#ifndef _WIN32

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace dutyon {

// Match the event and its final reason field, never text inside a quoted SSID.
inline std::string wifiEventBody(std::string event) {
    if (!event.empty() && event.front() == '<') {
        const auto end = event.find('>');
        if (end == std::string::npos) return "";
        event.erase(0, end + 1);
    }
    return event;
}

inline bool wifiWrongKeyEvent(const std::string& message) {
    const auto event = wifiEventBody(message);
    const std::string prefix = "CTRL-EVENT-SSID-TEMP-DISABLED ";
    const std::string reason = " reason=WRONG_KEY";
    return event.compare(0, prefix.size(), prefix) == 0 &&
           event.size() >= prefix.size() + reason.size() &&
           event.compare(event.size() - reason.size(), reason.size(), reason) == 0;
}

// Linux control socket subscription: no subprocesses, keys, or growing log files.
// All I/O is nonblocking so a missing/stalled supplicant cannot stop rendering.
class WifiEvents {
public:
    WifiEvents() = default;
    WifiEvents(const WifiEvents&) = delete;
    WifiEvents& operator=(const WifiEvents&) = delete;
    ~WifiEvents() { reset(); }

    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool pollWrongKey(const std::string& control_path) {
        if (fd_ < 0) {
            sockaddr_un remote{};
            remote.sun_family = AF_UNIX;
            if (control_path.size() >= sizeof(remote.sun_path)) return false;
            std::memcpy(remote.sun_path, control_path.c_str(), control_path.size() + 1);
            fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd_ < 0) return false;
            // Linux autobind allocates a unique abstract address, removed on close.
            sockaddr_un local{};
            local.sun_family = AF_UNIX;
            if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local.sun_family)) < 0 ||
                ::connect(fd_, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0 ||
                ::send(fd_, "ATTACH", 6, MSG_DONTWAIT | MSG_NOSIGNAL) != 6) {
                reset();
                return false;
            }
        }
        // Detect a replaced control socket and attach again on the next poll.
        if (::send(fd_, "PING", 4, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 &&
            errno != EAGAIN && errno != EWOULDBLOCK) {
            reset();
            return false;
        }
        bool wrong_key = false;
        for (int n = 0; n < 32; ++n) {
            char buf[4096];
            const auto size = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (size < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) reset();
                break;
            }
            const std::string event(buf, size);
            wrong_key = wrong_key || wifiWrongKeyEvent(event);
            const auto body = wifiEventBody(event);
            if (body.rfind("CTRL-EVENT-CONNECTED -", 0) == 0) wrong_key = false;
            if (event == "FAIL\n" || body.rfind("CTRL-EVENT-TERMINATING ", 0) == 0) {
                reset();
                break;
            }
        }
        return wrong_key;
    }

private:
    int fd_ = -1;
};

} // namespace dutyon
#endif
