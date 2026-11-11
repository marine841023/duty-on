#ifndef _WIN32  // 仅设备端（ARM Linux）

#include "net/wifi_manager.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/device_identity.h"
#include "net/wifi_credentials.h"
#include "net/wifi_scan.h"
#include "platform/atomic_file.h"

namespace dutyon {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---- 常量 ----
const char* kApIp = "192.168.4.1";        // AP 模式设备自身地址（portal 网关）
const long long kJoinTimeoutMs = 65000;   // 含锁等待、接口初始化和最多 28s DHCP
const int kMaxJoinAttempts = 2;           // 连续失败几次后回退 AP 重新配网
const char* kWlan = "wlan0";

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 射频编排脚本目录（随部署包放到 /opt/dutyon；测试可用 DUTYON_WIFI_DIR 覆盖）
std::string scriptDir() {
    const char* e = getenv("DUTYON_WIFI_DIR");
    return (e && *e) ? std::string(e) : std::string("/opt/dutyon");
}

// 运行期配置目录（tmpfs，掉电即清；hostapd/dnsmasq/wpa_supplicant 配置写这里）
std::string runDir() {
    const char* e = getenv("DUTYON_WIFI_RUN_DIR");
    return (e && *e) ? std::string(e) : std::string("/run/dutyon");
}

bool daemonAlive(const char* name) {
    std::ifstream in(runDir() + "/" + name + ".pid");
    long pid = 0;
    if (!(in >> pid) || pid <= 1 || pid > std::numeric_limits<pid_t>::max()) return false;
    std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
    std::string actual;
    if (!std::getline(comm, actual) || actual != name) return false;
    std::ifstream args("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    const std::string config = runDir() + "/" + name + ".conf";
    bool owned = false;
    while (std::getline(args, actual, '\0'))
        if (actual == config || actual == "--conf-file=" + config) owned = true;
    if (!owned) return false;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

std::string wifiJsonPath() {
    const char* home = getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path("/root");
    return (base / ".dutyon" / "wifi.json").string();
}

bool pathExists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// 后台起脚本：system() 里的 sh 立即返回（尾部 &），setsid 让脚本脱离会话，
// poll() 不阻塞——脚本内部起 hostapd/wpa_supplicant 守护进程，连接与否由
// 后续帧轮询 wlan0 是否有 IP 判定。参数只有固定脚本名 + iface（无用户输入）。
std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

void spawnDetached(const std::string& script_and_args, bool quiet = false) {
    const auto split = script_and_args.find(' ');
    std::string cmd = "setsid " + shellQuote(scriptDir() + "/" + script_and_args.substr(0, split));
    if (split != std::string::npos) cmd += " " + shellQuote(script_and_args.substr(split + 1));
    cmd += " >>" + shellQuote(runDir() + "/wifi.log") + " 2>&1 &";
    if (!quiet) printf("[Wifi] $ %s/%s\n", scriptDir().c_str(), script_and_args.c_str());
    // system() 带 warn_unused_result：GCC 下 (void) 转型无法消警，用分支消费返回值
    //（后台脚本的返回码无意义，setsid + 尾部 & 已立即返回）
    if (system(cmd.c_str()) != 0) { /* ignore */ }
}

// wpa_supplicant.conf 里 ssid/psk 用双引号包裹，需转义双引号与反斜杠
std::string wpaEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

bool writeFile(const std::string& path, const std::string& content) {
    if (!atomicWritePrivate(path, content)) {
        fprintf(stderr, "[Wifi] write failed: %s\n", path.c_str());
        return false;
    }
    return true;
}

// HTML 转义（SSID 可能含 & < > " 等，直接拼进属性/文本会破坏页面）
std::string htmlEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

// 读 wifi-ap.sh 在起 AP 前扫到的周边热点（一行一个 SSID，信号强->弱）；
// 空 = 未扫到（portal 仅显示手动输入框）。浏览器无法读取手机当前 Wi-Fi，
// 故改由设备扫描邻近网络供点选，免手输 SSID。
std::vector<std::string> readScanList() {
    std::ifstream in(runDir() + "/scan.txt");
    return parseWifiScanList(in);
}

// captive portal 探测路径识别：手机/系统连网检测请求（Android generate_204、
// Windows NCSI/connecttest、iOS hotspot-detect、Firefox canonical 等）。
// 对这些路径回 302 重定向到配网页，系统会自动弹出配网窗口（ESP 类配网的
// 标准做法，比返回 200 HTML 的弹窗触发率高得多）。
bool isProbePath(const std::string& path) {
    if (path == "/ncsi.txt" || path == "/connecttest.txt" ||
        path == "/redirect" || path == "/hotspot-detect.html" ||
        path == "/library/test/success.html" || path == "/canonical.html" ||
        path == "/success.txt" || path == "/wifi")
        return true;
    // Android 系（含小米/华为/OPPO 等厂商变体）：路径以 generate_204 结尾
    const char kSuffix[] = "/generate_204";
    const size_t sl = sizeof(kSuffix) - 1;
    return path.size() >= sl &&
           path.compare(path.size() - sl, sl, kSuffix) == 0;
}

// ---- captive portal 页面 ----
// 移动优先的极简内联样式；深色卡片。表单 POST /configure 提交 ssid/pass。
std::string portalPage(const std::string& ap_ssid,
                       const std::vector<std::string>& nets,
                       const std::string& failure) {
    std::ostringstream h;
    h << R"HTML(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">)HTML"
      << R"HTML(<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">)HTML"
      << "<title>DutyOn 配网</title><style>"
      << R"CSS(*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;background:#0f1115;color:#e8eaed;padding:20px}
.card{max-width:420px;margin:7vh auto;background:#1a1d24;border-radius:16px;padding:28px 24px;box-shadow:0 8px 32px rgba(0,0,0,.45)}
h1{font-size:20px;margin-bottom:6px}
.sub{font-size:13px;color:#9aa0a6;margin-bottom:20px;line-height:1.6}
.error{color:#ffb4ab;font-size:14px;line-height:1.6;margin-bottom:16px}
.tag{display:inline-block;font-size:12px;color:#8ab4ff;background:#1c2740;border-radius:6px;padding:2px 8px;margin-bottom:14px}
label{display:block;font-size:13px;color:#bdc1c6;margin:14px 0 6px}
input{width:100%;padding:12px 14px;font-size:16px;border-radius:10px;border:1px solid #33383f;background:#12151a;color:#fff}
input:focus{outline:none;border-color:#4c8dff}
.nets{display:flex;flex-direction:column;gap:8px;margin-top:8px}
.net{display:flex;align-items:center;justify-content:space-between;gap:12px;width:100%;min-width:0;padding:13px 14px;font-size:15px;border-radius:10px;border:1px solid #33383f;background:#12151a;color:#e8eaed;text-align:left}
.net .name,#sel-name{min-width:0;overflow-wrap:anywhere;word-break:break-word;white-space:pre-wrap}
.net.on{border-color:#4c8dff;color:#8ab4ff;background:#16203a}
.net .arr{flex-shrink:0;color:#5f6368;font-size:15px}
.net.on .arr{color:#8ab4ff}
.btn{width:100%;margin-top:20px;padding:14px;font-size:16px;font-weight:600;border:none;border-radius:10px;background:#4c8dff;color:#fff}
.btn:active{background:#3a76e0}
.btn.subtle{background:#2a2f37;color:#8ab4ff;border:1px solid #3a4150}
.selssid{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 14px;border-radius:10px;border:1px solid #4c8dff;background:#16203a;color:#8ab4ff;font-size:15px}
.selssid .re{flex-shrink:0;font-size:13px;color:#9aa0a6;text-decoration:underline;padding:4px 6px}
.hidden{display:none}
.ok{text-align:center;padding:14px 0}
.ok .big{font-size:46px;line-height:1;margin-bottom:16px}
.dl{margin-top:26px;padding-top:20px;border-top:1px solid #2a2f37}
.dl h2{font-size:16px;margin-bottom:8px;color:#e8eaed}
.steps{margin:10px 0 16px 20px;font-size:13px;color:#9aa0a6;line-height:1.9;overflow-wrap:anywhere}
.steps b{color:#8ab4ff}
.dlbtn{display:block;width:100%;margin-top:10px;padding:13px;font-size:15px;font-weight:600;text-align:center;text-decoration:none;border-radius:10px;background:#4c8dff;color:#fff}
.dlbtn.alt{background:#2a2f37;color:#8ab4ff;border:1px solid #3a4150}
.urls{margin-top:14px;font-size:12px;color:#6b7280;line-height:1.7;word-break:break-all;text-align:center}
)CSS"
      << "</style></head><body><div class=\"card\">";
    // 成功页由 POST 处理器直接返回；此处为表单页
    // 两步式表单：① 选网络（扫描列表点选 / 手动输入）→ ② 密码 + 连接。
    // 旧的"SSID 输入框 + 密码框同屏"信息过载，且移动端软键盘易误触。
    // 隐藏网络/未扫到时走"手动输入其他网络"分支（仍免打 SSID 之外的任何字）。
    if (!failure.empty())
        h << "<p class=\"error\" role=\"alert\">" << htmlEsc(failure)
          << "。请重新选择网络并输入密码。</p>";
    h << "<h1>DutyOn 设备配网</h1>"
      << "<div class=\"tag\">热点 " << htmlEsc(ap_ssid) << "</div>"
      << "<p class=\"sub\">选择 Wi-Fi 并输入密码，提交后设备自动联网，"
         "屏幕随后显示配对码，请在电脑端 DutyOn 完成配对。</p>"
      << "<form method=\"post\" action=\"/configure\">"
      << "<input type=\"hidden\" name=\"ssid\" id=\"f-ssid\" required>"
      // ---- 第①步：选择网络 ----
      << "<div id=\"step1\">";
    if (!nets.empty()) {
        h << "<label>选择 Wi-Fi 网络</label><div class=\"nets\">";
        for (const auto& s : nets)
            h << "<button type=\"button\" class=\"net\" data-s=\""
              << htmlEsc(s) << "\"><span class=\"name\">" << htmlEsc(s)
              << "</span><span class=\"arr\">›</span></button>";
        h << "</div>";
    }
    h << "<label>" << (nets.empty() ? "输入 Wi-Fi 名称" : "或手动输入名称")
      << "</label>"
      << "<input id=\"manual-ssid\" type=\"text\" autocomplete=\"off\" "
         "autocapitalize=\"off\" spellcheck=\"false\" placeholder=\"Wi-Fi 名称\">"
      << "<button type=\"button\" class=\"btn subtle\" id=\"use-manual\""
      << (nets.empty() ? "" : " style=\"margin-top:12px\"") << ">下一步</button>"
      << "</div>"
      // ---- 第②步：密码 + 连接 ----
      << "<div id=\"step2\" class=\"hidden\">"
      << "<label>已选择网络</label>"
      << "<div class=\"selssid\"><span id=\"sel-name\"></span>"
         "<a class=\"re\" href=\"javascript:void(0)\" id=\"reselect\">重新选择</a></div>"
      << "<label>Wi-Fi 密码</label>"
      << "<input name=\"pass\" type=\"password\" autocomplete=\"off\" "
         "placeholder=\"留空表示开放网络\">"
      << "<button type=\"submit\" class=\"btn\">连接</button>"
      << "</div>"
      << "</form>"
      << "<script>"
         "function toStep2(s){document.getElementById('f-ssid').value=s;"
         "document.getElementById('sel-name').textContent=s;"
         "document.getElementById('step1').classList.add('hidden');"
         "document.getElementById('step2').classList.remove('hidden');}"
         "document.querySelectorAll('.net').forEach(function(b){"
         "b.onclick=function(){toStep2(b.getAttribute('data-s'));};});"
         "document.getElementById('use-manual').onclick=function(){"
         "var v=document.getElementById('manual-ssid').value;"
         "if(v)toStep2(v);};"
         "document.getElementById('reselect').onclick=function(){"
         "document.getElementById('step2').classList.add('hidden');"
         "document.getElementById('step1').classList.remove('hidden');"
         "document.getElementById('f-ssid').value='';};"
         "</script>"
      // 电脑端软件下载指引：配网时手机在设备热点上无外网，链接可能打不开，
      // 故同时给出纯文本网址，方便回到有网的电脑上直接输入访问。
      << "<div class=\"dl\">"
      << "<h2>还没有电脑端 DutyOn？</h2>"
      << "<p class=\"sub\">配对需要在电脑上运行 DutyOn。按以下步骤免费下载安装（链接请在有网络的电脑浏览器打开）：</p>"
      << "<ol class=\"steps\">"
      << "<li>在电脑浏览器打开 <b>gitee.com/megrezsoft/duty-on</b>（国内访问快）<br>或 <b>github.com/marine841023/duty-on</b></li>"
      << "<li>进入仓库的 <b>Releases</b> 页面</li>"
      << "<li>下载最新版本的 <b>DutyOn-vX.X.X.zip</b> 压缩包</li>"
      << "<li>解压后双击运行安装程序，按提示完成安装</li>"
      << "<li>打开电脑端 DutyOn，右键宠物头像 → <b>设备</b> → <b>配对设备</b>，输入设备屏幕上显示的 <b>配对码</b></li>"
      << "</ol>"
      << "<a class=\"dlbtn\" href=\"https://gitee.com/megrezsoft/duty-on/releases\" target=\"_blank\" rel=\"noopener\">Gitee 下载（国内更快）</a>"
      << "<a class=\"dlbtn alt\" href=\"https://github.com/marine841023/duty-on/releases\" target=\"_blank\" rel=\"noopener\">GitHub 下载（Releases）</a>"
      << "<p class=\"urls\">gitee.com/megrezsoft/duty-on<br>"
         "github.com/marine841023/duty-on</p>"
      << "</div>"
      << "</div></body></html>";
    return h.str();
}

std::string portalSuccess(const std::string& ssid) {
    std::ostringstream h;
    h << R"HTML(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">)HTML"
      << R"HTML(<meta name="viewport" content="width=device-width,initial-scale=1">)HTML"
      << "<title>DutyOn 配网</title><style>"
      << R"CSS(*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;background:#0f1115;color:#e8eaed;padding:20px}
.card{max-width:420px;margin:12vh auto;background:#1a1d24;border-radius:16px;padding:34px 24px;text-align:center;box-shadow:0 8px 32px rgba(0,0,0,.45)}
.big{font-size:52px;line-height:1;margin-bottom:16px}
h1{font-size:19px;margin-bottom:10px}
p{font-size:14px;color:#9aa0a6;line-height:1.7}
b{color:#e8eaed;overflow-wrap:anywhere;word-break:break-word;white-space:pre-wrap}
)CSS"
      << "</style></head><body><div class=\"card\">"
      << "<div class=\"big\">&#10003;</div>"
      << "<h1>已收到 Wi-Fi 信息</h1>"
      << "<p>设备正在连接 <b>" << htmlEsc(ssid)
      << "</b>……<br>请查看设备屏幕的连接进度，联网后会显示配对码。"
         "<br>认证失败或约两分钟仍未连上时，会恢复设备热点。"
         "请重新连接热点，打开 192.168.4.1 核对网络和密码。</p>"
      << "</div></body></html>";
    return h.str();
}

} // namespace

const char* wifiStateStr(WifiState s) {
    switch (s) {
    case WifiState::Off: return "off";
    case WifiState::ApProvisioning: return "ap-provisioning";
    case WifiState::Joining: return "joining";
    case WifiState::Online: return "online";
    case WifiState::Failed: return "failed";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// captive portal：httplib::Server 监听 80，独立线程（listen 阻塞）。
// GET 任意路径返回配置表单（兼容 iOS/Android 的 portal 探测 URL——返回非预期
// 成功响应即触发系统弹出配网页）；POST /configure 解析 ssid/pass 后调
// saveAndJoin（仅置标志，实际射频切换在主循环 poll() 里做）。
// ---------------------------------------------------------------------------
struct WifiManager::PortalImpl {
    httplib::Server svr;
    std::thread th;
    WifiManager* owner = nullptr;

    bool start(WifiManager* o) {
        owner = o;
        svr.set_payload_max_length(4096);
        svr.set_read_timeout(3, 0);
        svr.set_write_timeout(3, 0);
        svr.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
            // 系统连网探测 -> 302 到配网页触发自动弹窗；其余 GET 直接配网页
            if (isProbePath(req.path)) {
                res.status = 302;
                res.set_header("Location", owner->portalUrl() + "/");
                return;
            }
            res.set_content(portalPage(owner->apSsid(), readScanList(), owner->joinFailure()), "text/html; charset=utf-8");
        });
        svr.Post("/configure", [this](const httplib::Request& req, httplib::Response& res) {
            std::string ssid = req.has_param("ssid") ? req.get_param_value("ssid") : "";
            std::string pass = req.has_param("pass") ? req.get_param_value("pass") : "";
            if (!validWifiCredentials(ssid, pass)) {
                res.status = 400;
                res.set_content("Wi-Fi 名称须为 1–32 字节；密码须为 8–63 位 ASCII、64 位十六进制，或留空。", "text/plain; charset=utf-8");
                return;
            }
            if (!owner->saveAndJoin(ssid, pass)) {
                res.status = 503;
                res.set_content("无法保存 Wi-Fi 信息，请稍后重试。", "text/plain; charset=utf-8");
                return;
            }
            res.set_content(portalSuccess(ssid), "text/html; charset=utf-8");
        });

        // 同步绑定：失败时不能声称已启动，也不遗留尚未 listen 的线程。
        if (!svr.bind_to_port("0.0.0.0", 80)) {
            fprintf(stderr, "[Portal] bind :80 failed\n");
            return false;
        }
        th = std::thread([this]() { svr.listen_after_bind(); });
        svr.wait_until_ready();
        return svr.is_running();
    }

    void stop() {
        svr.stop();
        if (th.joinable()) th.join();
    }
};

// ---------------------------------------------------------------------------
// WifiManager
// ---------------------------------------------------------------------------
WifiManager::WifiManager() = default;

WifiManager::~WifiManager() { stop(); }

bool WifiManager::start() {
    if (run_.load()) return true;

    // 驱动可能晚于显示服务就绪；脚本等待接口，poll 继续重试，不能永久 Off。
    wlan_ = kWlan;

    // 射频编排脚本缺失只告警不阻断：状态机照常跑，起不来 AP 时日志可查
    for (const char* s : {"wifi-ap.sh", "wifi-client.sh", "wifi-off.sh", "wifi-common.sh"}) {
        if (!pathExists(scriptDir() + "/" + s))
            fprintf(stderr, "[Wifi] missing script %s/%s\n", scriptDir().c_str(), s);
    }

    std::error_code ec;
    fs::create_directories(runDir(), ec);

    // AP 标识：SSID 由 device_id 派生（跨重启稳定，QR 不变）；口令用持久化的
    //   8 位随机数字（WPA2 最短 8 位、手机数字键盘好输，见 DeviceIdentity）
    auto& id = DeviceIdentity::instance();
    ap_ssid_ = "DutyOn-" + id.shortSuffix();
    ap_pass_ = id.apPass();

    run_ = true;
    loadCreds();

    if (has_creds_) {
        printf("[Wifi] saved creds found (ssid=\"%s\"), joining\n", home_ssid_.c_str());
        enterClient();
    } else {
        printf("[Wifi] no saved creds, entering AP provisioning (%s)\n", ap_ssid_.c_str());
        enterAp();
    }
    return true;
}

void WifiManager::stop() {
    if (!run_.exchange(false)) {
        stopPortal();  // 幂等：即便未 start 也清干净
        return;
    }
    stopPortal();
    wifi_events_.reset();
    if (writeFile(runDir() + "/desired-mode", "off\n"))
        spawnDetached("wifi-off.sh " + wlan_);
    setState(WifiState::Off);
}

std::string WifiManager::clientIp() const { return cached_ip_; }

std::string WifiManager::joinProgress() const {
    const auto remaining = std::max(0LL, (kJoinTimeoutMs - (nowMs() - state_since_ms_) + 999) / 1000);
    return "第 " + std::to_string(join_attempts_ + 1) + "/" + std::to_string(kMaxJoinAttempts) +
           " 次尝试 · 剩余 " + std::to_string(remaining) + " 秒";
}

std::string WifiManager::joinFailure() const {
    switch (join_failure_.load()) {
    case 1: return "Wi-Fi 认证失败，请核对密码";
    case 2: return "Wi-Fi 连接超时，请检查网络和密码";
    default: return "";
    }
}

bool WifiManager::saveAndJoin(const std::string& ssid, const std::string& pass) {
    if (!validWifiCredentials(ssid, pass)) return false;
    {
        std::lock_guard<std::mutex> lk(cred_mu_);
        if (!run_.load() || state_.load() != WifiState::ApProvisioning) return false;
        json j = {{"ssid", ssid}, {"pass", pass}};
        if (!writeFile(wifiJsonPath(), j.dump(2))) return false;
        home_ssid_ = ssid;
        home_pass_ = pass;
        has_creds_ = true;
    }
    pending_join_ = true;  // poll() 里切 client（避免在 portal 线程动射频）
    return true;
}

void WifiManager::poll() {
    if (!run_.load()) return;
    const long long now = nowMs();
    if (now - last_poll_ms_ < 1000) return;  // 1s 节流（主循环 30fps 每帧调用）
    last_poll_ms_ = now;

    // portal 提交了凭据 -> 切 client
    if (pending_join_.exchange(false)) {
        join_attempts_ = 0;
        join_failure_ = 0;
        enterClient();
        return;
    }

    switch (state_.load()) {
    case WifiState::Joining:
    {
        const bool wrong_key = wifi_events_.pollWrongKey("/run/wpa_supplicant/" + wlan_);
        if (clientConnected()) {
            join_failure_ = 0;
            setState(WifiState::Online);
        } else if (wrong_key) {
            // An explicit authentication failure needs corrected credentials,
            // rather than two full rounds of waiting without feedback.
            join_failure_ = 1;
            printf("[Wifi] authentication failed (WRONG_KEY), fall back to AP\n");
            join_attempts_ = 0;
            enterAp();
        } else if (now - state_since_ms_ > kJoinTimeoutMs) {
            if (++join_attempts_ >= kMaxJoinAttempts) {
                join_failure_ = 2;
                printf("[Wifi] join failed x%d, fall back to AP\n", join_attempts_);
                join_attempts_ = 0;
                enterAp();
            } else {
                printf("[Wifi] join timeout, retry (%d/%d)\n", join_attempts_,
                       kMaxJoinAttempts);
                enterClient();
            }
        }
        break;
    }

    case WifiState::Online: {
        // Drain the subscription during normal use too; keep the control socket
        // queue bounded and reconnect if wpa_supplicant has been replaced.
        const bool wrong_key = wifi_events_.pollWrongKey("/run/wpa_supplicant/" + wlan_);
        // wpa_supplicant 会自动重连；IP 短暂丢失回 Joining 等待，超时再回退 AP
        if (!clientConnected()) {
            printf("[Wifi] link lost, re-joining\n");
            if (wrong_key) {
                join_failure_ = 1;
                enterAp();
            } else {
                setState(WifiState::Joining);
            }
        }
        break;
    }

    case WifiState::ApProvisioning:
        // 脚本最多等接口 10s + 扫描 8s，宽限后再检查。进程失败只修复
        // Wi-Fi，不把普通配置/端口错误推断为驱动崩溃而重启整台设备。
        if (now - state_since_ms_ > 25000 &&
            now - last_ap_check_ms_ >= 5000) {
            last_ap_check_ms_ = now;
            if (!hostapdAlive()) {
                ++ap_revives_;
                printf("[Wifi] hostapd not alive, restarting AP "
                       "(attempt %d)\n", ap_revives_);
                spawnDetached("wifi-ap.sh " + wlan_, true);
                // 重跑脚本全程 ~8s（含扫描/hostapd/dnsmasq 起停），期间 pid
                // 文件会被脚本删除重建——把检查点推后 15s 防止重复触发
                last_ap_check_ms_ = now + 15000;
            } else {
                ap_revives_ = 0;
                // 热点进程存活不代表网关/DHCP 正常。幂等脚本只修复缺失部分，
                // 保留已有 hostapd 和手机关联，同时清除 client 残留。
                spawnDetached("wifi-ap.sh " + wlan_, true);
            }
            if (!portal_ || !portal_->svr.is_running()) {
                stopPortal();
                startPortal();
            }
        }
        // 广播仅作辅助保活；网关/DHCP/DNS 的健康由上面的幂等修复负责。
        if (now - last_arp_keepalive_ms_ >= 5000) {
            last_arp_keepalive_ms_ = now;
            spawnDetached("arp-keepalive.sh " + wlan_, true);
        }
        break;

    default:
        break;
    }
}

void WifiManager::setState(WifiState s) {
    if (state_.load() == s) return;
    state_ = s;
    state_since_ms_ = nowMs();
    printf("[Wifi] state -> %s%s%s\n", wifiStateStr(s),
           s == WifiState::Online ? " ip=" : "",
           s == WifiState::Online ? cached_ip_.c_str() : "");
}

bool WifiManager::clientConnected() {
    if (!daemonAlive("wpa_supplicant") || hostapdAlive()) {
        cached_ip_.clear();
        return false;
    }
    // DHCP 租约仍在时，断开的链路也不能算 Online。
    std::ifstream carrier("/sys/class/net/" + wlan_ + "/carrier");
    int link = 0;
    if (!(carrier >> link) || link != 1) { cached_ip_.clear(); return false; }
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return false;
    std::string ip;
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (wlan_ != p->ifa_name) continue;
        char buf[INET_ADDRSTRLEN] = {};
        auto* sa = reinterpret_cast<struct sockaddr_in*>(p->ifa_addr);
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
        const std::string candidate = buf;
        if (candidate == kApIp ||
            candidate.rfind("169.254.", 0) == 0 ||
            candidate.rfind("127.", 0) == 0 || candidate == "0.0.0.0") continue;
        ip = candidate;
        break;
    }
    freeifaddrs(ifa);

    // 家庭网络可以使用 192.168.4.0/24，不能排除整个网段。
    if (ip.empty()) {
        cached_ip_.clear();
        return false;
    }
    cached_ip_ = ip;
    return true;
}

void WifiManager::loadCreds() {
    std::lock_guard<std::mutex> lk(cred_mu_);
    has_creds_ = false;
    home_ssid_.clear();
    home_pass_.clear();
    const std::string path = wifiJsonPath();
    if (!pathExists(path)) return;
    if (!restrictPrivateFile(path))
        fprintf(stderr, "[Wifi] cannot restrict config permissions: %s\n", path.c_str());
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return;
    if (!j.contains("ssid") || !j["ssid"].is_string() ||
        (j.contains("pass") && !j["pass"].is_string())) return;
    const auto ssid = j["ssid"].get<std::string>();
    const auto pass = j.value("pass", std::string{});
    if (!validWifiCredentials(ssid, pass)) return;
    home_ssid_ = ssid;
    home_pass_ = pass;
    has_creds_ = true;
}

void WifiManager::enterAp() {
    wifi_events_.reset();
    stopPortal();  // 若从 client 回退，先停旧 portal（下面重起）
    // 先停 client 侧守护进程（脚本内部会 kill wpa_supplicant/dhcpcd）
    // hostapd.conf：WPA2-PSK，SSID/口令由 device_id 派生。country_code：
    // 无国家码时部分驱动按最低功率发射、部分手机兼容差（搜不到/连不上）；
    // wmm：802.11n 依赖 WMM，部分手机无 WMM 不关联
    std::ostringstream h;
    h << "interface=" << wlan_ << "\n"
      << "driver=nl80211\n"
      << "ssid=" << ap_ssid_ << "\n"
      << "hw_mode=g\n"
      << "channel=6\n"  // 固定已验证的 2.4GHz 信道，避免驱动/客户端换信道兼容问题
      << "country_code=CN\n"
      << "ieee80211d=1\n"
      << "ieee80211n=1\n"
      << "wmm_enabled=1\n"
      << "wpa=2\n"
      << "wpa_passphrase=" << ap_pass_ << "\n"
      << "wpa_key_mgmt=WPA-PSK\n"
      << "rsn_pairwise=CCMP\n"
      << "ignore_broadcast_ssid=0\n";
    if (!writeFile(runDir() + "/hostapd.conf", h.str())) return;

    // dnsmasq.conf：给客户端派地址 + 把任意域名解析到设备 IP（captive portal
    // 探测的关键——所有 DNS 都指向 192.168.4.1，手机才会弹配网页）
    std::ostringstream d;
    d << "interface=" << wlan_ << "\n"
      << "bind-interfaces\n"
      << "dhcp-authoritative\n"
      << "no-resolv\n"
      << "dhcp-range=192.168.4.100,192.168.4.200,255.255.255.0,12h\n"
      << "dhcp-option=3," << kApIp << "\n"
      << "dhcp-option=6," << kApIp << "\n"
      << "address=/#/" << kApIp << "\n";
    if (!writeFile(runDir() + "/dnsmasq.conf", d.str())) return;

    if (!writeFile(runDir() + "/desired-mode", "ap\n")) return;
    spawnDetached("wifi-ap.sh " + wlan_);
    ap_revives_ = 0;
    last_ap_check_ms_ = 0;
    startPortal();
    setState(WifiState::ApProvisioning);
}

void WifiManager::enterClient() {
    wifi_events_.reset();
    stopPortal();  // 释放 80 端口 + 停 portal 线程
    std::string ssid, pass;
    {
        std::lock_guard<std::mutex> lk(cred_mu_);
        ssid = home_ssid_;
        pass = home_pass_;
    }
    // wpa_supplicant.conf：有口令走 WPA-PSK，空口令走开放网络
    std::ostringstream w;
    w << "ctrl_interface=/run/wpa_supplicant\n"
      << "update_config=1\n"
      << "network={\n"
      << "    ssid=" << wifiSsidHex(ssid) << "\n";
    if (pass.empty()) {
        w << "    key_mgmt=NONE\n";
    } else {
        w << "    psk=" << (pass.size() == 64 ? pass : "\"" + wpaEscape(pass) + "\"") << "\n"
          << "    key_mgmt=WPA-PSK\n";
    }
    w << "}\n";
    if (!writeFile(runDir() + "/wpa_supplicant.conf", w.str())) return;

    if (!writeFile(runDir() + "/desired-mode", "client\n")) return;
    spawnDetached("wifi-client.sh " + wlan_);
    setState(WifiState::Joining);
    // 重试时状态仍是 Joining，setState 不会更新时间戳，必须单独重置。
    state_since_ms_ = nowMs();
    cached_ip_.clear();
}

void WifiManager::startPortal() {
    if (portal_) return;
    portal_ = new PortalImpl();
    if (!portal_->start(this)) {
        portal_->stop();
        delete portal_;
        portal_ = nullptr;
        fprintf(stderr, "[Wifi] captive portal unavailable; will retry\n");
        return;
    }
    printf("[Wifi] captive portal listening on :80 (%s)\n", portal_url_.c_str());
}

void WifiManager::stopPortal() {
    if (!portal_) return;
    portal_->stop();
    delete portal_;
    portal_ = nullptr;
    printf("[Wifi] captive portal stopped\n");
}

// hostapd 进程是否存活（wifi-ap.sh 启动时落盘 pid 文件）。pid 文件缺失/
// 进程不存在都视为不活；EPERM（进程存在但不属当前用户）算活
bool WifiManager::hostapdAlive() const {
    return daemonAlive("hostapd");
}

void WifiManager::resetToAp() {
    stopPortal(); // Drain in-flight submissions before clearing credentials.
    // 清内存 + 删凭据文件，重回 AP 配网模式。配对关系（token）不动：
    // 新网络入网后自动恢复与 PC 的连接，无需重新输配对码
    {
        std::lock_guard<std::mutex> lk(cred_mu_);
        has_creds_ = false;
        home_ssid_.clear();
        home_pass_.clear();
    }
    std::error_code ec;
    fs::remove(wifiJsonPath(), ec);
    pending_join_ = false;
    join_attempts_ = 0;
    join_failure_ = 0;
    printf("[Wifi] reset to AP provisioning (wifi.json removed)\n");
    enterAp();
}

} // namespace dutyon

#endif // !_WIN32
