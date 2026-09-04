#pragma once

// ---------------------------------------------------------------------------
// IDE 窗口扫描 —— src-tauri/src/ide_scanner.rs 的 C++ 移植（仅 Windows）。
//
// EnumWindows 遍历可见顶层窗口，按属主进程名（Qoder.exe/Qoder CN.exe/
// Qoder CN IDE.exe/Trae CN.exe/Trae.exe/Cursor.exe）过滤后用
// SendMessageTimeoutW（250ms 硬超时）读标题，
// 解析出项目名 + IDE 类型。另有 Codex/OpenCode CLI 进程存活检测。
//
// 设计要点（Rust 版踩过的坑，一并移植）：
//   - GetWindowTextW 会向属主线程发同步消息，CMD 跑长命令时无限阻塞，
//     扫描线程一旦持锁等它，整条 hook→状态→UI 管道停摆 —— 必须用
//     SendMessageTimeoutW + SMTO_ABORTIFHUNG。
//   - 先按进程名过滤再读标题：忙/挂死的非 IDE 窗口永远收不到消息。
//     进程名查不到（OpenProcess 失败）的窗口回退到超时保护读。
//   - 提权窗口标题带本地化后缀（[管理员]/[Administrator]），按
//     " - IDE名 [单个括号串]" 形状剥离而非枚举翻译。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "backend/state_manager.h"  // DetectedProject / CliLiveness

namespace dutyon::backend {

// 扫描所有可见顶层窗口，返回 IDE 项目（按项目名去重、字母序）。
// 第二个返回值为"疑似 IDE 但解析失败"的原始标题（诊断用，最多 8 条）。
std::pair<std::vector<DetectedProject>, std::vector<std::string>> scanIdeProjects();

// Codex/OpenCode CLI 进程存活检测（CreateToolhelp32Snapshot，排除 codex-relay）。
CliLiveness scanCliProcesses();

// 把项目名匹配的 IDE 窗口带到前台（精确 folder 匹配优先，标题子串回退，
// 最小化先还原）。返回是否找到并激活了窗口。
bool focusProjectWindow(const std::string& name);

} // namespace dutyon::backend
