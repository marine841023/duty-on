// 1.x 用户数据复用实现：~/.dutyon/config.json 读写 + Live2D 模型目录枚举。
// 后端（duty-on.exe）的 HTTP API 不暴露配置端点（1.x 走 Tauri IPC），
// 客户端直接读写文件 —— 后端仅在 Tauri 命令触发时写配置，2.0 无 WebView
// 调用路径，双写冲突可忽略。

#include "config/user_config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

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

// 读-改-写：只改一个字段，保留文件其余内容（含 1.x 独有字段）
template <typename Fn>
void updateConfig(Fn&& fn) {
    const std::string path = configPath();
    json j = json::object();
    {
        std::ifstream in(path);
        if (in) {
            try {
                in >> j;
            } catch (...) {
                j = json::object();
            }
            if (!j.is_object()) j = json::object();
        }
    }
    fn(j);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (out) out << j.dump(2) << std::endl;
}

// stateMotions JSON -> map（条目格式 {"sleeping": ["Flick3", 1], ...}）
std::map<std::string, std::pair<std::string, int>> parseMotions(const json& j) {
    std::map<std::string, std::pair<std::string, int>> out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const json& v = it.value();
        if (v.is_array() && v.size() >= 2 && v[0].is_string() && v[1].is_number())
            out[it.key()] = {v[0].get<std::string>(), v[1].get<int>()};
    }
    return out;
}

void collectModelFiles(const fs::path& dir, int depth,
                       std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        const fs::path& p = entry.path();
        const std::string fn = p.filename().string();
        if (fs::is_regular_file(p, ec) &&
            fn.size() > 12 && fn.compare(fn.size() - 12, 12, ".model3.json") == 0) {
            out.push_back(p);
        } else if (fs::is_directory(p, ec) && depth > 0) {
            collectModelFiles(p, depth - 1, out);
        }
    }
}

} // namespace

UserConfig UserConfigStore::load() {
    UserConfig cfg;
    std::ifstream in(configPath());
    if (!in) return cfg;
    json j;
    try {
        in >> j;
    } catch (...) {
        return cfg;
    }
    if (!j.is_object()) return cfg;

    if (j.contains("flipHorizontal") && j["flipHorizontal"].is_boolean()) {
        cfg.has_flip = true;
        cfg.flip = j["flipHorizontal"].get<bool>();
    }
    if (j.contains("miniMode") && j["miniMode"].is_boolean()) {
        cfg.has_mini = true;
        cfg.mini = j["miniMode"].get<bool>();
    }
    if (j.contains("language") && j["language"].is_string())
        cfg.language = j["language"].get<std::string>();
    if (j.contains("modelUrl") && j["modelUrl"].is_string())
        cfg.model_url = j["modelUrl"].get<std::string>();
    if (j.contains("activeCharacterId") && j["activeCharacterId"].is_string())
        cfg.active_character_id = j["activeCharacterId"].get<std::string>();
    if (j.contains("deviceMode") && j["deviceMode"].is_string())
        cfg.device_mode = j["deviceMode"].get<std::string>();
    if (j.contains("stateMotions") && j["stateMotions"].is_object()) {
        for (auto it = j["stateMotions"].begin(); it != j["stateMotions"].end(); ++it)
            cfg.state_motions[it.key()] = parseMotions(it.value());
    }
    if (j.contains("monitor") && j["monitor"].is_object()) {
        const json& m = j["monitor"];
        auto b = [&](const char* k, bool dflt) {
            return m.contains(k) && m[k].is_boolean() ? m[k].get<bool>() : dflt;
        };
        cfg.monitor_enabled = b("enabled", true);
        cfg.monitor_collapsed = b("collapsed", false);
        cfg.monitor_show_cpu = b("showCpu", true);
        cfg.monitor_show_ram = b("showRam", true);
        cfg.monitor_show_gpu = b("showGpu", true);
        cfg.monitor_show_net = b("showNet", true);
        cfg.monitor_show_self = b("showSelf", true);
        cfg.monitor_show_projects = b("showProjectList", true);
    }
    if (j.contains("windowPosition") && j["windowPosition"].is_object()) {
        const json& wp = j["windowPosition"];
        if (wp.contains("x") && wp["x"].is_number() &&
            wp.contains("y") && wp["y"].is_number()) {
            cfg.has_window_pos = true;
            cfg.win_x = wp["x"].get<int>();
            cfg.win_y = wp["y"].get<int>();
        }
    }
    if (j.contains("customCharacters") && j["customCharacters"].is_array()) {
        for (const auto& c : j["customCharacters"]) {
            if (!c.is_object() || !c.contains("id") || !c.contains("name")) continue;
            CustomCharacter ch;
            ch.id = c["id"].get<std::string>();
            ch.name = c["name"].get<std::string>();
            auto field = [&](const char* k, std::string& out) {
                if (c.contains(k) && c[k].is_string()) out = c[k].get<std::string>();
            };
            field("sleeping", ch.sleeping);
            field("working", ch.working);
            field("alert", ch.alert);
            cfg.custom_characters.push_back(std::move(ch));
        }
    }
    return cfg;
}

void UserConfigStore::saveFlip(bool flip) {
    updateConfig([&](json& j) { j["flipHorizontal"] = flip; });
}

void UserConfigStore::saveMini(bool mini) {
    updateConfig([&](json& j) { j["miniMode"] = mini; });
}

void UserConfigStore::saveLanguage(const std::string& code) {
    updateConfig([&](json& j) { j["language"] = code; });
}

void UserConfigStore::saveActiveCharacter(const std::string& id) {
    updateConfig([&](json& j) { j["activeCharacterId"] = id; });
}

void UserConfigStore::saveStateMotion(const std::string& model_key,
                                      const std::string& state,
                                      const std::string& group, int index) {
    updateConfig([&](json& j) {
        if (!j.contains("stateMotions") || !j["stateMotions"].is_object())
            j["stateMotions"] = json::object();
        json& motions = j["stateMotions"];
        if (!motions.contains(model_key) || !motions[model_key].is_object())
            motions[model_key] = json::object();
        motions[model_key][state] = json::array({group, index});
    });
}

void UserConfigStore::saveMonitor(const UserConfig& cfg) {
    updateConfig([&](json& j) {
        j["monitor"] = {
            {"enabled", cfg.monitor_enabled},
            {"collapsed", cfg.monitor_collapsed},
            {"showCpu", cfg.monitor_show_cpu},
            {"showRam", cfg.monitor_show_ram},
            {"showGpu", cfg.monitor_show_gpu},
            {"showNet", cfg.monitor_show_net},
            {"showSelf", cfg.monitor_show_self},
            {"showProjectList", cfg.monitor_show_projects},
        };
    });
}

void UserConfigStore::saveWindowPos(int x, int y) {
    // 与 Rust 端 WindowPosition（camelCase）结构一致；后端读-改-写时
    // serde 会解析并保留该字段，双端共存无冲突
    updateConfig([&](json& j) {
        j["windowPosition"] = {{"x", x}, {"y", y}};
    });
}

void UserConfigStore::saveDeviceMode(const std::string& mode) {
    // 设备模式（single/multi/frame）；/api/status 每次轮询读文件下发
    updateConfig([&](json& j) { j["deviceMode"] = mode; });
}

void UserConfigStore::saveCustomCharacters(const UserConfig& cfg) {
    // 设备端从 PC 下载新自定义形象后整体覆写该数组（camelCase 同 1.x serde）
    updateConfig([&](json& j) {
        json arr = json::array();
        for (const auto& c : cfg.custom_characters) {
            arr.push_back({{"id", c.id},
                           {"name", c.name},
                           {"sleeping", c.sleeping},
                           {"working", c.working},
                           {"alert", c.alert}});
        }
        j["customCharacters"] = std::move(arr);
    });
}

std::string UserConfigStore::userModelsDir() {
    const std::string home = homeDir();
    return home.empty() ? std::string("live2d") : home + "/.dutyon/live2d";
}

std::string UserConfigStore::animationsDir() {
    const std::string home = homeDir();
    return home.empty() ? std::string("animations") : home + "/.dutyon/animations";
}

std::string UserConfigStore::thumbnailsDir() {
    const std::string home = homeDir();
    return home.empty() ? std::string("thumbnails") : home + "/.dutyon/thumbnails";
}

std::string UserConfigStore::thumbnailFor(const std::string& model_name) {
    // 文件名安全化（1.x models.rs thumbnail_path：is_alphanumeric 或 '-' 保留，
    // 其余替换 '_'；ASCII 外的字节按字母数字处理以贴近 Rust 的 Unicode 语义）
    std::string safe;
    safe.reserve(model_name.size());
    for (const char c : model_name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' ||
                          (unsigned char)c >= 0x80;
        safe += keep ? c : '_';
    }
    if (safe.empty()) return std::string();
    const std::string path = thumbnailsDir() + "\\" + safe + ".png";
    std::error_code ec;
    return fs::is_regular_file(path, ec) ? path : std::string();
}

std::vector<ModelEntry> UserConfigStore::listModels(
    const std::vector<std::string>& builtin_roots) {
    std::vector<ModelEntry> out;

    // 内置模型：<root>/*.model3.json（1.x build.rs 只扫根层，键为
    // assets/live2d/<文件名>）
    for (const auto& root : builtin_roots) {
        std::error_code ec;
        const fs::path rd(root);
        if (!fs::is_directory(rd, ec)) continue;
        for (const auto& entry : fs::directory_iterator(rd, ec)) {
            const fs::path& p = entry.path();
            const std::string fn = p.filename().string();
            if (!fs::is_regular_file(p, ec)) continue;
            if (fn.size() <= 12 ||
                fn.compare(fn.size() - 12, 12, ".model3.json") != 0)
                continue;
            ModelEntry e;
            e.name = fn.substr(0, fn.size() - 12);
            e.dir = rd.generic_string();
            e.json = fn;
            e.key = "assets/live2d/" + fn;
            e.builtin = true;
            out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ModelEntry& a, const ModelEntry& b) { return a.name < b.name; });

    // 用户模型：~/.dutyon/live2d 递归两层（同 1.x scan_user_models），
    // 键为后端 HTTP 路由 URL（http://127.0.0.1:17521/live2d/<相对路径>）
    std::vector<fs::path> files;
    const fs::path user_root(userModelsDir());
    collectModelFiles(user_root, 2, files);
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        ModelEntry e;
        e.json = p.filename().string();
        e.name = e.json.substr(0, e.json.size() - 12);
        e.dir = p.parent_path().generic_string();
        std::string rel = fs::relative(p, user_root).generic_string();
        e.key = "http://127.0.0.1:17521/live2d/" + rel;
        e.builtin = false;
        out.push_back(std::move(e));
    }
    return out;
}

std::map<std::string, std::pair<std::string, int>> UserConfigStore::stateMotionsFor(
    const UserConfig& cfg, const std::string& model_key) {
    auto it = cfg.state_motions.find(model_key);
    if (it != cfg.state_motions.end() && !it->second.empty()) return it->second;
    auto dflt = cfg.state_motions.find("_default");
    if (dflt != cfg.state_motions.end()) return dflt->second;
    return {};
}

} // namespace dutyon
