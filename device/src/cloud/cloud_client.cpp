// 云端账户客户端实现（仅 Windows PC 端）—— 见 cloud_client.h 头注释。

#ifdef _WIN32

#include "cloud/cloud_client.h"

// Winsock 头顺序同 http_server.cpp：winsock2/ws2tcpip 先于 windows.h
//（后者 WIN32_LEAN_AND_MEAN），否则与 httplib.h 内部 winsock2.h 冲突。
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "config/user_config.h"
#include "sha256.h"

namespace dutyon {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string homeDir() {
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    return home ? std::string(home) : std::string();
}

std::string configPath() {
    const std::string home = homeDir();
    if (home.empty()) return "config.json";
    return home + "/.dutyon/config.json";
}

// 升级工作目录：%TEMP%\dutyon-update（pkg.zip / staged\ / updater.cmd）
std::string updateTempDir() {
    char buf[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, buf);
    return std::string(buf) + "dutyon-update";
}

std::wstring toWide(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)(n > 1 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
    std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

// URL 编码（query 参数里的文件路径可能含中文/空格）
std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

json readJsonFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return json::object();
    try {
        json j;
        in >> j;
        if (!j.is_object()) return json::object();
        return j;
    } catch (...) {
        return json::object();
    }
}

bool writeJsonFile(const std::string& path, const json& j) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << j.dump(2) << std::endl;
    return true;
}

// semver 比较：a>b 返回 1、相等 0、a<b 返回 -1（分段数字，缺段补 0）
int compareVersion(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v) {
        std::vector<int> parts;
        std::string cur;
        for (char c : v) {
            if (c == '.') {
                parts.push_back(atoi(cur.c_str()));
                cur.clear();
            } else {
                cur += c;
            }
        }
        parts.push_back(atoi(cur.c_str()));
        return parts;
    };
    const auto pa = split(a), pb = split(b);
    const size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
    for (size_t i = 0; i < n; ++i) {
        const int x = i < pa.size() ? pa[i] : 0;
        const int y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x > y ? 1 : -1;
    }
    return 0;
}

// 隐藏窗口执行命令行并等待结束（PowerShell 解压用）
bool runHiddenAndWait(const std::wstring& cmdline) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;  // CreateProcessW 可修改命令行缓冲
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::string nowHm() {
    const auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    struct tm tmv;
    localtime_s(&tmv, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return buf;
}

// 递归收集目录下全部常规文件的相对路径（正斜杠）
void collectFiles(const fs::path& root, const std::string& prefix,
                  std::vector<std::string>& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        std::string rel =
            (prefix.empty() ? std::string()
                            : prefix + "/") +
            e.path().lexically_relative(root).generic_string();
        out.push_back(std::move(rel));
    }
}

} // namespace

CloudClient::~CloudClient() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

// ---- 账户 ----

bool CloudClient::parseServerUrl(const std::string& url, std::string& host,
                                 int& port) const {
    std::string u = url;
    // 去尾部斜杠
    while (!u.empty() && u.back() == '/') u.pop_back();
    const std::string scheme = "http://";
    if (u.rfind(scheme, 0) != 0) return false;  // 仅支持 http（v1 明文）
    u = u.substr(scheme.size());
    if (u.empty() || u.find('/') != std::string::npos) return false;
    const auto colon = u.rfind(':');
    if (colon == std::string::npos) {
        host = u;
        port = 8787;  // 省略端口用服务端默认口
    } else {
        host = u.substr(0, colon);
        port = atoi(u.c_str() + colon + 1);
        if (host.empty() || port <= 0 || port > 65535) return false;
    }
    return !host.empty();
}

std::string CloudClient::registerAccount(const std::string& server,
                                         const std::string& user,
                                         const std::string& pass) {
    std::string host;
    int port = 0;
    if (!parseServerUrl(server, host, port)) return "服务器地址无效（形如 http://192.168.1.100:8787）";
    if (user.size() < 3 || user.size() > 32) return "用户名需 3-32 位";
    if (pass.size() < 6) return "密码至少 6 位";
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Post("/api/auth/register",
                        json{{"username", user}, {"password", pass}}.dump(),
                        "application/json");
    if (!res) return "无法连接服务器";
    json j = json::parse(res->body, nullptr, false);
    if (res->status != 201)
        return j.value("error", "注册失败 (" + std::to_string(res->status) + ")");
    std::lock_guard<std::mutex> lk(mtx_);
    account_ = {server, j.value("token", ""), user};
    UserConfigStore::saveCloudAccount(server, account_.token, user);
    return {};
}

std::string CloudClient::login(const std::string& server,
                               const std::string& user,
                               const std::string& pass) {
    std::string host;
    int port = 0;
    if (!parseServerUrl(server, host, port)) return "服务器地址无效（形如 http://192.168.1.100:8787）";
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Post("/api/auth/login",
                        json{{"username", user}, {"password", pass}}.dump(),
                        "application/json");
    if (!res) return "无法连接服务器";
    json j = json::parse(res->body, nullptr, false);
    if (res->status != 200)
        return j.value("error", "登录失败 (" + std::to_string(res->status) + ")");
    std::lock_guard<std::mutex> lk(mtx_);
    account_ = {server, j.value("token", ""), user};
    UserConfigStore::saveCloudAccount(server, account_.token, user);
    return {};
}

void CloudClient::logout() {
    // 先停 worker 再清账户（避免线程读到半空状态）
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    stop_ = false;
    std::string host, token;
    int port = 0;
    std::string server;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        server = account_.server;
        token = account_.token;
    }
    if (!token.empty() && parseServerUrl(server, host, port)) {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(3);
        httplib::Headers h = {{"Authorization", "Bearer " + token}};
        cli.Post("/api/auth/logout", h, "", "application/json");
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        account_.token.clear();
        account_.username.clear();
        UserConfigStore::saveCloudAccount(server, "", "");
        sync_hint_.clear();
    }
    startWorker();  // worker 保持运行（升级检查与登录态解耦）
}

std::string CloudClient::username() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return account_.username;
}

void CloudClient::setAccount(const std::string& server, const std::string& token,
                             const std::string& username) {
    std::lock_guard<std::mutex> lk(mtx_);
    account_ = {server, token, username};
}

std::string CloudClient::serverUrl() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return account_.server;
}

// ---- 后台任务 ----

void CloudClient::startWorker() {
    if (worker_.joinable()) return;  // 幂等
    stop_ = false;
    worker_ = std::thread([this] { workerLoop(); });
}

void CloudClient::syncNow() { sync_requested_ = true; }
void CloudClient::requestRestore() { restore_requested_ = true; }
void CloudClient::checkUpdate() { update_check_requested_ = true; }
void CloudClient::startUpdateDownload() { update_download_requested_ = true; }

void CloudClient::workerLoop() {
    auto last_upload = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    while (!stop_) {
        // 1s 节拍：处理一次性请求 + 周期上传
        for (int i = 0; i < 10 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 升级检查/下载与登录态解耦（未登录也可检查更新）
        if (update_check_requested_.exchange(false)) doCheckUpdate();
        if (update_download_requested_.exchange(false)) doDownloadUpdate();

        bool logged = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            logged = !account_.token.empty();
        }
        if (!logged) continue;

        if (restore_requested_.exchange(false)) {
            std::string err;
            doRestore(err);
            if (!err.empty()) setSyncHint("云恢复失败：" + err);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_upload >= std::chrono::seconds(60) ||
            sync_requested_.exchange(false)) {
            last_upload = now;
            std::string err;
            setSyncHint("正在同步…");
            if (doUploadOnce(err)) {
                setSyncHint("已同步 " + nowHm());
            } else {
                setSyncHint("同步失败：" + err);
            }
        }
    }
}

// ---- 增量上传 ----

bool CloudClient::doUploadOnce(std::string& err) {
    std::string host, token;
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!parseServerUrl(account_.server, host, port) ||
            account_.token.empty()) {
            err = "未登录";
            return false;
        }
        token = account_.token;
    }
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);
    httplib::Headers auth = {{"Authorization", "Bearer " + token}};

    const std::string home = homeDir();
    const std::string anim_dir = UserConfigStore::animationsDir();
    const std::string l2d_dir = UserConfigStore::userModelsDir();

    // 本地文件清单（size+mtime 缓存命中免算 sha256）
    const std::string cache_path = home + "/.dutyon/cloud-cache.json";
    json cache = readJsonFile(cache_path);
    if (!cache.contains("files") || !cache["files"].is_object())
        cache["files"] = json::object();
    json& cache_files = cache["files"];

    struct LocalFile {
        std::string rel;      // 相对 ~/.dutyon/（animations/x.gif）
        std::string abs;
        std::string sha256;
        long long size;
    };
    std::vector<LocalFile> locals;
    std::vector<std::string> rels;
    collectFiles(fs::path(anim_dir), "animations", rels);
    collectFiles(fs::path(l2d_dir), "live2d", rels);
    for (const auto& rel : rels) {
        const fs::path full = fs::path(home) / ".dutyon" / fs::path(rel);
        std::error_code ec;
        if (!fs::is_regular_file(full, ec)) continue;
        const auto sz = (long long)fs::file_size(full, ec);
        if (ec) continue;
        const auto mt = fs::last_write_time(full, ec);
        const long long mtime =
            (long long)mt.time_since_epoch().count();
        std::string sha;
        const json& c = cache_files.contains(rel) ? cache_files[rel] : json();
        if (c.is_object() && c.value("s", (long long)-1) == sz &&
            c.value("m", (long long)-1) == mtime && c.contains("h")) {
            sha = c.value("h", std::string());
        }
        if (sha.empty()) {
            sha = SHA256::fileHex(full.string());
            if (sha.empty()) continue;  // 读失败跳过
            cache_files[rel] = {{"s", sz}, {"m", mtime}, {"h", sha}};
        }
        locals.push_back({rel, full.string(), sha, sz});
    }

    // 云端清单
    auto res = cli.Get("/api/sync/manifest", auth);
    if (!res || res->status != 200) {
        err = res ? ("清单获取失败 (" + std::to_string(res->status) + ")")
                  : "无法连接服务器";
        return false;
    }
    json remote = json::parse(res->body, nullptr, false);
    std::map<std::string, std::string> remote_map;  // rel → sha256
    if (remote.is_array()) {
        for (const auto& e : remote) {
            if (e.is_object() && e.contains("path") && e.contains("sha256"))
                remote_map[e["path"].get<std::string>()] =
                    e["sha256"].get<std::string>();
        }
    }

    // 差异：本地有而云端无/不同 → 上传；云端有而本地无 → 删除
    for (const auto& lf : locals) {
        auto it = remote_map.find(lf.rel);
        if (it != remote_map.end() && it->second == lf.sha256) continue;
        std::ifstream in(lf.abs, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string body = ss.str();
        auto up = cli.Put("/api/sync/file?path=" + urlEncode(lf.rel), auth,
                          body, "application/octet-stream");
        if (!up || up->status != 200) {
            err = "上传 " + lf.rel + " 失败";
            return false;
        }
    }
    for (const auto& [rel, sha] : remote_map) {
        bool found = false;
        for (const auto& lf : locals)
            if (lf.rel == rel) {
                found = true;
                break;
            }
        if (!found) {
            auto del = cli.Delete("/api/sync/file?path=" + urlEncode(rel), auth);
            if (!del || del->status != 200) {
                err = "删除云端 " + rel + " 失败";
                return false;
            }
        }
    }

    // 配置白名单快照
    json snapshot = json::object();
    {
        const json local_cfg = readJsonFile(configPath());
        for (const char* key : cloudSyncFields())
            if (local_cfg.contains(key)) snapshot[key] = local_cfg[key];
    }
    auto cfg_res = cli.Put("/api/sync/config", auth, snapshot.dump(),
                           "application/json");
    if (!cfg_res || cfg_res->status != 200) {
        err = "配置快照上传失败";
        return false;
    }

    writeJsonFile(cache_path, cache);
    return true;
}

// ---- 云恢复 ----

bool CloudClient::doRestore(std::string& err) {
    std::string host, token;
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!parseServerUrl(account_.server, host, port) ||
            account_.token.empty()) {
            err = "未登录";
            return false;
        }
        token = account_.token;
    }
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(60);
    httplib::Headers auth = {{"Authorization", "Bearer " + token}};

    // 1. 配置合并：云端白名单字段覆盖本地（本机字段保留）
    auto cfg_res = cli.Get("/api/sync/config", auth);
    if (!cfg_res || cfg_res->status != 200) {
        err = "云端配置获取失败";
        return false;
    }
    json cj = json::parse(cfg_res->body, nullptr, false);
    int restored_files = 0;
    if (cj.is_object() && cj.contains("config") &&
        cj["config"].is_object()) {
        const json& remote_cfg = cj["config"];
        json local_cfg = readJsonFile(configPath());
        for (const char* key : cloudSyncFields())
            if (remote_cfg.contains(key)) local_cfg[key] = remote_cfg[key];
        writeJsonFile(configPath(), local_cfg);
    }

    // 2. 文件：逐个比对 sha256，差异下载（相对路径原位落盘）
    auto res = cli.Get("/api/sync/manifest", auth);
    if (!res || res->status != 200) {
        err = "云端清单获取失败";
        return false;
    }
    json remote = json::parse(res->body, nullptr, false);
    const std::string home = homeDir();
    if (remote.is_array()) {
        for (const auto& e : remote) {
            if (!e.is_object() || !e.contains("path")) continue;
            const std::string rel = e["path"].get<std::string>();
            const std::string sha =
                e.value("sha256", std::string());
            // 路径白名单（同服务端规则）：仅 animations/ 与 live2d/
            if (rel.rfind("animations/", 0) != 0 &&
                rel.rfind("live2d/", 0) != 0)
                continue;
            if (rel.find("..") != std::string::npos) continue;
            const fs::path full = fs::path(home) / ".dutyon" / fs::path(rel);
            if (SHA256::fileHex(full.string()) == sha) continue;  // 已一致
            std::error_code ec;
            fs::create_directories(full.parent_path(), ec);
            std::ofstream out(full, std::ios::binary);
            if (!out) continue;
            auto fr = cli.Get("/api/sync/file?path=" + urlEncode(rel), auth,
                              [&](const char* data, size_t len) {
                                  out.write(data, (std::streamsize)len);
                                  return out.good();
                              });
            out.close();
            if (!fr || fr->status != 200) {
                fs::remove(full, ec);  // 半截文件不留
                err = "下载 " + rel + " 失败";
                return false;
            }
            ++restored_files;
        }
    }

    // 3. 失效本地哈希缓存（文件已变，下轮上传重算）
    std::error_code ec;
    fs::remove(fs::path(home) / ".dutyon" / "cloud-cache.json", ec);

    // 4. 置恢复完成标志：主线程重载配置/切换角色（见 main.cpp）
    setSyncHint(restored_files > 0
                    ? ("已从云端恢复 " + std::to_string(restored_files) +
                       " 个文件")
                    : "云端无差异，配置已核对");
    restore_done_ = true;
    return true;
}

// ---- 升级 ----

void CloudClient::doCheckUpdate() {
    std::string server = serverUrl();
    if (server.empty()) {
        setUpdateHint("未配置云端服务器");
        return;
    }
    std::string host;
    int port = 0;
    if (!parseServerUrl(server, host, port)) {
        setUpdateHint("云端服务器地址无效");
        return;
    }
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Get("/api/app/version");
    if (!res) {
        setUpdateHint("无法连接服务器");
        std::lock_guard<std::mutex> lk(mtx_);
        update_state_ = UpdateState::kFailed;
        return;
    }
    if (res->status == 404) {
        setUpdateHint(std::string("当前 ") + DUTYON_VERSION + "（已是最新）");
        std::lock_guard<std::mutex> lk(mtx_);
        update_state_ = UpdateState::kIdle;
        update_info_ = UpdateInfo{};
        return;
    }
    if (res->status != 200) {
        setUpdateHint("版本检查失败");
        std::lock_guard<std::mutex> lk(mtx_);
        update_state_ = UpdateState::kFailed;
        return;
    }
    json j = json::parse(res->body, nullptr, false);
    UpdateInfo info;
    info.version = j.value("version", "");
    info.notes = j.value("notes", "");
    info.size = j.value("size", (long long)0);
    info.sha256 = j.value("sha256", "");
    if (info.version.empty() ||
        compareVersion(info.version, DUTYON_VERSION) <= 0) {
        setUpdateHint(std::string("当前 ") + DUTYON_VERSION + "（已是最新）");
        std::lock_guard<std::mutex> lk(mtx_);
        update_state_ = UpdateState::kIdle;
        update_info_ = UpdateInfo{};
        return;
    }
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%.1f MB",
             (double)info.size / 1048576.0);
    setUpdateHint("发现 " + info.version + " (" + size_str + ")");
    std::lock_guard<std::mutex> lk(mtx_);
    update_state_ = UpdateState::kAvailable;
    update_info_ = info;
}

void CloudClient::doDownloadUpdate() {
    UpdateInfo info;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        info = update_info_;
        update_state_ = UpdateState::kDownloading;
    }
    if (info.version.empty() || info.sha256.empty()) {
        setUpdateHint("升级信息缺失，请先检查更新");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            update_state_ = UpdateState::kFailed;
        }
        return;
    }
    std::string host;
    int port = 0;
    if (!parseServerUrl(serverUrl(), host, port)) {
        setUpdateHint("未配置云端服务器");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            update_state_ = UpdateState::kFailed;
        }
        return;
    }
    httplib::Client cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(120);

    const std::string dir = updateTempDir();
    std::error_code ec;
    fs::remove_all(fs::path(dir), ec);
    fs::create_directories(fs::path(dir), ec);
    const std::string zip_path = dir + "\\pkg.zip";
    std::ofstream out(zip_path, std::ios::binary);
    if (!out) {
        setUpdateHint("升级包写入失败");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            update_state_ = UpdateState::kFailed;
        }
        return;
    }
    long long received = 0;
    const long long total = info.size;
    auto res = cli.Get(
        "/api/app/download",
        [&](const char* data, size_t len) {
            out.write(data, (std::streamsize)len);
            received += (long long)len;
            if (total > 0 && (received % (1 << 20)) < (long long)len) {
                char p[64];
                snprintf(p, sizeof(p), "%d%%", (int)(received * 100 / total));
                setUpdateHint(std::string("下载 ") + p);
            }
            return out.good();
        });
    out.close();
    if (!res || res->status != 200) {
        setUpdateHint("升级包下载失败");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            update_state_ = UpdateState::kFailed;
        }
        return;
    }

    // sha256 校验
    if (SHA256::fileHex(zip_path) != info.sha256) {
        setUpdateHint("升级包校验失败");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            update_state_ = UpdateState::kFailed;
        }
        return;
    }

    // PowerShell 解压（零 C++ zip 依赖）到 staged 目录
    const std::string staged = dir + "\\staged";
    fs::create_directories(fs::path(staged), ec);
    std::wstring ps = L"powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '";
    ps += toWide(zip_path);
    ps += L"' -DestinationPath '";
    ps += toWide(staged);
    ps += L"' -Force\"";
    setUpdateHint("解压中…");
    if (!runHiddenAndWait(ps)) {
        setUpdateHint("解压失败");
        {
            std::lock_guard<std::mutex> lk(mtx_);
            update_state_ = UpdateState::kFailed;
        }
        return;
    }
    setUpdateHint("更新就绪，点击安装");
    {
        std::lock_guard<std::mutex> lk(mtx_);
        update_state_ = UpdateState::kReady;
    }
}

bool CloudClient::applyUpdate() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (update_state_ != UpdateState::kReady) return false;
    }
    // exe 安装目录 + PID
    wchar_t exe_buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    const fs::path exe_dir = fs::path(exe_buf).parent_path();
    const std::string dir = updateTempDir();
    const std::string staged = dir + "\\staged";
    std::error_code ec;
    if (!fs::is_directory(fs::path(staged), ec)) return false;

    // updater.cmd：等本进程退出 → xcopy 覆盖 → 清理 → 重启 → 自删
    //（运行中的 exe/dll 被锁，必须等退出后由外部进程替换）
    const std::string pid = std::to_string(GetCurrentProcessId());
    std::ostringstream cmd;
    cmd << "@echo off\r\n"
        << ":wait\r\n"
        << "tasklist /FI \"PID eq " << pid << "\" 2>nul | find \"" << pid
        << "\" >nul 2>&1\r\n"
        << "if not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait)\r\n"
        << "xcopy /E /Y \"" << staged << "\\*\" \""
        << exe_dir.string() << "\\\" >nul 2>&1\r\n"
        << "rd /s /q \"" << dir << "\" >nul 2>&1\r\n"
        << "start \"\" \"" << (exe_dir / "dutyon-pet.exe").string() << "\"\r\n"
        << "del \"%~f0\" >nul 2>&1\r\n";

    const std::string cmd_path = dir + "\\updater.cmd";
    {
        std::ofstream f(cmd_path, std::ios::binary);
        if (!f) return false;
        f << cmd.str();
    }

    // 分离启动（新进程组 + 隐藏窗口），随后本程序退出
    std::wstring run = L"cmd.exe /c \"";
    run += toWide(cmd_path);
    run += L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = run;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS |
                            CREATE_NEW_PROCESS_GROUP,
                        nullptr, toWide(dir).c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void CloudClient::cleanupUpdateLeftovers() {
    std::error_code ec;
    fs::remove_all(fs::path(updateTempDir()), ec);
    // exe 旁的旧备份兜底（当前方案不产生，防未来变更遗留）
    wchar_t exe_buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    fs::remove(fs::path(exe_buf).parent_path() / "dutyon-pet.old", ec);
}

// ---- 状态 ----

bool CloudClient::restoreDone() const { return restore_done_; }
void CloudClient::consumeRestore() { restore_done_ = false; }

std::string CloudClient::syncHint() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sync_hint_;
}

CloudClient::UpdateState CloudClient::updateState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return update_state_;
}

std::string CloudClient::updateHint() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return update_hint_;
}

void CloudClient::setSyncHint(const std::string& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    sync_hint_ = s;
}

void CloudClient::setUpdateHint(const std::string& s) {
    std::lock_guard<std::mutex> lk(mtx_);
    update_hint_ = s;
}

} // namespace dutyon

#endif // _WIN32
