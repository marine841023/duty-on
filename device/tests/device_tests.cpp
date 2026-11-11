#include "audio/sound_player.h"
#include "config/user_config.h"
#include "net/wifi_credentials.h"
#include "net/wifi_scan.h"
#include "net/wifi_events.h"
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
#include <sstream>
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
    const std::string chinese = "\xe5\x8d\x8e\xe5\x8f\x91\xe6\x96\x87\xe5\x8c\x96";
    const std::string escaped = R"(\xe5\x8d\x8e\xe5\x8f\x91\xe6\x96\x87\xe5\x8c\x96)";
    check(decodeIwSsid(escaped) == chinese, "iw Chinese UTF8 bytes decoded");
    check(decodeIwSsid(escaped + "_5G") == chinese + "_5G", "iw Chinese suffix preserved");
    check(decodeIwSsid(R"(\x5cxe5)") == R"(\xe5)", "literal backslash decoded only once");
    check(decodeIwSsid(R"(\x20 network\x20)") == "  network ", "SSID edge spaces preserved");
    check(decodeIwSsid(R"(\xQ1\x1\t)") == R"(\xQ1\x1\t)", "unknown escapes not interpreted");
    check(decodeIwSsid(R"(\x3c\x22\x26)") == "<\"&", "HTML metacharacters decoded as data");
    check(displayableWifiSsid(chinese) && displayableWifiSsid(std::string(32, 'a')),
          "UTF8 and 32-byte names supported");
    for (const auto& invalid : {std::string(33, 'a'), std::string("\0", 1), std::string("a\nb"),
             std::string("a\tb"), std::string("\x7f"), std::string("\xc0\xaf"),
             std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
             std::string("\xe5\x8d"), std::string("\xe5\x41\x41")})
        check(!displayableWifiSsid(invalid), "unrepresentable/control SSID excluded");
    std::istringstream scan(escaped + "\n" + chinese + "\n" + R"(\x20edge\x20)" +
                           "\n" + R"(bad\x00name)" + "\n" + R"(\xff)" + "\n" +
                           std::string(33, 'a') + "\n" + "plain\r\n");
    check(parseWifiScanList(scan) == std::vector<std::string>{chinese, " edge ", "plain"},
          "scan list decodes before validation/deduplication without trimming");
}
static void wifiEventTests(const fs::path& dir) {
    const std::string wrong = "<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"network\" auth_failures=1 duration=10 reason=WRONG_KEY";
    check(wifiWrongKeyEvent(wrong), "supplicant wrong key event recognized");
    check(!wifiWrongKeyEvent("<3>CTRL-EVENT-DISCONNECTED reason=15"), "disconnect is not necessarily wrong key");
    check(!wifiWrongKeyEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\" reason=WRONG_KEY\" reason=CONN_FAILED"),
          "SSID content cannot impersonate authentication reason");
    check(!wifiWrongKeyEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED reason=AUTH_FAILED"), "other authentication failures not mislabeled");
    WifiEvents monitor;
    check(!monitor.pollWrongKey((dir / "missing").string()), "missing supplicant is nonblocking");
    const auto path = (dir / "wpa-control").string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int server = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check(server >= 0 && bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "fake supplicant control socket created");
    for (int cycle = 0; cycle < 2; ++cycle) {
        const auto start = Clock::now();
        check(!monitor.pollWrongKey(path), "silent supplicant does not signal failure");
        check(Clock::now() - start < std::chrono::milliseconds(100), "silent supplicant cannot freeze UI");
        sockaddr_un peer{};
        socklen_t len = sizeof(peer);
        char request[64]{};
        const auto n = recvfrom(server, request, sizeof(request), 0, reinterpret_cast<sockaddr*>(&peer), &len);
        check(n == 6 && std::string(request, n) == "ATTACH", "monitor subscribes after open/reset");
        check(recv(server, request, sizeof(request), 0) == 4, "monitor checks supplicant liveness");
        check(sendto(server, wrong.data(), wrong.size(), 0, reinterpret_cast<sockaddr*>(&peer), len) == (ssize_t)wrong.size(),
              "fake authentication failure delivered to abstract client socket");
        check(monitor.pollWrongKey(path), "control event reaches main thread");
        check(recv(server, request, sizeof(request), 0) == 4, "drain health check");
        const std::string connected = "<3>CTRL-EVENT-CONNECTED - Connection to 00:11:22:33:44:55 completed [id=0]";
        check(sendto(server, wrong.data(), wrong.size(), 0, reinterpret_cast<sockaddr*>(&peer), len) > 0 &&
              sendto(server, connected.data(), connected.size(), 0, reinterpret_cast<sockaddr*>(&peer), len) > 0,
              "fake reconnection queued after earlier failure");
        check(!monitor.pollWrongKey(path), "successful reconnection supersedes queued authentication error");
        check(recv(server, request, sizeof(request), 0) == 4, "drain reconnection health check");
        monitor.reset();
    }
    close(server);
    check(!monitor.pollWrongKey(path), "terminated supplicant does not block reconnect");
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
    check(chmod(file.c_str(), 0644) == 0, "legacy permissions fixture");
    check(restrictPrivateFile(file) && read(file) == "old", "legacy migration preserves contents");
    check(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "legacy permissions restricted");
    const auto link = dir / "private-link";
    fs::create_symlink(file, link);
    check(chmod(file.c_str(), 0644) == 0 && !restrictPrivateFile(link), "symlink migration rejected");
    check(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644, "symlink target unchanged");
    fs::remove(link);
    check(restrictPrivateFile(file), "restore private permissions");
    check(!restrictPrivateFile(dir) && !restrictPrivateFile((dir / "missing").string()),
          "non-file migration rejected");
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
        wifiTests(); wifiEventTests(dir); mediaTests(); storageTests(dir); childTests(); audioTests(dir);
        fs::remove_all(dir);
        std::cout << "PASS: WiFi credentials/scan/events, media budgets, atomic storage, config, child/audio shutdown\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << " (fixtures: " << dir << ")\n";
        return 1;
    }
}
