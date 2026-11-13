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
#include "cloud/cloud_transport.h"
#include <regex>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                      nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr,
                            nullptr);
    return s;
}

bool validDigest(const std::string& s) {
    return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
}
bool validCloudPath(const std::string& s) {
    for (unsigned char c : s) if (c < 32 || c == 127) return false;
    if (s.size()>512 || (s.rfind("animations/",0)!=0 && s.rfind("live2d/",0)!=0) ||
        s.find("..")!=std::string::npos || s.find_first_of("\\:\0<>\"|?*",0,9)!=std::string::npos) return false;
    std::istringstream input(s); std::string part;
    while(std::getline(input,part,'/')) {
        if(part.empty() || part=="." || part.back()=='.' || part.back()==' ') return false;
        std::string base=part.substr(0,part.find('.'));
        for(auto& c:base) c=(char)toupper((unsigned char)c);
        if(base=="CON" || base=="PRN" || base=="AUX" || base=="NUL" ||
           (base.size()==4 && (base.rfind("COM",0)==0 || base.rfind("LPT",0)==0) && base[3]>='0' && base[3]<='9')) return false;
    }
    return !s.empty() && s.back()!='/';
}
std::wstring psQuote(const std::string& s) {
    std::wstring out; for(wchar_t c:toWide(s)) {out+=c; if(c==L'\'') out+=c;} return out;
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
    std::ifstream in(fs::u8path(path), std::ios::binary);
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
    const auto target = fs::u8path(path);
    fs::create_directories(target.parent_path(), ec);
    auto temp = target;
    temp += L".cloud-write";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << '\n';
        out.close();
        if (!out) { fs::remove(temp, ec); return false; }
    }
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    fs::remove(temp, ec);
    return false;
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
    DWORD exitCode=1; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode==0;
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
            e.path().lexically_relative(root).generic_u8string();
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
    CloudTransport transport(url);
    host=url; port=0;
    return transport.valid();
}

std::string CloudClient::registerAccount(const std::string& server,
                                         const std::string& user,
                                         const std::string& pass) {
    std::string host;
    int port = 0;
    if (!parseServerUrl(server, host, port)) return "服务器地址无效（形如 https://cloud.example.com）";
    if (user.size() < 3 || user.size() > 32) return "用户名需 3-32 位";
    if (pass.size() < 6) return "密码至少 6 位";
    CloudTransport cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Post("/api/auth/register",
                        json{{"username", user}, {"password", pass}}.dump(),
                        "application/json");
    if (!res) return "无法连接服务器";
    json j = json::parse(res->body, nullptr, false);
    if (!j.is_object()) return "服务器返回格式错误";
    if (res->status != 201)
        return j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "注册失败";
    if (!j.contains("token") || !j["token"].is_string() || j["token"].get<std::string>().size()!=64) return "服务器返回的登录凭据无效";
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
    if (!parseServerUrl(server, host, port)) return "服务器地址无效（形如 https://cloud.example.com）";
    CloudTransport cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Post("/api/auth/login",
                        json{{"username", user}, {"password", pass}}.dump(),
                        "application/json");
    if (!res) return "无法连接服务器";
    json j = json::parse(res->body, nullptr, false);
    if (!j.is_object()) return "服务器返回格式错误";
    if (res->status != 200)
        return j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "登录失败";
    if (!j.contains("token") || !j["token"].is_string() || j["token"].get<std::string>().size()!=64) return "服务器返回的登录凭据无效";
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
        CloudTransport cli(host, port);
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

bool CloudClient::loggedIn() const {std::lock_guard<std::mutex> lk(mtx_); return !account_.token.empty();}

std::string CloudClient::username() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return account_.username;
}

void CloudClient::setAccount(const std::string& server, const std::string& token,
                             const std::string& username) {
    std::lock_guard<std::mutex> lk(mtx_);
    account_ = {server.empty() ? DUTYON_CLOUD_URL : server, token, username};
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

void CloudClient::syncNow() {
    // 上一轮发现设置两边都改过：用户再点「立即同步」表示以本机为准
    if (config_conflict_) force_push_ = true;
    sync_requested_ = true;
}
void CloudClient::requestRestore() { restore_requested_ = true; }
void CloudClient::checkUpdate() { update_check_requested_ = true; }
void CloudClient::startUpdateDownload() { update_download_requested_ = true; }

void CloudClient::workerLoop() {
    auto last_upload = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    while (!stop_) {
        // 1s 节拍：处理一次性请求 + 周期上传
        for (int i = 0; i < 10 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        try {
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
        } catch (const std::exception&) {
            setSyncHint("云端操作失败：响应或本地数据无效，请重试");
            setUpdateHint("云端操作失败，请重试");
            std::lock_guard<std::mutex> lk(mtx_); update_state_=UpdateState::kFailed;
        }
    }
}

// ---- 增量上传 ----

// 一轮同步：本机与云端双向对账（文件 + 设置）。
//
// 网页云空间可以直接增删改角色，所以不能再"本机永远覆盖云端"：
//   设置   三方比较（上次同步时的本机/云端 vs 现在的本机/云端）：
//     只有本机改过 → 上传；只有云端改过 → 取回并应用；两边都改过 →
//     不覆盖任何一边，提示用户选"从云端恢复"或"立即同步"。
//   文件   本机有、云端没有：以前同步过且现在没被引用 → 是在网页上删的，本机
//     那一份移进 ~/.dutyon/cloud-trash；否则是新文件，上传。
//   云端有、本机没有：以前同步过且现在没被引用 → 是在本机删的，云端
//     也删；否则是网页新传的（或被引用却丢了），下载。
//   两边内容不同：沿用原规则（云端在别处变过则报冲突，否则上传本机版）。
// "被引用" = 当前设置里某个角色的动画或声音正用着这个文件，这类文件不删。
bool CloudClient::doUploadOnce(std::string& err) {
    std::string host, token;
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!parseServerUrl(account_.server, host, port) || account_.token.empty()) {
            err = "未登录";
            return false;
        }
        token = account_.token;
    }
    CloudTransport cli(host, port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(60);
    httplib::Headers auth = {{"Authorization", "Bearer " + token}};

    const std::string home = homeDir();
    const std::string anim_dir = UserConfigStore::animationsDir();
    const std::string l2d_dir = UserConfigStore::userModelsDir();
    const bool force_push = force_push_.exchange(false);

    // 本机缓存：文件摘要（size+mtime 命中免算）、各文件上次同步时的摘要、
    // 设置上次同步时的本机摘要与云端版本号
    const std::string cache_path = home + "/.dutyon/cloud-cache.json";
    json cache = readJsonFile(cache_path);
    if (!cache.contains("files") || !cache["files"].is_object()) cache["files"] = json::object();
    json& cache_files = cache["files"];
    std::string accountKey;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        accountKey = account_.server + "/" + account_.username;
    }
    for (const char* section : {"synced", "config"}) {
        if (!cache.contains(section) || !cache[section].is_object())
            cache[section] = json::object();
        if (!cache[section].contains(accountKey) || !cache[section][accountKey].is_object())
            cache[section][accountKey] = json::object();
    }
    json& synced = cache["synced"][accountKey];
    json& cfg_base = cache["config"][accountKey];

    // ---- 云端现状：文件清单 + 设置 ----
    auto res = cli.Get("/api/sync/manifest", auth);
    if (!res || res->status != 200) {
        err = res ? ("清单获取失败 (" + std::to_string(res->status) + ")") : "无法连接服务器";
        return false;
    }
    const json remote = json::parse(res->body, nullptr, false);
    if (!remote.is_array()) {
        err = "云端清单格式错误";
        return false;
    }
    struct RemoteFile {
        std::string sha;
        long long size;
    };
    std::map<std::string, RemoteFile> remote_map;
    for (const auto& e : remote) {
        if (!e.is_object() || !e.contains("path") || !e.contains("sha256")) continue;
        const long long size =
            e.contains("size") && e["size"].is_number_integer() ? e["size"].get<long long>() : -1;
        remote_map[e["path"].get<std::string>()] = {e["sha256"].get<std::string>(), size};
    }
    auto cfg_res = cli.Get("/api/sync/config", auth);
    if (!cfg_res || cfg_res->status != 200) {
        err = "云端配置获取失败";
        return false;
    }
    const json cj = json::parse(cfg_res->body, nullptr, false);
    if (!cj.is_object() || !cj.contains("config") || !cj["config"].is_object()) {
        err = "云端配置格式错误";
        return false;
    }
    const json& remote_cfg = cj["config"];
    // 旧版服务端不返回版本号：退回"本机覆盖云端"的老行为
    const std::string remote_rev = cj.value("rev", std::string());

    // ---- 设置：决定方向 ----
    auto snapshotOf = [](const json& cfg) {
        json s = json::object();
        for (const char* key : cloudSyncFields())
            if (cfg.contains(key)) s[key] = cfg[key];
        return s;
    };
    auto digest = [](const json& j) {
        const std::string text = j.dump();
        return SHA256::toHex(SHA256::hash(text.data(), text.size()));
    };
    json local_cfg = readJsonFile(configPath());
    if (!local_cfg.is_object()) local_cfg = json::object();
    const std::string local_hash = digest(snapshotOf(local_cfg));
    const bool have_base = cfg_base.contains("rev") && cfg_base.contains("local");
    const bool remote_changed =
        have_base && !remote_rev.empty() && cfg_base.value("rev", std::string()) != remote_rev;
    const bool local_changed = !have_base || cfg_base.value("local", std::string()) != local_hash;
    enum class CfgAction { None, Push, Pull, Conflict };
    CfgAction action = CfgAction::None;
    if (remote_rev.empty() || force_push)
        action = CfgAction::Push;
    else if (remote_changed && local_changed)
        action = CfgAction::Conflict;
    else if (remote_changed)
        action = CfgAction::Pull;
    else if (local_changed)
        action = CfgAction::Push;

    bool apply_needed = false;  // 本机设置或文件被云端改动 → 通知主线程重载
    if (action == CfgAction::Pull) {
        for (const char* key : cloudSyncFields())
            if (remote_cfg.contains(key)) local_cfg[key] = remote_cfg[key];
        if (!writeJsonFile(configPath(), local_cfg)) {
            err = "配置保存失败";
            return false;
        }
        apply_needed = true;
    }

    // 当前设置引用到的文件（角色各状态的动画、各状态绑定的声音）
    std::set<std::string> referenced;
    if (local_cfg.contains("customCharacters") && local_cfg["customCharacters"].is_array())
        for (const auto& c : local_cfg["customCharacters"])
            for (const char* state : {"sleeping", "working", "alert"})
                if (c.is_object() && c.contains(state) && c[state].is_string() &&
                    !c[state].get<std::string>().empty())
                    referenced.insert("animations/" + c[state].get<std::string>());
    if (local_cfg.contains("stateAudio") && local_cfg["stateAudio"].is_object())
        for (const auto& per_key : local_cfg["stateAudio"])
            if (per_key.is_object())
                for (const auto& name : per_key)
                    if (name.is_string() && !name.get<std::string>().empty())
                        referenced.insert("animations/" + name.get<std::string>());
    const std::string active = local_cfg.value("activeCharacterId", std::string());
    auto inUse = [&](const std::string& rel) {
        if (referenced.count(rel)) return true;
        // Live2D：正在使用的模型整个文件夹都算被引用
        if (rel.rfind("live2d/", 0) == 0) {
            const size_t slash = rel.find('/', 7);
            const std::string model =
                rel.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
            return !model.empty() && active.find(model) != std::string::npos;
        }
        return false;
    };

    // ---- 本机文件清单 ----
    struct LocalFile {
        std::string rel;  // 相对 ~/.dutyon/（animations/x.gif）
        std::string abs;
        std::string sha256;
        long long size;
    };
    std::vector<LocalFile> locals;
    std::set<std::string> local_set;
    std::vector<std::string> rels;
    collectFiles(fs::u8path(anim_dir), "animations", rels);
    collectFiles(fs::u8path(l2d_dir), "live2d", rels);
    for (const auto& rel : rels) {
        if (!validCloudPath(rel) ||
            (rel.size() >= 15 && rel.substr(rel.size() - 15) == ".cloud-download"))
            continue;
        const fs::path full = fs::u8path(home) / ".dutyon" / fs::u8path(rel);
        std::error_code ec;
        if (!fs::is_regular_file(full, ec)) continue;
        const auto sz = (long long)fs::file_size(full, ec);
        if (ec) continue;
        const auto mt = fs::last_write_time(full, ec);
        const long long mtime = (long long)mt.time_since_epoch().count();
        std::string sha;
        const json& c = cache_files.contains(rel) ? cache_files[rel] : json();
        if (c.is_object() && c.value("s", (long long)-1) == sz &&
            c.value("m", (long long)-1) == mtime && c.contains("h"))
            sha = c.value("h", std::string());
        if (sha.empty()) {
            sha = SHA256::fileHex(full);
            if (sha.empty()) continue;  // 读失败跳过
            cache_files[rel] = {{"s", sz}, {"m", mtime}, {"h", sha}};
        }
        locals.push_back({rel, full.u8string(), sha, sz});
        local_set.insert(rel);
    }

    // 云端清单为空而本机记着同步过不少文件：更像是服务端数据出了问题，
    // 不把它当成"用户在网页上全删了"
    const bool remote_wiped = remote_map.empty() && synced.size() >= 3;
    auto wasSynced = [&](const std::string& rel, const std::string& sha) {
        return synced.contains(rel) && synced[rel].is_string() &&
               synced[rel].get<std::string>() == sha;
    };

    // ---- 本机有的文件 ----
    for (const auto& lf : locals) {
        auto it = remote_map.find(lf.rel);
        if (it != remote_map.end() && it->second.sha == lf.sha256) {
            synced[lf.rel] = lf.sha256;
            continue;
        }
        if (it == remote_map.end() && synced.contains(lf.rel) && !remote_wiped && !inUse(lf.rel)) {
            // 以前同步过、现在云端没有了：在网页上删的。本机那份移进回收目录
            std::string flat = lf.rel;
            std::replace(flat.begin(), flat.end(), '/', '_');
            const fs::path trash = fs::u8path(home) / ".dutyon" / "cloud-trash";
            std::error_code ec;
            fs::create_directories(trash, ec);
            fs::rename(fs::u8path(lf.abs), trash / fs::u8path(flat), ec);
            if (ec) {  // 目标已存在或跨盘：退而求其次直接覆盖
                fs::copy_file(fs::u8path(lf.abs), trash / fs::u8path(flat),
                              fs::copy_options::overwrite_existing, ec);
                fs::remove(fs::u8path(lf.abs), ec);
            }
            synced.erase(lf.rel);
            cache_files.erase(lf.rel);
            apply_needed = true;
            continue;
        }
        if (it != remote_map.end() && !wasSynced(lf.rel, it->second.sha)) {
            err = "云端文件已变化，请先从云端恢复：" + lf.rel;
            writeJsonFile(cache_path, cache);
            return false;
        }
        auto uploadAuth = auth;
        if (it == remote_map.end())
            uploadAuth.emplace("If-None-Match", "*");
        else
            uploadAuth.emplace("If-Match", "\"" + it->second.sha + "\"");
        if (lf.size > 50ll * 1024 * 1024) {
            err = "文件超过50MiB：" + lf.rel;
            return false;
        }
        std::ifstream in(fs::u8path(lf.abs), std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        auto up = cli.Put("/api/sync/file?path=" + urlEncode(lf.rel), uploadAuth, ss.str(),
                          "application/octet-stream");
        if (!up || up->status != 200) {
            err = "上传 " + lf.rel + " 失败";
            return false;
        }
        synced[lf.rel] = lf.sha256;
        writeJsonFile(cache_path, cache);
    }

    // ---- 云端有、本机没有的文件 ----
    for (const auto& [rel, rf] : remote_map) {
        if (local_set.count(rel) || !validCloudPath(rel)) continue;
        if (wasSynced(rel, rf.sha) && !inUse(rel)) {
            // 以前同步过、本机已经没有、也没被引用：在本机删的，云端跟着删
            auto del = cli.Delete("/api/sync/file?path=" + urlEncode(rel), auth);
            if (del && del->status == 200) synced.erase(rel);
            continue;
        }
        // 网页新传的文件（或被引用却丢了的文件）：下载
        if (!validDigest(rf.sha) || rf.size < 0 || rf.size > 50ll * 1024 * 1024) continue;
        const fs::path full = fs::u8path(home) / ".dutyon" / fs::u8path(rel);
        std::error_code ec;
        fs::create_directories(full.parent_path(), ec);
        auto temp = full;
        temp += L".cloud-download";
        long long received = 0;
        bool ok = false;
        {
            std::ofstream out(temp, std::ios::binary);
            if (!out) continue;
            auto fr = cli.Get("/api/sync/file?path=" + urlEncode(rel), auth,
                              [&](const char* data, size_t len) {
                                  received += (long long)len;
                                  if (received > rf.size) return false;
                                  out.write(data, (std::streamsize)len);
                                  return out.good();
                              });
            out.close();
            ok = fr && fr->status == 200 && received == rf.size && SHA256::fileHex(temp) == rf.sha;
        }
        if (!ok || !MoveFileExW(temp.c_str(), full.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fs::remove(temp, ec);
            err = "下载 " + rel + " 失败";
            writeJsonFile(cache_path, cache);
            return false;
        }
        synced[rel] = rf.sha;
        apply_needed = true;
    }

    // ---- 设置：执行 ----
    if (action == CfgAction::Push) {
        const json snapshot = snapshotOf(local_cfg);
        auto headers = auth;
        if (!remote_rev.empty())  // 强制以本机为准时也带版本号："覆盖我刚看到的这一版"
            headers.emplace("If-Match", "\"" + remote_rev + "\"");
        auto put = cli.Put("/api/sync/config", headers, snapshot.dump(), "application/json");
        if (put && put->status == 412) {
            err = "云端设置刚被修改，下一轮再同步";
            writeJsonFile(cache_path, cache);
            return false;
        }
        if (!put || put->status != 200) {
            err = "配置快照上传失败";
            return false;
        }
        const json pj = json::parse(put->body, nullptr, false);
        const std::string new_rev = pj.is_object() ? pj.value("rev", std::string()) : std::string();
        if (!new_rev.empty()) {
            cfg_base["rev"] = new_rev;
            cfg_base["local"] = digest(snapshot);
        }
    } else if (action == CfgAction::Pull || (action == CfgAction::None && !have_base)) {
        cfg_base["rev"] = remote_rev;
        cfg_base["local"] = digest(snapshotOf(local_cfg));
    }
    config_conflict_ = action == CfgAction::Conflict;
    writeJsonFile(cache_path, cache);
    if (apply_needed) restore_done_ = true;  // 主线程重载设置、重新载入当前角色
    if (action == CfgAction::Conflict) {
        err = "设置在云端和本机都改过。选「从云端恢复」用云端的，或再点一次「立即同步」用本机的";
        return false;
    }
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
    CloudTransport cli(host, port);
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
    json pendingConfig;
    if (cj.is_object() && cj.contains("config") &&
        cj["config"].is_object()) {
        const json& remote_cfg = cj["config"];
        json local_cfg = readJsonFile(configPath());
        for (const char* key : cloudSyncFields())
            if (remote_cfg.contains(key)) local_cfg[key] = remote_cfg[key];
        pendingConfig=std::move(local_cfg);
    } else {err="云端配置格式错误"; return false;}

    // 2. 文件：逐个比对 sha256，差异下载（相对路径原位落盘）
    auto res = cli.Get("/api/sync/manifest", auth);
    if (!res || res->status != 200) {
        err = "云端清单获取失败";
        return false;
    }
    json remote = json::parse(res->body, nullptr, false);
    const std::string home = homeDir();
    if (!remote.is_array()) {err="云端清单格式错误"; return false;}
    if (remote.is_array()) {
        for (const auto& e : remote) {
            if (!e.is_object() || !e.contains("path")) continue;
            const std::string rel = e["path"].get<std::string>();
            const std::string sha =
                e.value("sha256", std::string());
            if (!validCloudPath(rel) || !validDigest(sha) || !e.contains("size") ||
                !e["size"].is_number_integer()) {err="云端文件清单无效"; return false;}
            const auto expected=e["size"].get<long long>();
            if (expected<0 || expected>50ll*1024*1024) {err="云端文件大小无效"; return false;}
            const fs::path full = fs::u8path(home) / ".dutyon" / fs::u8path(rel);
            if (SHA256::fileHex(full) == sha) continue;
            std::error_code ec;
            fs::create_directories(full.parent_path(), ec);
            auto temp=full; temp+=L".cloud-download";
            std::ofstream out(temp, std::ios::binary);
            if (!out) {err="无法写入 " + rel; return false;}
            long long received=0;
            auto fr = cli.Get("/api/sync/file?path=" + urlEncode(rel), auth,
                              [&](const char* data, size_t len) {
                                  received+=(long long)len;
                                  if(received>expected) return false;
                                  out.write(data, (std::streamsize)len);
                                  return out.good();
                              });
            out.close();
            if (!fr || fr->status != 200 || received!=expected || SHA256::fileHex(temp)!=sha) {
                fs::remove(temp, ec);
                err = "下载或校验 " + rel + " 失败"; return false;
            }
            if (!MoveFileExW(temp.c_str(),full.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
                fs::remove(temp,ec); err="替换 " + rel + " 失败"; return false;
            }
            ++restored_files;
        }
    }

    if (!writeJsonFile(configPath(),pendingConfig)) {err="配置保存失败"; return false;}

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
    CloudTransport cli(host, port);
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
        setUpdateHint(std::string("当前 ") + DUTYON_VERSION + "（暂无已发布更新）");
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
    if(!j.is_object()) throw std::runtime_error("invalid release");
    UpdateInfo info;
    info.version = j.value("version", "");
    info.notes = j.value("notes", "");
    info.size = j.value("size", (long long)0);
    info.sha256 = j.value("sha256", "");
    if (!std::regex_match(info.version,std::regex(R"(\d+(\.\d+){1,3})")) || !validDigest(info.sha256) || info.size<=0 || info.size>1024ll*1024*1024) throw std::runtime_error("invalid release metadata");
    if (compareVersion(info.version, DUTYON_VERSION) <= 0) {
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
    CloudTransport cli(host, port);
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
        "/api/app/download?sha256=" + info.sha256,
        [&](const char* data, size_t len) {
            if (received+(long long)len>total) return false;
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
    if (received!=total || SHA256::fileHex(zip_path) != info.sha256) {
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
    ps += psQuote(zip_path);
    ps += L"' -DestinationPath '";
    ps += psQuote(staged);
    ps += L"' -Force\"";
    setUpdateHint("解压中…");
    if (!runHiddenAndWait(ps) || !fs::is_regular_file(fs::path(staged)/"dutyon-pet.exe")) {
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
        << "if errorlevel 1 exit /b 1\r\n"
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
