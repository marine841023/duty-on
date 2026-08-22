#pragma once

// ---------------------------------------------------------------------------
// IDE 类型标识（从 src-tauri/src/models.rs 的 IdeKind 移植）。
// 序列化为小写字符串 "trae"/"qoder"/"cursor"/"codex"/"opencode"。
// ---------------------------------------------------------------------------

#include <optional>
#include <string>

namespace dutyon::backend {

enum class IdeKind {
    Trae,
    Qoder,
    Cursor,
    Codex,
    OpenCode,
};

inline const char* ideKindStr(IdeKind k) {
    switch (k) {
        case IdeKind::Trae: return "trae";
        case IdeKind::Qoder: return "qoder";
        case IdeKind::Cursor: return "cursor";
        case IdeKind::Codex: return "codex";
        case IdeKind::OpenCode: return "opencode";
    }
    return "";
}

// 字符串 -> IdeKind；未知/空返回 nullopt（旧版桥接脚本不上报 ide 字段）
inline std::optional<IdeKind> ideKindFromStr(const std::string& s) {
    if (s == "trae") return IdeKind::Trae;
    if (s == "qoder") return IdeKind::Qoder;
    if (s == "cursor") return IdeKind::Cursor;
    if (s == "codex") return IdeKind::Codex;
    if (s == "opencode") return IdeKind::OpenCode;
    return std::nullopt;
}

} // namespace dutyon::backend
