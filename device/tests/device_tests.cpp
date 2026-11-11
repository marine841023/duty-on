#include "audio/sound_player.h"
#include "config/user_config.h"
#include "net/wifi_credentials.h"
#include "platform/atomic_file.h"
#include "platform/child_process.h"
#include "render/media_limits.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace dutyon;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void wifiTests() {
    check(validWifiCredentials("network", ""), "open network");
    check(validWifiCredentials(std::string(32, 's'), std::string(63, 'p')), "maximum credentials");
    check(!validWifiCredentials(std::string(33, 's'), "password"), "SSID byte limit");
    check(!validWifiCredentials("", "password"), "empty SSID");
    check(!validWifiCredentials("x\ny", "password"), "SSID newline injection");
    check(!validWifiCredentials(std::string("x\0y", 3), "password"), "SSID NUL injection");
    check(!validWifiCredentials("network", "pass\rword"), "PSK newline injection");
    check(!validWifiCredentials("network", "short"), "short PSK");
    check(validWifiCredentials("network", std::string(64, 'a')), "raw hex PSK");
    check(!validWifiCredentials("network", std::string(64, 'g')), "nonhex raw PSK");
    check(wifiSsidHex(std::string("\xc3\xa9", 2)) == "c3a9", "UTF8 SSID hex encoding");
    check(validWifiCredentials(" trailing ", "pass word"), "spaces are significant");
}
static std::vector<unsigned char> gif(unsigned w, unsigned h, unsigned frames) {
    std::vector<unsigned char> b{'G','I','F','8','9','a',
        (unsigned char)w, (unsigned char)(w >> 8),
        (unsigned char)h, (unsigned char)(h >> 8), 0x80, 0, 0,
        0,0,0, 255,255,255};
    const unsigned char frame[]{0x2c,0,0,0,0,1,0,1,0,0,2,2,0x44,1,0};
    for (unsigned i = 0; i < frames; ++i) b.insert(b.end(), std::begin(frame), std::end(frame));
    b.push_back(0x3b);
    return b;
}
static void mediaTests() {
    auto b = gif(1, 1, 1);
    check(validGifBudget(b.data(), b.size()), "valid small GIF");
    for (size_t n = 0; n < b.size(); ++n)
        check(!validGifBudget(b.data(), n), "truncated GIF rejected");
    for (auto bad : {gif(5000, 1, 1), gif(1, 1, 1001), gif(4096, 2048, 5), gif(0, 1, 1)})
        check(!validGifBudget(bad.data(), bad.size()), "GIF allocation budget");
    b[20] = 1; // frame x=1 falls outside 1-pixel canvas
    check(!validGifBudget(b.data(), b.size()), "frame outside canvas");
    check(validImageSize(4096, 2048), "image pixel boundary");
    check(!validImageSize(4096, 4096) && !validImageSize(-1, 2), "image size bounds");
}
static void storageTests(const fs::path& dir) {
    const auto file = (dir / "private.json").string();
    check(atomicWritePrivate(file, "old"), "initial atomic write");
    struct stat st{};
    check(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "private permissions");
    struct rlimit original{};
    check(getrlimit(RLIMIT_FSIZE, &original) == 0, "get file limit");
    auto handler = signal(SIGXFSZ, SIG_IGN);
    struct rlimit limited = original;
    limited.rlim_cur = 3;
    check(setrlimit(RLIMIT_FSIZE, &limited) == 0, "set short-write fixture");
    const bool saved = atomicWritePrivate(file, "replacement");
    check(setrlimit(RLIMIT_FSIZE, &original) == 0, "restore file limit");
    signal(SIGXFSZ, handler);
    check(!saved && read(file) == "old", "failed partial write preserves old file");
    check(atomicWritePrivate(file, "new"), "replace atomic file");
    check(read(file) == "new", "replacement content");
    for (const auto& e : fs::directory_iterator(dir))
        check(e.path().filename().string().find(".tmp.") == std::string::npos, "temporary cleanup");

    setenv("USERPROFILE", dir.c_str(), 1); // isolate configuration from real user files
    const auto cfgPath = dir / ".dutyon/config.json";
    check(atomicWritePrivate(cfgPath, R"({"deviceBrightness":18446744073709551615,
        "deviceVolume":1.5,"screenRotation":45,"windowPosition":{"x":1e30,"y":3},
        "stateMotions":{"m":{"bad":["Idle",-1],"good":["Idle",2]}},
        "customCharacters":[{"id":42,"name":"bad"}]})"), "bad config fixture");
    auto cfg = UserConfigStore::load();
    check(cfg.device_brightness == 100 && cfg.device_volume == 80 && cfg.screen_rotation == 0,
          "invalid numeric settings keep defaults");
    check(!cfg.has_window_pos && cfg.custom_characters.empty(), "invalid field types rejected");
    check(cfg.state_motions.at("m").size() == 1 && cfg.state_motions.at("m").at("good").second == 2,
          "motion indices validated independently");
    check(atomicWritePrivate(cfgPath, "{broken"), "truncated config fixture");
    check(UserConfigStore::load().device_volume == 80, "bad JSON does not crash");
}
static void childTests() {
    int ready[2];
    check(pipe(ready) == 0, "child handshake pipe");
    pid_t pid = fork();
    check(pid >= 0, "fork fixture");
    if (!pid) {
        close(ready[0]);
        signal(SIGTERM, SIG_IGN);
        (void)!write(ready[1], "r", 1);
        for (;;) pause();
    }
    close(ready[1]);
    char c;
    check(::read(ready[0], &c, 1) == 1, "TERM-ignore child ready");
    close(ready[0]);
    const auto start = Clock::now();
    check(terminateChild(pid) && pid == -1, "KILL fallback reaps child");
    check(Clock::now() - start < std::chrono::seconds(2), "child exit bounded");
}

// The test executable doubles as fake aplay/ffmpeg. No real audio device is used.
static int fakeCommand(const std::string& name) {
    signal(SIGTERM, SIG_IGN);
    const fs::path dir = getenv("DUTYON_TEST_DIR");
    std::ofstream(dir / (name + ".ready")).put('r');
    const std::string mode = getenv("DUTYON_TEST_AUDIO_MODE");
    if ((name == "aplay" && mode == "blocked") || (name == "ffmpeg" && mode != "flood"))
        for (;;) pause();
    char bytes[8192]{};
    if (name == "ffmpeg") while (::write(1, bytes, sizeof(bytes)) > 0) {}
    else while (::read(0, bytes, sizeof(bytes)) > 0) usleep(40000);
    return 0;
}
static void waitReady(const fs::path& path) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!fs::exists(path) && Clock::now() < deadline) usleep(10000);
    check(fs::exists(path), "fake audio subprocess started");
}
static void audioTests(const fs::path& dir) {
    const auto bin = dir / "bin";
    fs::create_directory(bin);
    const auto self = fs::read_symlink("/proc/self/exe");
    fs::create_symlink(self, bin / "aplay");
    fs::create_symlink(self, bin / "ffmpeg");
    const std::string path = bin.string() + ":" + getenv("PATH");
    setenv("PATH", path.c_str(), 1);
    setenv("DUTYON_TEST_DIR", dir.c_str(), 1);
    for (const char* mode : {"blocked", "silent", "flood"}) {
        setenv("DUTYON_TEST_AUDIO_MODE", mode, 1);
        fs::remove(dir / "aplay.ready");
        fs::remove(dir / "ffmpeg.ready");
        auto player = std::make_unique<SoundPlayer>();
        player->playFile((dir / "missing.mp3").string());
        waitReady(dir / "aplay.ready");
        waitReady(dir / "ffmpeg.ready");
        usleep(400000); // fill blocked pipe / give decoder data time to arrive
        const auto start = Clock::now();
        player.reset();
        check(Clock::now() - start < std::chrono::seconds(2), "audio shutdown bounded");
    }
    int status = 0;
    check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "all audio children reaped");
}
int main(int, char** argv) {
    const auto name = fs::path(argv[0]).filename().string();
    if (name == "aplay" || name == "ffmpeg") return fakeCommand(name);
    char tmp[] = "/tmp/dutyon-tests-XXXXXX";
    const char* made = mkdtemp(tmp);
    if (!made) return 1;
    const fs::path dir = made;
    try {
        wifiTests(); mediaTests(); storageTests(dir); childTests(); audioTests(dir);
        fs::remove_all(dir);
        std::cout << "PASS: WiFi credentials, media budgets, atomic storage, config, child/audio shutdown\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << " (fixtures: " << dir << ")\n";
        return 1;
    }
}
