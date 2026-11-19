#pragma once

#include <windows.h>

#include <mutex>
#include <thread>

namespace dutyon {

class SecondaryCursorBarrier {
public:
    ~SecondaryCursorBarrier();

    void setBounds(const RECT& bounds);
    void stop();

private:
    static LRESULT CALLBACK mouseProc(int code, WPARAM message, LPARAM data);
    bool blockMove(POINT destination);

    std::mutex bounds_mutex_;
    RECT bounds_{};
    HANDLE stop_event_ = nullptr;
    std::thread thread_;
};

}  // namespace dutyon
