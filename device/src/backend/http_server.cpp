// 内嵌 HTTP 服务器实现 —— server.rs 1:1 移植（cpp-httplib，仅 Windows）。

#ifdef _WIN32

#include "backend/http_server.h"

// Winsock 头顺序：winsock2/ws2tcpip 必须先于 windows.h（且后者带
// WIN32_LEAN_AND_MEAN），否则旧版 winsock.h 与 httplib.h 内部的
// winsock2.h 冲突，ws2tcpip.h 的多播结构体（IP_MSFILTER 等）解析错乱。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <httplib.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include "backend/autostart.h"
#include "backend/backend_config.h"
#include "backend/hooks_installer.h"
#include "backend/ide_scanner.h"
#include "backend/pairing_manager.h"
#include "config/user_config.h"  // UserConfig::builtinDefaultColor（内置角色出厂色）

// 照片缩放重编码（解码/编码实现均在 live2d_renderer.cpp，本 TU 仅引用声明）
#include <stb_image.h>
#include <stb_image_write.h>

namespace dutyon::backend {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string homeDir() {
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    return home ? std::string(home) : std::string();
}

// /api/hooks/install 的 hooks 资源目录解析在 hooks_installer.cpp
// （resolveHooksSourceDir，本文件与 BackendService 共用）。

// ---- 文件服务 ----
std::optional<std::string> readFileIfExists(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* mimeForExtension(const std::string& ext) {
    if (ext == "json") return "application/json";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "ogg") return "audio/ogg";
    return "application/octet-stream";
}

// 追加一行到 ~/.dutyon/frontend.log（超 512KB 截断；release 无控制台，
// 诊断日志要落盘才能在用户机器上排查）
void appendLogFile(const std::string& level, const std::string& msg) {
    const std::string home = homeDir();
    if (home.empty()) return;
    const fs::path path = fs::path(home) / ".dutyon" / "frontend.log";
    std::error_code ec;
    if (fs::exists(path) && fs::file_size(path, ec) > 512 * 1024) {
        fs::remove(path, ec);
    }
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    out << "[" << secs << "][" << level << "] " << msg << "\n";
}

// 回环判定（写端点 loopback_guard）：仅放行 127.0.0.1 / ::1
bool isLoopback(const std::string& addr) {
    return addr == "127.0.0.1" || addr == "::1" || addr == "localhost";
}

// 200 + JSON 响应（匿名 namespace 自由函数：路由 lambda 不必逐个捕获）
void okJson(httplib::Response& res, const json& j) {
    res.status = 200;
    res.set_content(j.dump(), "application/json");
}

// 读取 ~/.dutyon/config.json（解析失败/文件缺失返回 null）。
// /api/status 每次轮询都要读：小文件 + 容错解析，开销可忽略。
json readConfigJson() {
    const std::string home = homeDir();
    if (home.empty()) return json{};
    auto content = readFileIfExists(fs::path(home) / ".dutyon" / "config.json");
    if (!content.has_value()) return json{};
    return json::parse(*content, nullptr, /*allow_exceptions=*/false);
}

// ---------------------------------------------------------------------------
// 相框「指定文件夹」照片服务（GET /api/frame/photo）
//
// 设备端不批量同步照片：每播放完一张才要下一张，本端每次只回一个文件，
// 照片始终留在 PC 磁盘（设备侧零落盘、不累积）。文件列表按目录 5s 缓存，
// 目录增删照片最迟 5s 生效；选取用蓄水池随机（每请求独立抽签）：一轮内
// 几乎不重复，无需在 PC 端维护播放进度表。
// 大尺寸手机照片按 max_side 在 PC 端缩放重编码后下发（设备内存/带宽友好），
// 重编码结果按文件路径+mtime+尺寸键缓存于内存（不写盘）。
// ---------------------------------------------------------------------------
constexpr auto kFrameDirTtl = std::chrono::seconds(5);
constexpr int kFrameMaxSide = 1536;   // 下发照片最长边上限（设备屏 800x480）
constexpr size_t kFrameCacheBudget = 12 * 1024 * 1024;  // 重编码缓存预算

struct FrameDirCache {
    std::mutex mtx;
    std::string dir_key;                      // 当前缓存的目录
    std::chrono::steady_clock::time_point scanned{};
    std::vector<fs::path> files;
};
FrameDirCache g_frame_dir;

struct FramePhotoCacheItem {
    std::string key;   // 绝对路径|mtime秒|文件大小
    std::string jpeg;  // 重编码字节
};
std::vector<FramePhotoCacheItem> g_frame_photos;  // 前端单请求顺序访问，不加锁

bool isFramePhotoExt(const fs::path& p) {
    std::string ext = p.extension().string();
    for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

// 列目录（非递归，仅 jpg/jpeg/png）；TTL 内命中缓存。空 vector = 目录无效或无图
std::vector<fs::path> framePhotoList(const fs::path& dir) {
    std::lock_guard<std::mutex> lk(g_frame_dir.mtx);
    const auto now = std::chrono::steady_clock::now();
    const std::string key = dir.string();
    if (g_frame_dir.dir_key != key || now - g_frame_dir.scanned >= kFrameDirTtl) {
        std::vector<fs::path> files;
        std::error_code ec;
        if (fs::is_directory(dir, ec)) {
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.is_regular_file(ec) && isFramePhotoExt(e.path()))
                    files.push_back(e.path());
            }
        }
        std::sort(files.begin(), files.end());  // 缓存顺序稳定，便于键比对
        g_frame_dir.dir_key = key;
        g_frame_dir.scanned = now;
        g_frame_dir.files = std::move(files);
    }
    return g_frame_dir.files;
}

// 蓄水池随机：n 个里等概率取 1 个（无需保存“已播过”集合）
size_t framePickReservoir(size_t n) {
    static std::mt19937_64 rng{
        (uint64_t)std::random_device{}() * 0x9E3779B97F4A7C15ull ^
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()};
    std::uniform_real_distribution<double> u(0.0, 1.0);
    size_t pick = 0;
    for (size_t i = 1; i < n; ++i)
        if (u(rng) < 1.0 / (double)(i + 1)) pick = i;
    return pick;
}

// 缩到最长边 ≤ max_side 并重编码 JPEG（质量 88）；失败返回空。
// 缓存命中直接复用（避免每张新照片都占一次 CPU 缩放）。
std::string framePhotoScaled(const fs::path& file, int max_side) {
    std::error_code ec;
    const auto mt = fs::last_write_time(file, ec);
    const auto sz = fs::file_size(file, ec);
    std::string key = file.string() + "|" +
                      std::to_string(mt.time_since_epoch().count()) + "|" +
                      std::to_string((unsigned long long)sz);
    for (size_t i = 0; i < g_frame_photos.size(); ++i) {
        if (g_frame_photos[i].key != key) continue;
        if (i != 0) {
            FramePhotoCacheItem it = std::move(g_frame_photos[i]);
            g_frame_photos.erase(g_frame_photos.begin());
            g_frame_photos.insert(g_frame_photos.begin(), std::move(it));
        }
        return g_frame_photos.front().jpeg;
    }
    auto raw = readFileIfExists(file);
    if (!raw.has_value() || raw->empty()) return {};
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(
        (const unsigned char*)raw->data(), (int)raw->size(), &w, &h, &ch, 3);
    if (!px) {
        stbi_image_free(px);
        return {};  // 解不开（伪装扩展名/损坏）：当作无图
    }
    std::string out;
    const int longest = w > h ? w : h;
    if (longest > max_side && longest > 0) {
        const int nw = (int)((double)w * max_side / longest + 0.5);
        const int nh = (int)((double)h * max_side / longest + 0.5);
        const int sw = nw < 1 ? 1 : nw, sh = nh < 1 ? 1 : nh;
        // 最近邻缩放（照片仅用于 800x480 屏预览，不追求插值质量；
        // 避免引入 stb_image_resize 头文件）
        std::vector<unsigned char> dst((size_t)sw * sh * 3);
        for (int y = 0; y < sh; ++y) {
            int sy = (int)((double)y * h / sh);
            if (sy >= h) sy = h - 1;
            for (int x = 0; x < sw; ++x) {
                int sx = (int)((double)x * w / sw);
                if (sx >= w) sx = w - 1;
                const unsigned char* s = px + ((size_t)sy * w + sx) * 3;
                unsigned char* d = &dst[((size_t)y * sw + x) * 3];
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
        std::vector<unsigned char> buf;
        stbi_write_jpg_to_func(
            [](void* ctx, void* data, int size) {
                auto* v = static_cast<std::vector<unsigned char>*>(ctx);
                v->insert(v->end(), (const unsigned char*)data,
                          (const unsigned char*)data + size);
            },
            &buf, sw, sh, 3, dst.data(), 88);
        out.assign((const char*)buf.data(), buf.size());
    }
    stbi_image_free(px);
    if (!out.empty()) {
        // LRU 写入：同键已存在则先移除；超预算从尾逐出（只留最近几张）
        for (auto it = g_frame_photos.begin(); it != g_frame_photos.end();) {
            if (it->key == key) it = g_frame_photos.erase(it);
            else ++it;
        }
        g_frame_photos.insert(g_frame_photos.begin(),
                              FramePhotoCacheItem{key, out});
        size_t total = 0;
        for (auto it = g_frame_photos.begin(); it != g_frame_photos.end();) {
            total += it->jpeg.size();
            if (total > kFrameCacheBudget && g_frame_photos.size() > 1)
                it = g_frame_photos.erase(it);
            else
                ++it;
        }
    }
    return out;
}

} // namespace

HttpServer::HttpServer(StateManager& sm, SysMonitor& monitor, PairingManager& pairing)
    : sm_(sm), monitor_(monitor), pairing_(pairing) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    if (running_) return true;
    svr_ = new httplib::Server();

    // SSE 长连接占线程：限制池大小防无限膨胀；keep-alive 短超时防半关闭
    // 连接积压（Rust 版 SO_KEEPALIVE 解决的同一问题，这里用应用层超时）
    svr_->new_task_queue = [] { return new httplib::ThreadPool(8); };
    svr_->set_keep_alive_max_count(4);
    svr_->set_read_timeout(15, 0);
    svr_->set_write_timeout(30, 0);

    registerRoutes();

    // 始终绑 0.0.0.0：设备经局域网 Wi-Fi 访问（不再有 USB 网段直连假设）。
    // 安全由 token 门控保证 —— /api/* 除配对端点外，未配对/无效 token 一律
    // 401（见 set_pre_routing_handler），写端点仍受 loopback_guard 保护。
    const char* bind_host = "0.0.0.0";
    if (!svr_->bind_to_port(bind_host, (int)bc::kPort)) {
        // AddrInUse = 已有实例在跑（老版本双进程并存期也会出现）
        fprintf(stderr, "[HttpServer] Port %u is already in use. Another instance may be "
                        "running.\n",
                (unsigned)bc::kPort);
        delete svr_;
        svr_ = nullptr;
        return false;
    }

    running_ = true;
    std::thread([this] {
        if (!svr_->listen_after_bind()) {
            fprintf(stderr, "[HttpServer] serve error\n");
        }
        running_ = false;
    }).detach();
    printf("[HttpServer] Listening on http://%s:%u (token-gated)\n", bind_host,
           (unsigned)bc::kPort);

    // 设备发现应答线程（UDP 17522）：替代旧 USB 网段的 ARP 通告。
    // joinable 成员线程（非 detach）：stop() 置否标志后 join，避免退出时
    // 线程仍访问已析构的 pairing_（recv 超时 500ms，join 最多等这么久）。
    discovery_run_ = true;
    discovery_thread_ = std::thread([this] { runDiscovery(); });
    return true;
}

void HttpServer::stop() {
    discovery_run_ = false;  // 发现线程收包超时 500ms 内醒来并退出
    if (discovery_thread_.joinable()) discovery_thread_.join();
    if (svr_) {
        svr_->stop();
        delete svr_;
        svr_ = nullptr;
    }
    running_ = false;
}

void HttpServer::runDiscovery() {
    // UDP 17522 发现应答：设备入网后广播 DUTYON_DISCOVER <device_id>，这里
    // 单播回 DUTYON_OFFER <api_port> <paired>，设备据回包源 IP + 端口组
    // base_url 轮询 /api/*（替代旧 USB 网段的 ARP 通告，见 net/pc_discovery）。
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    // 收包超时 500ms：周期性检查 discovery_run_ 以便 stop() 及时退出
    DWORD rcv_to = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_to, sizeof(rcv_to));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bc::kDiscoveryPort);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        fprintf(stderr, "[Discovery] bind :%u failed (%d)\n",
                (unsigned)bc::kDiscoveryPort, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return;
    }
    printf("[Discovery] listening on UDP :%u\n", (unsigned)bc::kDiscoveryPort);
    while (discovery_run_.load()) {
        char buf[256] = {};
        sockaddr_in from{};
        int fromlen = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n <= 0) continue;  // 超时/错误 -> 回头检查运行标志
        buf[n] = '\0';
        char prefix[32] = {};
        char device_id[128] = {};
        if (sscanf(buf, "%31s %127s", prefix, device_id) != 2) continue;
        if (strcmp(prefix, "DUTYON_DISCOVER") != 0) continue;
        const int paired = pairing_.isPaired(device_id) ? 1 : 0;
        char offer[64];
        const int len = snprintf(offer, sizeof(offer), "DUTYON_OFFER %u %d",
                                 (unsigned)bc::kPort, paired);
        if (len > 0)
            (void)sendto(sock, offer, len, 0,
                         reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
    closesocket(sock);
    WSACleanup();
}

void HttpServer::registerRoutes() {
    // CORS：任意来源（硬件显示端浏览器要跨源 fetch + EventSource）
    auto add_cors = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "*");
    };
    auto loopback_guard = [&](const httplib::Request& req, httplib::Response& res) -> bool {
        if (!isLoopback(req.remote_addr)) {
            res.status = 403;
            res.set_content("write endpoints are loopback-only; use /api/* for remote access",
                            "text/plain");
            return false;
        }
        return true;
    };

    // CORS 预检（全部路径）
    svr_->Options(R"(.*)", [add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        res.status = 204;
    });

    // token 门控（Wi-Fi 配对码方案）：非回环（局域网设备）访问 /api/*、
    // /live2d/* 必须携有效 X-DutyOn-Token（= 已配对），否则 401；配对端点
    // /api/pair-request 豁免（设备此时尚无令牌）。回环 = 本机 PC（菜单
    // 动作 / IDE hook 桥接）不受门控。带有效 token 的轮询同时刷新设备
    // 在线时间戳（菜单"设备"子页显示/隐藏在线设置项）。
    svr_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "OPTIONS")  // CORS 预检放行
            return httplib::Server::HandlerResponse::Unhandled;
        if (!isLoopback(req.remote_addr)) {
            const std::string& path = req.path;
            const bool need_auth =
                path.rfind("/api/", 0) == 0 || path.rfind("/live2d/", 0) == 0;
            const bool pairing_ep = (path == "/api/pair-request");
            if (need_auth && !pairing_ep) {
                const std::string tok = req.get_header_value("X-DutyOn-Token");
                if (!pairing_.validateToken(tok)) {
                    res.status = 401;
                    res.set_content("unpaired device", "text/plain");
                    return httplib::Server::HandlerResponse::Handled;
                }
                device_last_seen_.store(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                // 捕获设备上报的程序版本（与在线时间戳同请求刷新，故
                // deviceOnline() 翻真时 deviceVersion() 必为最新）
                {
                    std::lock_guard<std::mutex> lk(ver_mtx_);
                    device_version_ = req.get_header_value("X-DutyOn-Version");
                }
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ---- internal tier（写端点仅限回环）----

    // POST /hook —— IDE 桥接脚本上报事件（snake_case body）
    svr_->Post("/hook", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        if (!loopback_guard(req, res)) return;
        json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded()) {
            res.status = 422;
            res.set_content("invalid JSON", "text/plain");
            return;
        }
        auto ev = HookEvent::fromJson(body);
        if (!ev.has_value()) {
            res.status = 422;
            res.set_content("missing session_id or hook_event_name", "text/plain");
            return;
        }
        // [DIAG] 事件落盘（~/.dutyon/hook-received.log）：排查"子代理期间
        // 宠物误睡"时抓取真实 payload 用（hook 链路多 IDE 多形态，stdout
        // 在 GUI 子系统不可见）
        {
            char tstamp[32];
            const time_t now_sec = time(nullptr);
            struct tm tmv;
            localtime_s(&tmv, &now_sec);
            strftime(tstamp, sizeof(tstamp), "%m-%d %H:%M:%S", &tmv);
            const char* home = std::getenv("USERPROFILE");
            if (home) {
                FILE* df = fopen((std::string(home) + "\\.dutyon\\hook-received.log").c_str(), "a");
                if (df) {
                    fprintf(df, "[%s] %s\n", tstamp, req.body.c_str());
                    fclose(df);
                }
            }
        }
        const std::string project_label = !ev->project_name.empty()
                                              ? ev->project_name
                                              : (!ev->cwd.empty() ? ev->cwd : std::string("?"));
        printf("[HttpServer] event: %s | session=%s | project=%s%s\n",
               ev->hook_event_name.c_str(), ev->session_id.c_str(), project_label.c_str(),
               ev->tool_name.has_value() ? (" | tool=" + *ev->tool_name).c_str() : "");
        sm_.handleHookEvent(*ev);
        okJson(res, {{"ok", true}});
    });

    // POST /unregister —— IDE 关闭时移除会话
    svr_->Post("/unregister", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        if (!loopback_guard(req, res)) return;
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_discarded() && body.contains("session_id") &&
            body["session_id"].is_string()) {
            sm_.removeSession(body["session_id"].get<std::string>());
        }
        okJson(res, {{"ok", true}});
    });

    // POST /log —— 诊断日志转发（落盘 ~/.dutyon/frontend.log）
    svr_->Post("/log", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        if (!loopback_guard(req, res)) return;
        json body = json::parse(req.body, nullptr, false);
        const std::string level =
            (!body.is_discarded() && body.contains("level") && body["level"].is_string())
                ? body["level"].get<std::string>()
                : "info";
        const std::string msg =
            (!body.is_discarded() && body.contains("msg") && body["msg"].is_string())
                ? body["msg"].get<std::string>()
                : "";
        printf("[frontend][%s] %s\n", level.c_str(), msg.c_str());
        appendLogFile(level, msg);
        okJson(res, {{"ok", true}});
    });

    // GET /status —— 当前快照（调试）
    svr_->Get("/status", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        okJson(res, sm_.snapshotJson());
    });

    // GET /live2d/*path —— 用户 Live2D 模型文件（~/.dutyon/live2d/）。
    // 路径校验：拒绝空段与 ..（保证不逃出根目录）
    svr_->Get(R"(/live2d/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string rel = req.matches[1].str();
        auto bad = [&res] {
            res.status = 400;
            res.set_content("invalid path", "text/plain");
        };
        if (rel.empty()) return bad();
        size_t pos = 0;
        while (pos <= rel.size()) {
            const size_t next = rel.find('/', pos);
            const std::string seg =
                rel.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            if (seg.empty() || seg == "..") return bad();
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        const std::string home = homeDir();
        if (home.empty()) {
            res.status = 500;
            res.set_content("home dir unavailable", "text/plain");
            return;
        }
        const fs::path file_path =
            fs::path(home) / ".dutyon" / "live2d" / fs::path(rel);
        if (auto bytes = readFileIfExists(file_path)) {
            std::string ext = file_path.extension().string();
            for (auto& c : ext)
                if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            if (!ext.empty()) ext = ext.substr(1);  // ".png" -> "png"
            res.status = 200;
            res.set_content(*bytes, mimeForExtension(ext));
        } else {
            res.status = 404;
            res.set_content("not found", "text/plain");
        }
    });

    // ---- external tier（任意来源只读）----

    svr_->Get("/health", [this, add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        okJson(res, {{"status", "ok"}, {"port", (int)bc::kPort}});
    });

    // POST /api/pair-request —— 设备配对握手（Wi-Fi 配对码方案）。
    // body {deviceId, code}：已配对 -> {status:"paired", token}（设备拿回
    // 令牌）；未配对 -> 记 pending 并回 {status:"pending"}，等用户在 PC
    // 菜单输入设备屏幕上的配对码确认。免 token 门控（见 pre-routing 豁免）。
    svr_->Post("/api/pair-request",
               [this, add_cors](const httplib::Request& req, httplib::Response& res) {
                   add_cors(res);
                   json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
                   std::string device_id, code;
                   if (!body.is_discarded() && body.is_object()) {
                       device_id = body.value("deviceId", std::string{});
                       code = body.value("code", std::string{});
                   }
                   if (device_id.empty()) {
                       res.status = 400;
                       res.set_content("missing deviceId", "text/plain");
                       return;
                   }
                   std::string token;
                   const std::string st = pairing_.handleRequest(device_id, code,
                                                                 req.remote_addr, &token);
                   if (st == "paired")
                       okJson(res, {{"status", "paired"}, {"token", token}});
                   else
                       okJson(res, {{"status", "pending"}});
               });

    svr_->Get("/api/status", [this, add_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        json j = sm_.snapshotJson();
        // 当前形象键 + 设备模式随快照下发（"char_xxx" = 自定义 GIF；否则
        // Live2D 模型 key；deviceMode = single/multi/frame/voice），硬件屏
        // 据此热切换形象/布局模式与 PC 保持一致。旧版客户端忽略未知字段。
        if (json cfg = readConfigJson(); cfg.is_object()) {
            j["activeCharacter"] = cfg.value("activeCharacterId", std::string{});
            j["deviceMode"] = cfg.value("deviceMode", "multi");
            // 相框播放源（motion=动作轮播 / folder=指定文件夹照片）；旧版
            // 设备端忽略未知字段，行为保持动作轮播
            j["frameSource"] = cfg.value("frameSource", "motion");
            // 时钟颜色：当前角色专属色（characterColors[active]）优先，
            // 其次内置角色出厂默认色，否则全局 clockColor——切换角色时
            // 自动切换显示屏文字颜色
            {
                std::string eff = cfg.value("clockColor", "amber");
                const std::string ckey =
                    cfg.value("activeCharacterId", std::string{});
                bool has_char_color = false;
                if (auto cc = cfg.find("characterColors");
                    cc != cfg.end() && cc->is_object()) {
                    if (auto it = cc->find(ckey);
                        it != cc->end() && it->is_string() &&
                        !it->get<std::string>().empty()) {
                        eff = it->get<std::string>();
                        has_char_color = true;
                    }
                }
                if (!has_char_color) {
                    const std::string def = UserConfig::builtinDefaultColor(ckey);
                    if (!def.empty()) eff = def;
                }
                j["clockColor"] = std::move(eff);
            }
            j["deviceBrightness"] = cfg.value("deviceBrightness", 100);
            j["deviceVolume"] = cfg.value("deviceVolume", 80);
            j["screenRotation"] = cfg.value("screenRotation", 0);
            j["flipHorizontal"] = cfg.value("flipHorizontal", false);
            // 状态音频（设备端状态切换时播放）：activeAudio = 当前角色
            // {状态: 文件名}；soundMute = 完全静音；soundMutedStates =
            // 当前角色被单独静音的状态列表（后端按 activeCharacterId 算好）
            j["soundMute"] = cfg.value("soundMute", false);
            const std::string akey = cfg.value("activeCharacterId", std::string{});
            json audio = json::object();
            if (auto sa = cfg.find("stateAudio");
                sa != cfg.end() && sa->is_object()) {
                if (auto it = sa->find(akey); it != sa->end() && it->is_object())
                    audio = *it;
            }
            j["activeAudio"] = std::move(audio);
            // 状态动作覆盖（PC「动作设定」）：仅下发当前角色的一份
            // {状态: [组, 序号]}，设备端据此与 PC 保持一致；旧版设备忽略未知字段
            json motions = json::object();
            if (auto sm = cfg.find("stateMotions");
                sm != cfg.end() && sm->is_object()) {
                if (auto it = sm->find(akey); it != sm->end() && it->is_object())
                    motions = *it;
            }
            j["stateMotions"] = std::move(motions);
            json muted = json::array();
            if (auto sam = cfg.find("stateAudioMuted");
                sam != cfg.end() && sam->is_object()) {
                for (auto it = sam->begin(); it != sam->end(); ++it)
                    if (it.value().is_boolean() && it.value().get<bool>() &&
                        it.key().rfind(akey + ":", 0) == 0)
                        muted.push_back(it.key().substr(akey.size() + 1));
            }
            j["soundMutedStates"] = std::move(muted);
        }
        // 欢迎信号序号（设备成功连接时 PC 递增）：设备端检测到增大即播一次欢迎
        j["welcomeSeq"] = welcome_seq_.load();
        // PC 时间（设备无 RTC/网络不可信，时钟跟随 PC）：epoch 秒 +
        // 本地时区偏移分钟（东八区=480），设备端 steady_clock 自行推进
        {
            const time_t now_sec = time(nullptr);
            struct tm lt, gt;
            localtime_s(&lt, &now_sec);
            gmtime_s(&gt, &now_sec);
            // 偏移秒 = 本地时刻 - UTC 时刻（含跨日/跨年的 yday 差）
            const long offset_sec =
                (lt.tm_yday - gt.tm_yday) * 86400L +
                (lt.tm_hour - gt.tm_hour) * 3600L +
                (lt.tm_min - gt.tm_min) * 60L + (lt.tm_sec - gt.tm_sec);
            j["serverTime"] = (double)now_sec;
            j["utcOffset"] = (int)(offset_sec / 60);
        }
        // 挂起的"重新配网"指令（换 WiFi 场景）：token 反查请求者身份，
        // 匹配目标设备才注入；设备执行后 POST /api/cmd-ack 清除
        if (const std::string tok = req.get_header_value("X-DutyOn-Token");
            !tok.empty()) {
            const std::string cmd_id =
                pairing_.pendingResetWifiFor(pairing_.deviceByToken(tok));
            if (!cmd_id.empty())
                j["deviceCmd"] = {{"type", "reset-wifi"}, {"id", cmd_id}};
        }
        okJson(res, j);
    });

    // POST /api/cmd-ack —— 设备指令回执。body {id}；设备身份从请求 token
    // 反查（不采信 body 里的自报 deviceId），id 匹配挂起指令即清除。
    // 失败不清：设备下一轮 status 会重复收到 deviceCmd，ack 幂等
    svr_->Post("/api/cmd-ack", [this, add_cors](const httplib::Request& req,
                                               httplib::Response& res) {
        add_cors(res);
        json body;
        try {
            body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        } catch (...) {
        }
        if (!body.is_object()) {
            res.status = 400;
            return;
        }
        const std::string tok = req.get_header_value("X-DutyOn-Token");
        const std::string did = pairing_.deviceByToken(tok);
        const std::string id = body.value("id", std::string{});
        if (did.empty() || id.empty()) {
            res.status = 400;
            return;
        }
        pairing_.ackResetWifi(did, id);
        okJson(res, {{"status", "ok"}});
    });

    // GET /api/character —— 当前角色详情（硬件屏拉取自定义 GIF 定义用）。
    // 返回 {"type":"custom","id","name","sleeping","working","alert"} 或
    // {"type":"live2d","id":<模型key>}；文件名相对 ~/.dutyon/animations/。
    svr_->Get("/api/character", [add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        const json cfg = readConfigJson();
        if (!cfg.is_object()) {
            res.status = 500;
            res.set_content("config unavailable", "text/plain");
            return;
        }
        const std::string id = cfg.value("activeCharacterId", std::string{});
        json out;
        if (id.rfind("char_", 0) == 0) {
            out = {{"type", "custom"}, {"id", id}};
            for (const auto& c : cfg.value("customCharacters", json::array())) {
                if (!c.is_object() || c.value("id", std::string{}) != id) continue;
                out["name"] = c.value("name", std::string{});
                out["sleeping"] = c.value("sleeping", std::string{});
                out["working"] = c.value("working", std::string{});
                out["alert"] = c.value("alert", std::string{});
                break;
            }
        } else {
            out = {{"type", "live2d"}, {"id", id}};
        }
        okJson(res, out);
    });

    // GET /api/animations/<file> —— 自定义形象动画文件（~/.dutyon/animations/）。
    // 仅单文件名（无子目录）；拒绝 .. 防逃逸（校验同 /live2d）。
    svr_->Get(R"(/api/animations/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1].str();
        if (name.empty() || name == "..") {
            res.status = 400;
            res.set_content("invalid path", "text/plain");
            return;
        }
        const std::string home = homeDir();
        if (home.empty()) {
            res.status = 500;
            res.set_content("home dir unavailable", "text/plain");
            return;
        }
        const fs::path file_path = fs::path(home) / ".dutyon" / "animations" / name;
        if (auto bytes = readFileIfExists(file_path)) {
            // 动画文件现支持 GIF/PNG/JPG（自定义角色静态图）；按扩展名给 MIME
            std::string ext = file_path.extension().string();
            for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
            const char* mime = ext == ".png"  ? "image/png"
                               : ext == ".jpg" || ext == ".jpeg" ? "image/jpeg"
                                                                 : "image/gif";
            res.status = 200;
            res.set_content(*bytes, mime);
        } else {
            res.status = 404;
            res.set_content("not found", "text/plain");
        }
    });

    // GET /api/frame/photo —— 相框「指定文件夹」随机下发一张（不落盘设备）。
    // 参数 max_side（默认 1536）：超限照片由 PC 端缩放重编码，回包可能是
    // 原文件也可能是重编码 JPEG，统一按 image/jpeg 给（设备端 stb 自识格式）。
    // 照片留在 PC 磁盘：设备每播完一张才要下一张，随机在 PC 端抽，设备侧
    // 内存里始终只有一张。
    svr_->Get(R"(/api/frame/photo)",
              [](const httplib::Request& req, httplib::Response& res) {
                  const json cfg = readConfigJson();
                  const std::string folder = cfg.value("frameFolder", std::string{});
                  auto no_photo = [&res](const char* why) {
                      res.status = 404;
                      res.set_content(why, "text/plain");
                  };
                  if (folder.empty()) return no_photo("no frame folder");
                  int max_side = kFrameMaxSide;
                  if (req.has_param("max_side")) {
                      const int v = atoi(req.get_param_value("max_side").c_str());
                      if (v >= 256 && v <= 4096) max_side = v;
                  }
                  const auto files = framePhotoList(fs::path(folder));
                  if (files.empty()) return no_photo("no photos");
                  const fs::path& pick = files[framePickReservoir(files.size())];
                  auto bytes = readFileIfExists(pick);
                  if (!bytes.has_value() || bytes->empty())
                      return no_photo("photo unreadable");
                  std::error_code ec;
                  const auto sz = fs::file_size(pick, ec);
                  std::string body;
                  bool scaled = false;
                  // 原文件已足够小：直接透传（零 CPU，保留 PNG 无损）
                  if (sz <= 1536 * 1024) {
                      body = std::move(*bytes);
                  } else {
                      body = framePhotoScaled(pick, max_side);
                      scaled = !body.empty();
                      if (!scaled) body = std::move(*bytes);  // 重编码失败回退原图
                  }
                  res.status = 200;
                  res.set_header("X-DutyOn-Photo",
                                 pick.filename().string());
                  res.set_header("X-DutyOn-Photo-Scaled", scaled ? "1" : "0");
                  res.set_content(std::move(body), "image/jpeg");
              });

    // GET /api/events —— SSE 状态流。每次状态机有效变更推完整 Snapshot；
    // 15s keep-alive 注释防代理掐空闲连接。
    svr_->Get("/api/events", [this, add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        res.status = 200;
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                uint64_t last = sm_.version();
                while (sink.is_writable()) {
                    if (sm_.waitVersion(last, 15000)) {
                        last = sm_.version();
                        const std::string data = sm_.snapshotJson().dump();
                        const std::string msg = "data: " + data + "\n\n";
                        if (!sink.write(msg.data(), msg.size())) return false;
                    } else {
                        // keep-alive 注释行（对齐 Rust 版 KeepAlive text）
                        static const char kKeepAlive[] = ": keep-alive\n\n";
                        if (!sink.write(kKeepAlive, sizeof(kKeepAlive) - 1)) return false;
                    }
                }
                return true;
            });
    });

    // GET /api/metrics —— 最新系统指标（轮询即隐式激活采样器；首采前 503）
    svr_->Get("/api/metrics", [this, add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        monitor_.pokeActive();
        auto m = monitor_.latestMetrics();
        if (!m.has_value()) {
            res.status = 503;
            res.set_content("metrics not ready yet", "text/plain");
            return;
        }
        json j = {
            {"cpuUsage", m->cpu_usage},
            {"memTotal", m->mem_total},
            {"memUsed", m->mem_used},
            {"gpuName", m->has_gpu ? json(m->gpu_name) : json(nullptr)},
            {"gpuUsage", m->has_gpu ? json(m->gpu_usage) : json(nullptr)},
            {"vramTotal", m->has_gpu ? json(m->vram_total) : json(nullptr)},
            {"vramUsed", m->has_gpu ? json(m->vram_used) : json(nullptr)},
            {"netRxRate", m->net_rx_rate},
            {"netTxRate", m->net_tx_rate},
            {"selfCpu", m->self_cpu},
            {"selfMem", m->self_mem},
        };
        okJson(res, j);
    });

    // GET /api/sounds/:state —— 状态音效（~/.dutyon/sounds/<state>.{mp3,wav,ogg}）。
    // state 名校验为字母数字+连字符，防路径逃逸
    svr_->Get(R"(/api/sounds/([A-Za-z0-9\-]+))",
              [](const httplib::Request& req, httplib::Response& res) {
                  const std::string state = req.matches[1].str();
                  const std::string home = homeDir();
                  if (home.empty()) {
                      res.status = 500;
                      res.set_content("home dir unavailable", "text/plain");
                      return;
                  }
                  const fs::path dir = fs::path(home) / ".dutyon" / "sounds";
                  for (const char* ext : {"mp3", "wav", "ogg"}) {
                      const fs::path file = dir / (state + "." + ext);
                      if (auto bytes = readFileIfExists(file)) {
                          res.status = 200;
                          res.set_content(*bytes, mimeForExtension(ext));
                          return;
                      }
                  }
                  res.status = 404;
                  res.set_content("no sound for this state", "text/plain");
              });

    // ---- pet-client tier（POST 动作，仅限回环）----

    // GET /api/hooks —— 安装状态诊断
    svr_->Get("/api/hooks", [add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        okJson(res, isHooksInstalled().toJson());
    });

    // POST /api/hooks/install —— 静默安装/刷新 IDE hooks
    svr_->Post("/api/hooks/install", [add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        okJson(res, installHooks(resolveHooksSourceDir()).toJson());
    });

    // POST /api/bring-to-front —— 项目行点击前置 IDE 窗口
    svr_->Post("/api/bring-to-front",
               [add_cors](const httplib::Request& req, httplib::Response& res) {
                   add_cors(res);
                   json body = json::parse(req.body, nullptr, false);
                   std::string target;
                   if (!body.is_discarded() && body.contains("target") &&
                       body["target"].is_string()) {
                       target = body["target"].get<std::string>();
                   }
                   if (target.empty()) {
                       res.status = 400;
                       res.set_content("missing target", "text/plain");
                       return;
                   }
                   // target 可能是完整路径（取末段文件夹名）或直接是项目名
                   const size_t slash = target.find_last_of("\\/");
                   const std::string name =
                       slash == std::string::npos ? target : target.substr(slash + 1);
                   const bool focused = focusProjectWindow(name);
                   okJson(res, {{"focused", focused}});
               });

    // GET /api/autostart —— 自启动状态
    svr_->Get("/api/autostart", [add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        okJson(res, {{"enabled", autostartEnabled()}});
    });

    // POST /api/autostart —— 开关自启动（body {"enabled": bool}）
    svr_->Post("/api/autostart", [add_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        json body = json::parse(req.body, nullptr, false);
        const bool enabled =
            !body.is_discarded() && body.contains("enabled") && body["enabled"].is_boolean()
                ? body["enabled"].get<bool>()
                : false;
        if (setAutostartEnabled(enabled)) {
            okJson(res, {{"ok", true}, {"enabled", enabled}});
        } else {
            okJson(res, {{"ok", false}, {"enabled", enabled}});
        }
    });

    // POST /api/quit —— 整个应用退出
    svr_->Post("/api/quit", [this, add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        if (quit_handler_) {
            // 先回包再触发退出（同进程下直接退出会掐断响应）
            okJson(res, {{"ok", true}});
            quit_handler_();
        } else {
            res.status = 503;
            res.set_content("app handle not registered", "text/plain");
        }
    });
}

} // namespace dutyon::backend

#endif // _WIN32
