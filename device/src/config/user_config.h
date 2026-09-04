#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dutyon {

// 自定义形象（1.x CustomCharacter）：每状态一个动画文件，
// 文件名相对 ~/.dutyon/animations/（serde camelCase）
struct CustomCharacter {
    std::string id;        // "char_xxx"
    std::string name;      // 显示名
    std::string sleeping;  // 各状态动画文件名（空 = 未设置）
    std::string working;
    std::string alert;
};

// ---------------------------------------------------------------------------
// 1.x 用户数据复用：直接读写 ~/.dutyon/config.json（与桌面版共用同一份）。
// 结构对齐 src-tauri/src/user_config.rs（serde camelCase）。
// ---------------------------------------------------------------------------
struct UserConfig {
    bool has_flip = false;
    bool flip = false;
    bool has_mini = false;
    bool mini = false;
    std::string language;                 // 空 = 未设置（默认 zh-CN）
    std::string model_url;                // 旧字段（1.x 早期）
    std::string active_character_id;      // Live2D 模型 URL 或 "char_xxx"
    // stateMotions: { "<模型URL>": { sleeping/working/alert: [组, 序号] } }
    std::map<std::string, std::map<std::string, std::pair<std::string, int>>> state_motions;
    // 监控显隐
    bool monitor_enabled = true;
    bool monitor_collapsed = false;
    bool monitor_show_cpu = true, monitor_show_ram = true, monitor_show_gpu = true;
    bool monitor_show_net = true, monitor_show_self = true, monitor_show_projects = true;
    // 窗口位置记忆（1.x 字段 windowPosition 复用）：x=窗口左缘、y=窗口底边
    //（贴底生长锚点：面板展开/收起高度变化时底边不动，恢复最稳）
    bool has_window_pos = false;
    int win_x = 0;
    int win_y = 0;
    // 自定义 GIF 形象（2.0 原生端 WIC 解码渲染）
    std::vector<CustomCharacter> custom_characters;
    // 硬件显示端模式：single=单任务（角色全屏+大时钟）/ multi=多任务
    //（角色+任务列表）/ frame=电子相框（角色全屏循环播放动作）
    std::string device_mode = "multi";
};

// 模型目录条目
struct ModelEntry {
    std::string name;    // 显示名（model3.json 文件名去后缀）
    std::string dir;     // 模型所在目录（正斜杠）
    std::string json;    // model3.json 文件名
    std::string key;     // 1.x 配置里的 URL 键（内置=assets/live2d/x.json；
                         // 用户=http://127.0.0.1:17521/live2d/<相对路径>）
    bool builtin = false;
};

class UserConfigStore {
public:
    // 读取 ~/.dutyon/config.json（失败返回默认值）
    static UserConfig load();

    // ---- 单字段持久化（读-改-写，保留其余字段）----
    static void saveFlip(bool flip);
    static void saveMini(bool mini);
    static void saveLanguage(const std::string& code);
    static void saveActiveCharacter(const std::string& id);
    static void saveStateMotion(const std::string& model_key, const std::string& state,
                                const std::string& group, int index);
    static void saveMonitor(const UserConfig& cfg);
    // 窗口位置记忆（x=左缘、y=底边，见 UserConfig 注释）
    static void saveWindowPos(int x, int y);
    // 自定义形象列表整体覆写（设备端从 PC 下载新角色后持久化）
    static void saveCustomCharacters(const UserConfig& cfg);
    // 硬件显示端模式（single/multi/frame；PC 菜单选择后经 /api/status 下发）
    static void saveDeviceMode(const std::string& mode);

    // ---- 模型目录（内置 frontend/assets/live2d + 用户 ~/.dutyon/live2d）----
    // builtin_roots: 内置模型搜索目录（相对 exe 解析，main 传入）
    static std::vector<ModelEntry> listModels(const std::vector<std::string>& builtin_roots);
    static std::string userModelsDir();

    // 自定义形象动画文件目录（~/.dutyon/animations）
    static std::string animationsDir();

    // 1.x Live2D 模型缩略图缓存目录（~/.dutyon/thumbnails；1.x 前端生成）
    static std::string thumbnailsDir();
    // 模型缩略图路径（文件名安全化同 1.x Rust thumbnail_path：
    // 非字母数字/连字符替换为 '_'；文件不存在返回空串）
    static std::string thumbnailFor(const std::string& model_name);

    // 按当前模型 key 查状态动作映射（精确 key -> "_default" -> 空）
    static std::map<std::string, std::pair<std::string, int>> stateMotionsFor(
        const UserConfig& cfg, const std::string& model_key);
};

} // namespace dutyon
