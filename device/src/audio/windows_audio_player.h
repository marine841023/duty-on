#pragma once

#include <memory>
#include <string>
#include <vector>

namespace dutyon {

// Windows alert and preview playback, confined to the main thread.
class WindowsAudioPlayer {
public:
    struct Output {
        std::string id;
        std::string name;
    };

    WindowsAudioPlayer();
    ~WindowsAudioPlayer();

    std::vector<Output> outputs() const;
    void setVolume(int percent);
    void play(const std::string& path, const std::string& endpoint_id);
    void stop();
    void tick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dutyon
