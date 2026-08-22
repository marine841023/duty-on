#pragma once

// ---------------------------------------------------------------------------
// IDE hook 安装器 —— src-tauri/src/hooks_installer.rs 的 C++ 移植。
//
// 把桥接脚本装到 ~/.dutyon/hooks/，并把 hook 条目合并进：
//   ~/.trae-cn/hooks.json   （Trae，Nested 格式，加 version:1）
//   ~/.qoder/settings.json  （Qoder，Nested + shell 字段，仅在已安装时）
//   ~/.cursor/hooks.json    （Cursor，Flat 格式 + camelCase 事件，仅已装时）
//   ~/.codex/hooks.json     （Codex，Nested，剥 version 字段，仅已装时）
//   ~/.config/opencode/plugins/dutyon-bridge.js（OpenCode JS 插件，仅已装时）
//
// 合并逻辑的关键性质（1.x 踩坑沉淀，全部保留）：
//   - 幂等：按命令串里的 .dutyon/.trae-pet 标记去重，重装不叠加
//   - 容错：BOM 容忍；损坏/形状错误的文件先备份再重建，绝不静默清空
//   - 保留：其他工具的 hook 条目与非 hooks 键原样保留
// ---------------------------------------------------------------------------

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace dutyon::backend {

// install() 的结果（camelCase 序列化，与 Rust 版 /api/hooks/install 响应一致）
struct InstallResult {
    bool success = false;
    std::optional<std::string> error;
    std::optional<std::string> warning;      // 非致命提示（如损坏配置已备份重建）
    std::optional<std::string> hook_dir;
    std::optional<std::string> hooks_path;
    std::optional<std::string> qoder_hooks_path;
    std::optional<std::string> cursor_hooks_path;
    std::optional<std::string> codex_hooks_path;
    std::optional<std::string> opencode_plugin_path;
    bool needs_enable = false;

    nlohmann::json toJson() const;
};

// is_installed() 的诊断状态（camelCase 序列化）
struct InstalledStatus {
    bool installed = false;      // bridge 存在且至少一个 IDE 已接线
    bool hooks_exist = false;    // Trae hooks.json 含宠物条目
    bool bridge_exists = false;  // ~/.dutyon/hooks/trae-hook-bridge.ps1 存在
    bool qoder_hooks_exist = false;
    bool cursor_hooks_exist = false;
    bool codex_hooks_exist = false;
    bool opencode_plugin_exist = false;

    nlohmann::json toJson() const;
};

// 安装/刷新。hooks_source_dir 为随程序分发的 hooks 资源目录
// （含 trae-hook-bridge.ps1 / install-hooks.ps1）。
InstallResult installHooks(const std::string& hooks_source_dir);

// 解析 hooks 资源目录：打包布局 <exe>/hooks 或 <exe>/resources/hooks；
// 开发布局回退 ../../../hooks。找不到含桥接脚本的目录时返回第一个候选。
std::string resolveHooksSourceDir();

// 查询安装状态（各 IDE 独立报告）。
InstalledStatus isHooksInstalled();

} // namespace dutyon::backend
