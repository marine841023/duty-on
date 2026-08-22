#pragma once

// 开机自启动（注册表）—— 对齐 tauri-plugin-autostart 的 Windows 实现：
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run 下键名 DutyOn。
// 从 http_server.cpp 抽出共享：HTTP 端点与本地菜单都直调。

namespace dutyon::backend {

bool autostartEnabled();

// enable=true 写入当前 exe 路径；false 删除键（不存在视为成功，幂等）。
// 返回操作是否成功。
bool setAutostartEnabled(bool enable);

} // namespace dutyon::backend
