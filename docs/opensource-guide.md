# 开源发布与运营方案 · Duty On 开工啦

本文档是发布前的操作手册：上传步骤、仓库设置、发版流程、推广物料与日常管理方案。
**只需要按顺序执行一遍「一、上传前检查」和「二、首次上传」，项目就上线了。**

---

## 一、上传前检查（必做）

| # | 事项 | 怎么做 |
|---|------|--------|
| 1 | ~~替换占位符~~ | 已完成：全部替换为 `marine841023` |
| 2 | **LICENSE 署名** | 把 `LICENSE` 第 3 行 `DutyOn Authors` 改成你的名字/网名 |
| 3 | **确认用户模型不入库** | `~/.dutyon/`（含 unitychan 测试模型）在仓库之外，不会被提交；不要手动复制进仓库（Unity-Chan 素材有自己的许可，不能随项目分发） |
| 4 | **提交所有未提交改动** | 最近的功能修复（迷你模式、/live2d 路由、分辨率增强等）和本次开源物料都还在工作区，见下方提交命令 |
| 5 | **本地最终验证** | `cd src-tauri && cargo test`（应 52 全绿） |

提交命令（PowerShell/CMD 均可；commit message 用 -F 文件避免引号转义问题）：

```bash
cd /d d:\src\traeSprite
git add -A
echo docs: open-source readiness (README EN/zh-CN, LICENSE, NOTICE, CI, landing page) > %TEMP%\cmsg.txt
git commit -F %TEMP%\cmsg.txt
```

---

## 二、首次上传 GitHub

1. **建仓库**（二选一）：
   - 网页：github.com/new → 名字 `duty-on` → Public → **不要**勾 README/LICENSE/.gitignore（本地已有）
   - 或 CLI：`gh repo create duty-on --public --source . --push`
2. **关联并推送**（网页建仓时）：

   ```bash
   git remote add origin https://github.com/marine841023/duty-on.git
   git branch -M main        # GitHub 默认主分支是 main；想用 master 可跳过
   git push -u origin main
   ```

   > 若提示认证：浏览器会弹出 GitHub 登录（需装 Git Credential Manager，Git for Windows 自带）。

3. **仓库装修**（网页 Settings / 首页）：
   - 首页右上 **About → ⚙**：Description 填 `A Live2D desk pet that watches your AI coding agents (Trae/Qoder). 桌面精灵实时监控 AI 任务状态`；Website 填 Pages 地址（见第 4 步）；Topics 加 `tauri` `rust` `live2d` `desktop-pet` `trae` `qoder` `ai-agent` `windows`
   - **Features**：勾选 Issues；Discussions 建议开（用户晒模型/提问）
4. **开启 GitHub Pages**（落地页上线）：
   - Settings → Pages → Source: `Deploy from a branch` → Branch: `main` + 目录 `/docs` → Save
   - 约 1 分钟后访问 `https://marine841023.github.io/duty-on/` 即是广告页（docs/index.html + assets/hero.png）

## 三、发版流程（Release）

**推荐：打 tag 走 CI 自动发版**（`.github/workflows/build.yml` 已配好）：

```bash
git tag v1.0.0
git push origin v1.0.0
```

推送 tag 后 GitHub Actions 自动：跑 52 个测试 → Windows 构建 NSIS → 创建 Release 并上传安装包 + 自动生成更新日志。macOS/Linux 的构建矩阵已在 workflow 里注释好，验证过环境后取消注释即可。

**手动发版**（CI 不可用时）：本地 `cargo tauri build`，在 Releases 页拖拽上传 `src-tauri/target/release/bundle/nsis/DutyOn_x.x.x_x64-setup.exe`。

**版本号约定**：语义化 `v主.次.修`——新功能升次版本、修 bug 升修订号、UI/协议不兼容升主版本。同步改 `src-tauri/tauri.conf.json` 的 `version` 与 `Cargo.toml`。

## 四、Screenshots & media（截图清单）

README 的截图区等你补图。建议截以下素材，放进 `docs/screenshots/` 并替换 README「Screenshots」一节：

| 文件 | 内容 | 截法 |
|------|------|------|
| `normal-mode.png` | 完整模式：精灵 + 状态栏挂 2~3 个 IDE 项目 | 正常摆放右下角整窗截 |
| `mini-mode.png` | 迷你模式缩在角落 | 菜单切迷你后截 |
| `menu.png` | 右键菜单展开「切换形象」子菜单 | 菜单打开时截 |
| `state-sleeping.png` / `state-working.png` / `state-alert.png` | 三状态特写 | 用 `.userdata/test-flow.ps1` 逐个造状态 |
| `demo.gif` | 状态联动：IDE 发事件 → 精灵从睡到忙再到提醒 | ScreenToGif 录 10 秒 |

> 截图时背景放个 IDE 窗口更有说服力；注意别拍到隐私代码。

## 五、推广物料

### 中文（V2EX / 掘金 / 即刻）

> **【开源】开工啦：一只盯着 AI 打工仔干活的 Live2D 桌面精灵**
>
> 现在同时开几个 IDE 跑 AI 已经很常见了，但每个 agent 是干完了还是卡在等你确认，只能不停切窗口看。做了个小东西：一只浮在桌面上的 Live2D 小人儿，实时聚合所有 Trae/Qoder 会话状态——全员空闲她就睡觉飘 ZZZ，有任务在跑她就睁眼踱步，有 agent 等你确认她就着急地弹感叹号。状态栏列出每个项目，点名字直达对应 IDE 窗口。
>
> Tauri 2 + Rust 写的，内存 ~80MB（Electron 版的零头），真·点击穿透，支持迷你模式和自换 Live2D 皮。52 个单测护航，MIT 开源。Windows 安装包直接下，mac/Linux 一行 cargo 构建。
> GitHub: https://github.com/marine841023/duty-on

### 英文（Show HN / Reddit r/selfhosted、r/opensource / X）

> **Duty On – a Live2D desk pet that watches your AI coding agents**
>
> I run AI agents in multiple IDE windows and got tired of Alt-Tabbing to check on them. Duty On is a tiny transparent Live2D character that floats above your desktop and aggregates the state of every Trae/Qoder session: 💤 all idle · ⚡ working · 🔔 needs your confirmation. A status bar lists each project; clicking it focuses that IDE window.
>
> Built with Tauri 2 + Rust (~80MB RAM), real click-through, mini mode, bring-your-own Live2D model, 8 languages. MIT licensed, Windows installer in Releases.
> https://github.com/marine841023/duty-on

**发布节奏建议**：先发 V2EX/掘金（中文 IDE 用户浓度高）→ 隔天 Show HN（美西上午 = 北京晚上 11 点）→ X/Reddit 带 `demo.gif` 图。GIF 演示动图的传播效率远高于截图。

## 六、日常管理方案

- **分支**：`main` 保持可发布；自己的功能开发也开短分支 + PR（哪怕只有你一个人），让 CI 测试跑在 PR 上
- **Issue 分流**：bug 走模板（要求附 `~/.dutyon/frontend.log`）；功能建议先 Discussion 再转 Issue，避免许愿池
- **标签**：`bug` / `enhancement` / `good first issue`（留给翻译、文档、新动作映射这类低门槛任务，吸引路人贡献）/ `help wanted`
- **里程碑**：每个次版本一个 milestone（如 `v1.1 mini-mode polish`），Release notes 按 milestone 汇总
- **安全**：端口 17521 只监听回环已是最小面；如收到安全报告，开 GitHub Security Advisory 私下修再公开
- **版本兼容承诺**：`~/.dutyon/config.json` 与用户模型目录向后兼容，破坏性格式变更需写迁移说明

## 七、Why not the Tauri asset protocol

（README 引用本节）用户自定义模型最初通过 Tauri asset protocol（`http://asset.localhost/...`）提供，但 cubism4/pixi 的 XHR 加载器带自定义请求头，触发 CORS 预检；asset handler 响应不含 CORS 头，预检失败报模糊的 "Network error"——而同 URL 的 fetch 探针（简单请求无预检）返回 200。鉴别特征：**fetch 通、XHR 挂**。
解法：本地 axum 服务器加 `GET /live2d/*path` 路由（拒绝 `..` 穿越 + 空段），经 `tower_http::cors` 放行 webview origin 且 `allow_headers(Any)`。实现见 `src-tauri/src/server.rs`，测试覆盖预检/穿越/404。
