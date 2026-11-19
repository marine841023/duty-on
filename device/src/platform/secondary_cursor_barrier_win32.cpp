#include "platform/secondary_cursor_barrier_win32.h"

#include <algorithm>
#include <cstdio>
#include <future>
#include <system_error>
#include <utility>

namespace dutyon {
namespace {

thread_local SecondaryCursorBarrier* active_barrier = nullptr;

POINT clampToMonitor(POINT point, const RECT& rect) {
    return {std::clamp(point.x, rect.left, rect.right - 1),
            std::clamp(point.y, rect.top, rect.bottom - 1)};
}

bool bordersCrossedEdge(const RECT& source, const RECT& neighbor, POINT destination) {
    const bool sharedY = source.top < neighbor.bottom && neighbor.top < source.bottom;
    const bool sharedX = source.left < neighbor.right && neighbor.left < source.right;
    return (sharedY && ((destination.x >= source.right && source.right == neighbor.left) ||
                        (destination.x < source.left && source.left == neighbor.right))) ||
           (sharedX && ((destination.y >= source.bottom && source.bottom == neighbor.top) ||
                        (destination.y < source.top && source.top == neighbor.bottom)));
}

long long distanceSquared(POINT a, POINT b) {
    const long long dx = static_cast<long long>(a.x) - b.x;
    const long long dy = static_cast<long long>(a.y) - b.y;
    return dx * dx + dy * dy;
}

struct EdgeCrossing {
    RECT source;
    RECT blocked;
    POINT destination;
    long long blockedDistance;
    bool hasCloserNeighbor = false;
};

BOOL CALLBACK checkNeighbor(HMONITOR, HDC, LPRECT rect, LPARAM data) {
    auto& crossing = *reinterpret_cast<EdgeCrossing*>(data);
    if (!EqualRect(&crossing.blocked, rect) &&
        bordersCrossedEdge(crossing.source, *rect, crossing.destination) &&
        distanceSquared(clampToMonitor(crossing.destination, *rect), crossing.destination) <=
            crossing.blockedDistance) {
        crossing.hasCloserNeighbor = true;
        return FALSE;
    }
    return TRUE;
}

bool canSnapIntoBlockedMonitor(const RECT& source, const RECT& blocked, POINT destination) {
    // Windows can send an off-screen move to a monitor that shares the exit
    // edge, even if the shorter monitor does not reach the requested point.
    if (!bordersCrossedEdge(source, blocked, destination)) return false;
    EdgeCrossing crossing{source, blocked, destination,
                          distanceSquared(clampToMonitor(destination, blocked), destination)};
    EnumDisplayMonitors(nullptr, nullptr, checkNeighbor, reinterpret_cast<LPARAM>(&crossing));
    return !crossing.hasCloserNeighbor;
}

}  // namespace

SecondaryCursorBarrier::~SecondaryCursorBarrier() { stop(); }

void SecondaryCursorBarrier::setBounds(const RECT& bounds) {
    if (IsRectEmpty(&bounds)) {
        stop();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(bounds_mutex_);
        bounds_ = bounds;
    }
    if (thread_.joinable()) return;

    std::promise<bool> ready;
    auto started = ready.get_future();
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        fprintf(stderr, "[SecondaryCursorBarrier] CreateEvent failed: %lu\n", GetLastError());
        return;
    }
    try {
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            active_barrier = this;
            HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
            if (!hook)
                fprintf(stderr, "[SecondaryCursorBarrier] SetWindowsHookEx failed: %lu\n", GetLastError());
            ready.set_value(hook != nullptr);

            if (hook) {
                // Low-level hooks run on the installing thread. Keep this pump
                // separate from rendering and check for shutdown between messages.
                while (MsgWaitForMultipleObjectsEx(1, &stop_event_, INFINITE, QS_ALLINPUT,
                                                   MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1) {
                    MSG msg;
                    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                        DispatchMessageW(&msg);
                }
                UnhookWindowsHookEx(hook);
            }
            active_barrier = nullptr;
        });
    } catch (const std::system_error& error) {
        fprintf(stderr, "[SecondaryCursorBarrier] Could not start hook thread: %s\n", error.what());
        stop();
        return;
    }
    if (!started.get()) stop();
}

void SecondaryCursorBarrier::stop() {
    if (!stop_event_) return;
    SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
}

LRESULT CALLBACK SecondaryCursorBarrier::mouseProc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && message == WM_MOUSEMOVE && active_barrier &&
        active_barrier->blockMove(reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt)) {
        return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool SecondaryCursorBarrier::blockMove(POINT destination) {
    RECT blocked;
    {
        std::lock_guard<std::mutex> lock(bounds_mutex_);
        blocked = bounds_;
    }
    const bool landsOnBlocked = PtInRect(&blocked, destination);
    if (!landsOnBlocked && MonitorFromPoint(destination, MONITOR_DEFAULTTONULL)) return false;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
        return false;

    POINT current;
    if (!GetPhysicalCursorPos(&current) || PtInRect(&blocked, current)) return false;

    // Keep the part of the motion that runs along the edge of the current monitor.
    MONITORINFO source{};
    source.cbSize = sizeof(source);
    const HMONITOR monitor = MonitorFromPoint(current, MONITOR_DEFAULTTONULL);
    if (monitor && GetMonitorInfoW(monitor, &source)) {
        const RECT& rect = source.rcMonitor;
        if (!landsOnBlocked && !canSnapIntoBlockedMonitor(rect, blocked, destination)) return false;
        const POINT edge = clampToMonitor(destination, rect);
        if ((edge.x != current.x || edge.y != current.y) && !PtInRect(&blocked, edge))
            SetPhysicalCursorPos(edge.x, edge.y);
    } else if (!landsOnBlocked) {
        return false;
    }
    return true;
}

}  // namespace dutyon
