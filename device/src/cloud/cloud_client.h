#pragma once
// ---------------------------------------------------------------------------
// 云端账户客户端（仅 Windows PC 端）—— 账号登录 / 云同步 / 一键升级
//
// 服务端：dutyon-cloud（Go，仓库 server/ 目录，明文 HTTP，Bearer token 鉴权）
//   POST /api/auth/{register,login}   登录/注册 → token
//   GET/PUT /api/sync/config          配置白名单快照
//   GET /api/sync/manifest + /api/sync/file?path=   文件清单与传输
//   GET /api/app/{version,download}   升级包分发
//
// 传输：cpp-httplib Client（http:// 明文；不链 OpenSSL，https 后续需要再加）
//
// 线程模型：
//   · 登录/注册在 UI 线程同步调用（登录弹窗内，5s 超时）
//   · 登录后启动 worker 线程：每 60s 自动增量上传（animations/ + live2d/
//     + 配置白名单快照）；restore/checkUpdate/download 经原子标志请求
//   · 主线程每帧轮询只读状态（mutex 保护字符串）；云恢复完成后主线程
//     重载配置并切换角色（见 main.cpp restoreDone 分支）
//
// 升级（ZIP 全量替换）：
//   checkUpdate → GET /api/app/version → semver 比较；startUpdateDownload
//   → 下载 %TEMP%\dutyon-update\pkg.zip + sha256 校验 + PowerShell 解压到
//   staged\；applyUpdate（主线程）→ 生成 updater.cmd（等待本进程退出 →
//   xcopy 覆盖 exe 目录 → 重启程序）分离启动后返回 true，调用方走退出流程
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace dutyon {

class CloudClient {
public:
    struct UpdateInfo {
        bool available = false;
        std::string version;   // 新版本号（如 2.0.11）
        std::string notes;     // 发布说明
        long long size = 0;    // 升级包字节数
        std::string sha256;    // 升级包校验值
    };
    enum class UpdateState {
        kIdle,         // 未检查 / 无新版
        kAvailable,    // 发现新版本（未开始下载）
        kDownloading,  // 下载/校验/解压中（hint 显示进度）
        kReady,        // 就绪，点菜单即应用并重启
        kFailed,       // 检查/下载失败（hint 显示原因）
    };

    CloudClient() = default;
    ~CloudClient();
    CloudClient(const CloudClient&) = delete;
    CloudClient& operator=(const CloudClient&) = delete;

    // ---- 账户（UI 线程同步调用；返回空串 = 成功，否则为错误提示）----
    // 成功后 token/server/username 已持久化到 config.json
    std::string registerAccount(const std::string& server, const std::string& user,
                                const std::string& pass);
    std::string login(const std::string& server, const std::string& user,
                      const std::string& pass);
    void logout();  // 清 token + 停 worker

    // 启动时从 config.json 恢复账户（不联网；token 非空即视为已登录）
    void setAccount(const std::string& server, const std::string& token,
                    const std::string& username);

    bool loggedIn() const { return !account_.token.empty(); }
    std::string username() const;
    std::string serverUrl() const;

    // ---- 后台任务（worker 线程执行；主线程仅置标志）----
    void startWorker();    // 登录后 / 启动时 token 非空调用；幂等
    void syncNow();        // 立即执行一次增量上传
    void requestRestore(); // 云恢复：下载差异文件 + 配置合并落盘
    void checkUpdate();    // 检查新版本
    void startUpdateDownload();  // 下载升级包（校验 + 解压到 staged）

    // ---- 主线程状态轮询 ----
    bool restoreDone() const;         // 云恢复已落盘待应用（消费后清零）
    void consumeRestore();            // 主线程应用完配置后清除标志
    std::string syncHint() const;     // "已同步 HH:MM" / "同步失败：…" / "正在同步…"
    UpdateState updateState() const;
    std::string updateHint() const;   // "当前 2.0.10" / "发现 2.0.11 (38 MB)" / "下载 45%" / …

    // 一键升级执行（主线程）：写 updater.cmd + 分离启动 cmd；成功返回
    // true，调用方随后走程序退出流程（释放 exe/dll 锁供 xcopy 覆盖）
    bool applyUpdate();

    // 升级残留清理（启动时调用：上次升级的 %TEMP%\dutyon-update 与
    // 安装目录 dutyon-pet.old 兜底删除）
    static void cleanupUpdateLeftovers();

private:
    struct Account {
        std::string server;   // 形如 http://192.168.1.100:8787
        std::string token;
        std::string username;
    };

    void workerLoop();
    bool doUploadOnce(std::string& err);    // 增量上传（文件 + 配置快照）
    bool doRestore(std::string& err);       // 云恢复（文件 + 配置合并）
    void doCheckUpdate();
    void doDownloadUpdate();
    bool parseServerUrl(const std::string& url, std::string& host, int& port) const;

    void setSyncHint(const std::string& s);
    void setUpdateHint(const std::string& s);

    Account account_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> sync_requested_{false};
    std::atomic<bool> restore_requested_{false};
    std::atomic<bool> update_check_requested_{false};
    std::atomic<bool> update_download_requested_{false};
    std::atomic<bool> restore_done_{false};

    mutable std::mutex mtx_;               // 保护下列字符串/结构
    std::string sync_hint_;
    std::string update_hint_;
    UpdateState update_state_ = UpdateState::kIdle;
    UpdateInfo update_info_;
};

} // namespace dutyon

#endif // _WIN32
