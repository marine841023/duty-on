#pragma once

#ifdef _WIN32

#include <cstdint>
#include <memory>

namespace dutyon {

class TurzxDisplay {
public:
    TurzxDisplay();
    ~TurzxDisplay();

    TurzxDisplay(const TurzxDisplay&) = delete;
    TurzxDisplay& operator=(const TurzxDisplay&) = delete;

    bool start();
    void stop();
    bool available() const;

    // The buffer is RGBA, bottom row first (OpenGL readback order). The call
    // only replaces the pending frame; USB I/O happens on a worker thread.
    void submitRgba(const std::uint8_t* pixels, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dutyon

#endif
