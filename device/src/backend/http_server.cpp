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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "backend/autostart.h"
#include "backend/backend_config.h"
#include "backend/hooks_installer.h"
#include "backend/ide_scanner.h"

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

} // namespace

HttpServer::HttpServer(StateManager& sm, SysMonitor& monitor) : sm_(sm), monitor_(monitor) {}

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

    if (!svr_->bind_to_port(bc::kHost, (int)bc::kPort)) {
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
    printf("[HttpServer] Listening on http://%s:%u\n", bc::kHost, (unsigned)bc::kPort);
    return true;
}

void HttpServer::stop() {
    if (svr_) {
        svr_->stop();
        delete svr_;
        svr_ = nullptr;
    }
    running_ = false;
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

    svr_->Get("/api/status", [this, add_cors](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        okJson(res, sm_.snapshotJson());
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
