# Hook 链路 Review 与 Codex 检测重构

日期：2026-11-04。重点审查 `device/src/backend/` 的 Hook 安装、进程扫描、HTTP 接收、状态机和 `hooks/` 桥接脚本；未对渲染、网络配对、云端服务做完整安全审计。

## 项目结构与检测依据

DutyOn 是 C++17 原生桌宠，Windows 端在一个进程内运行 GLFW/OpenGL、Live2D/ImGui 和 HTTP 后端；Linux ARM 设备复用核心并通过网络读取状态。`server/` 的 Go 服务负责独立的账号与同步功能，不是本机 Codex 检测入口。

事件链路：IDE Hook → 桥接脚本 → `POST /hook` → `HookEvent::fromJson` → `StateManager` → 快照/SSE/本机 UI。HTTP 服务同时供局域网设备访问，`/hook` 写入受回环地址检查保护。进程扫描提供退出证据，窗口扫描为 GUI IDE 提供项目占位和退出清理。

Codex 的进程可以在对话空闲时持续存在，也可以同时服务多个会话，因此“存在 codex.exe”不能作为“所有 Codex 会话正在工作”的依据。

## 2026-11-11：v2.0.14 子代理与确认复查

发现并修复的关键问题：主代理 Stop 原先清空整个会话，误消除子代理待确认；设备声音仅观察状态切换，连续确认期间的新请求和周期提醒没有发声；空 pending 被错误当作通知过期证据；普通 Notification 的关键词遍历使用 18 作为 17 项数组的长度，可能越界崩溃。

现按 Session → Actor → Tool/Confirmation 记账。完成事件只清理相同代理的相同调用，请求答复只清理相同 request_id；主 Interrupt 取消整树并阻挡尚未见过的迟到子代理。已完成调用和已答复请求保存有限历史以抑制重复。`alertSeq` 在新确认和每 60 秒提醒时递增，随快照和 SSE 传到设备。

| IDE | 本次处理 | 限制 |
| --- | --- | --- |
| Trae | Notification 分类，确认先于 Pre 可提醒；保留浏览器/电脑确认启发式 | 当前安装接口没有完整子代理事件，依赖宿主实际上报的身份字段 |
| Qoder | 子代理起止、工具失败、权限/通知独立聚合 | 缺少 agent_id/tool_use_id 时只能退化关联；工具名启发式可能误报 |
| Codex | agent_id 隔离、turn_id 过滤、主子停止/取消、同步 request_user_input | 云端或未信任 Hook 不可见；异步提问返回不代表已回答 |
| OpenCode | callID 流去重、父子 session 聚合、permission/question asked/replied/rejected、question.v2 | 按本机 1.19.5 协议及回归事件验证，其他版本需检查事件兼容 |
| Cursor | 10 种事件归一化；观察 Hook 合法响应，失败不阻塞 IDE | 官方无通用权限等待/答复事件；subagentStop 无稳定 ID，生命周期只刷新会话活动，不建立无法关闭的独立子代理 |

这不是所有 IDE 的现场全覆盖测试。后端 CTest 覆盖主子并发、精确答复、重复/迟到事件、新请求/周期序号、取消整树和通知崩溃路径；Windows 桥接 19 场景与 OpenCode 插件事件流回归通过。Shell 仅检查语法。

Windows Release 与 ARM 交叉编译通过，v2.0.14 已替换本机运行程序并部署到设备。通过真实 `/hook` 和 `/api/status` 接口验证了主 Stop 保留子代理确认、第二个请求递增提醒序号、重复请求不重播、逐个答复以及取消后抑制迟到子代理；测试会话随后清理。设备服务保持 active，日志记录两次独立的提醒音播放事件（`[Sound] play ev=2`），这确认了事件消费与播放调用，不等同于人工听音验证。每 60 秒提醒使用模拟时钟回归验证。

## 本次修复

| 级别 | 原问题及影响 | 修改 |
| --- | --- | --- |
| P1 | 模糊进程名匹配将 sandbox、relay、computer-use 等辅助进程当作 Codex；任意活进程还会刷新所有会话活动时间，漏 Stop 后长期显示工作中 | 精确匹配 `codex.exe` / `opencode.exe`；扫描不再刷新 Hook 时间；连续两次成功扫描缺失才清理，扫描失败打断连续缺失计数 |
| P1 | Codex 中断/结束事件未接入，压缩和模型请求阶段缺少真实信号 | 接入 14 种官方事件；Interrupt 结束回合，SessionEnd 删除会话并短期阻止迟到事件重建；使用 turn_id 过滤旧回合事件；compact 不重置忙碌状态 |
| P1 | 单个 pending 工具无法覆盖并发；普通工具完成会清掉另一个工具的确认提醒 | 按 tool_use_id 配对并行调用，无 ID 时按工具名计数；重复 ID 的 Pre 幂等；单独跟踪待确认工具；模型/压缩事件保留待确认状态 |
| P1 | 合并安装时删除整个共享 matcher，连带删除其他应用的 handler；损坏配置备份失败后仍可能覆盖原文件 | 只替换本应用的 bridge handler，保留其他 handler 和组元数据；备份失败拒绝重建 |
| P2 | Codex computer_use 被其他 IDE 的工具名启发式误判成待确认 | Codex 通过 PermissionRequest 或 request_user_input 标记确认，不因普通 computer_use 自动进入提醒 |
| P2 | 命令依赖外层 shell 展开环境变量，忽略自定义 CODEX_HOME 和中文路径 | 写入带引号的绝对脚本路径；安装与查询统一解析 CODEX_HOME；Windows 路径使用宽字符，JSON 路径使用 UTF-8；Interrupt/SessionEnd 超时为 3 秒 |
| P2 | 桥接优先使用其他 IDE 遗留环境变量，造成项目串扰；PowerShell 字符串 POST 存在中文编码问题 | 优先 project_path/cwd/workspace_roots，按当前 IDE 选择环境变量兜底；PowerShell 显式 UTF-8 字节 POST |
| P2 | 原始请求体、prompt 和工具参数/结果写入日志 | 桥接仅转发状态字段白名单；桥接日志不记录原始输入，HTTP 日志仅记 session/event/ide/tool |
| P2 | 空字符串 optional、未知事件、伪造 Cursor session 和同名目录去重造成误判 | 空字段保留 nullopt；拒绝未知事件与缺失会话 ID；按完整项目路径和 IDE 去重显示 |

## 仍存在的边界与后续建议

- **P2：会话与进程尚无一一对应。** 另一实例仍在时，无法只凭全局进程扫描确认某个会话是否崩溃。正常会话靠 Stop/Interrupt/SessionEnd；漏事件后保留现有 10 分钟会话/思考超时、15 分钟在途工具超时。超长任务没有新事件仍可能被清理。
- **P2：确认状态豁免超时。** 用户等待回答时保持提醒是现有设计；如果结束事件丢失且另一 Codex 进程仍在，提醒可能残留。可手动重置，后续应基于可靠的每会话归属补强清理。
- **P2：GUI 窗口仍按项目名匹配。** 不同路径或 IDE 的同名窗口可能互相影响占位和清理。完整路径去重修复仅作用于 Hook 会话展示，窗口标题通常不提供完整路径。
- **P2：内部 session_id 没有按 IDE 加命名空间。** 不同 IDE 发生相同 ID 时仍可能合并；跨 IDE namespace 应作为后续协议迁移处理。
- **P2：安装写入不是事务。** 多 IDE 部分失败可能以 warning 返回；配置写入未采用临时文件原子替换，也没有跨进程锁。备份失败保护不能解决并发编辑覆盖或写入中断。
- **P3：HTTP 接收日志仍无容量上限，Windows 日志路径仍使用窄字符。** 桥接日志已有轮转；中文用户目录下应同时检查桥接日志和 `/api/status`。本次未扩展到通用日志框架。
- 无 tool_use_id 时，同名并行调用只能计数，无法同时可靠区分重复上报与不同调用；前后事件缺少一致关联字段时，确认状态也无法精确配对。
- Shell 桥接已同步路径优先级、字段白名单与缺失 ID 处理，但仍通过 `cat` 等待 stdin EOF，依赖宿主超时；本次 Windows 环境只验证其语法，未声称完成 macOS/Linux 端到端验证。

## Codex 安装与排查

1. 精灵的安装器将脚本复制到 `~/.dutyon/hooks/`，配置写入 `$CODEX_HOME/hooks.json`，未设置时为 `~/.codex/hooks.json`。启动精灵时须带相同 CODEX_HOME；当前仅在对应配置目录已存在时安装。
2. 在 Codex CLI 的 `/hooks` 检查并信任新定义。官方信任绑定精确定义，修改命令/事件后需重新确认；应用不会自动写入信任。若 `features.hooks=false`，先开启。
3. 重启 IDE/会话，依次验证提交问题、工具执行、用户输入、Stop 与 Interrupt。检查 `~/.dutyon/hooks/bridge.log`、`~/.dutyon/hook-received.log` 和 `/api/status`。
4. “已安装”只表示 JSON 内存在本应用 handler，不代表所有事件齐全、Codex 信任已生效或事件已成功送达。本机桥接不保证观测云端执行的会话。

协议依据：[OpenAI 官方 Hooks 文档](https://developers.openai.com/codex/hooks)、[配置参考](https://developers.openai.com/codex/config-reference)。不同 Codex 版本的支持范围可能不同。

## 验证

独立后端测试不依赖渲染资源，安装测试将 USERPROFILE/CODEX_HOME 指向临时中文目录，不操作真实用户配置：

```powershell
cmake -S device/tests -B device/build/backend-tests -G "Visual Studio 17 2022" -A x64
cmake --build device/build/backend-tests --config Release --parallel 4
ctest --test-dir device/build/backend-tests -C Release --output-on-failure
node hooks/tests/bridge.test.cjs
node hooks/tests/opencode.test.cjs
bash -n hooks/trae-hook-bridge.sh
cmake --build device/build --config Release --parallel 4
git diff --check
```

Node 测试在 Windows PowerShell 5.1 下通过模拟 HTTP 函数验证中文、UTF-8 BOM、路径归属、Cursor 归一化、缺失字段、非法 JSON、字段白名单和日志。后端测试覆盖并行工具、确认保留、中断/结束后的迟到事件、旧 turn、压缩、超时、扫描失败、进程名分类、同名目录、中文安装路径、共享 handler、幂等安装和备份失败保护。

独立测试不修改真实用户配置。另已按用户要求完成 Windows 完整程序启动、渲染和正常退出验证；托盘菜单点击效果尚未完成实测。完整 Release 构建存在既有 `LNK4098` 默认库冲突警告，需要在独立构建配置工作中处理。
