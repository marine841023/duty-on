# Trae Pet - Live2D 桌面精灵监控

一个浮在桌面顶层的 Live2D 精灵，用于实时监控多个 Trae IDE 实例的 AI 任务状态。

## 功能

- **Live2D 精灵**：浮在桌面最顶层的透明窗口，支持拖拽移动
- **三种状态动画**：
  - 💤 **睡觉**：所有 IDE 空闲时，精灵闭眼睡觉，飘出 ZZZ
  - ⚡ **忙碌**：有 AI 任务正在执行时，精灵睁眼专注工作，冒出火花
  - 🔔 **提醒**：需要你确认操作时，精灵惊讶抖动，弹出感叹号
- **状态栏**：精灵下方显示所有已连接的 Trae IDE 项目及其状态
- **点击跳转**：点击状态栏中的项目名，自动激活对应的 Trae IDE 窗口
- **多 IDE 支持**：同时监控多个 Trae IDE 实例，每个实例独立追踪

## 工作原理

```
┌─────────────────────────────────────────────┐
│           Trae Pet (Electron 桌面应用)        │
│  ┌───────────────────────────────────────┐  │
│  │  Live2D 精灵 (pixi-live2d-display)     │  │
│  │  状态: 💤 睡觉 / ⚡ 忙碌 / 🔔 提醒     │  │
│  └───────────────────────────────────────┘  │
│  ┌───────────────────────────────────────┐  │
│  │  状态栏 (项目列表 + 状态指示)           │  │
│  └───────────────────────────────────────┘  │
│  ┌───────────────────────────────────────┐  │
│  │  HTTP Server (localhost:17521)         │  │
│  └───────────────────────────────────────┘  │
└──────────────────┬──────────────────────────┘
                   │ HTTP POST /hook
                   │ (localhost)
    ┌──────────────┼──────────────┐
    │              │              │
┌───┴───┐    ┌────┴───┐    ┌────┴───┐
│ Trae  │    │ Trae   │    │ Trae   │
│ IDE 1 │    │ IDE 2  │    │ IDE 3  │
│Proj A │    │Proj B  │    │Proj C  │
└───────┘    └────────┘    └────────┘
  Hook        Hook           Hook
  事件         事件           事件
```

### Trae IDE Hook 事件映射

| Hook 事件 | 触发时机 | 精灵状态 |
|-----------|---------|---------|
| `SessionStart` | IDE 会话创建 | 项目上线 (idle) |
| `UserPromptSubmit` | 用户发送消息 | → 忙碌 (working) |
| `PreToolUse` | AI 即将执行工具 | → 忙碌 (working) |
| `PostToolUse` | AI 工具执行完成 | → 仍忙碌 (working) |
| `Notification` | 需要用户确认 | → 提醒 (alert!) |
| `Stop` | AI 完成任务 | → 空闲 (idle) |

## 快速开始

### 1. 安装依赖

```bash
cd traeSprite
npm install
```

> 安装时会自动下载 Live2D Cubism Core SDK。如果下载失败，应用会在运行时尝试 CDN 加载。

### 2. 启动精灵

```bash
npm start
```

精灵会出现在屏幕右下角，初始状态为"睡觉"。

### 3. 安装 Hook 集成

有两种方式安装 Hook：

**方式一**：在精灵的菜单中点击"安装 Hook 集成"

**方式二**：运行独立安装脚本

```powershell
.\hooks\install-hooks.ps1
```

安装后，**重启 Trae IDE 或开启新的 AI 会话**，Hook 即可生效。

### 4. 测试

#### 单元测试

状态机核心逻辑由 Jest 单元测试覆盖（事件→状态映射、多会话优先级、超时清理、提醒重复、脏检查）：

```bash
npm test          # 运行一次
npm run test:watch # 监听模式
```

#### 端到端回归脚本

需先启动精灵（`npm start`），再在另一个终端运行：

```powershell
# 5 步状态流转回归：sleeping → working → alert → sleeping → 清理
.\.userdata\test-flow.ps1

# Notification 白名单分类：task_complete→idle、模糊→idle、tool_name→alert
.\.userdata\test-notification.ps1
```

#### 手动测试

在精灵菜单中可以选择"测试: 忙碌/提醒/睡觉状态"来预览三种动画效果。

也可以手动发送测试事件：

```powershell
# 测试忙碌状态
$body = '{"session_id":"test-1","hook_event_name":"UserPromptSubmit","project_path":"C:\test-project","project_name":"test-project"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'

# 测试提醒状态（带 tool_name 视为需确认 → alert）
$body = '{"session_id":"test-1","hook_event_name":"Notification","project_path":"C:\test-project","project_name":"test-project","tool_name":"RunCommand"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'

# 测试任务完成通知（notification_type=task_complete → idle，不触发提醒）
$body = '{"session_id":"test-1","hook_event_name":"Notification","project_path":"C:\test-project","project_name":"test-project","notification_type":"task_complete"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'

# 测试空闲状态
$body = '{"session_id":"test-1","hook_event_name":"Stop","project_path":"C:\test-project","project_name":"test-project"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'
```

## 项目结构

```
traeSprite/
├── package.json                    # 项目配置 (含 jest 测试脚本)
├── jest.config.js                  # Jest 测试配置
├── src/
│   ├── main/
│   │   ├── index.js                # Electron 主进程 (窗口 + IPC + Hook安装)
│   │   ├── server.js               # HTTP 服务器 (接收Hook事件)
│   │   ├── state-manager.js        # 状态管理器 (多会话追踪)
│   │   ├── config.js               # 主进程集中配置 (端口/超时/白名单)
│   │   ├── preload.js              # 预加载脚本 (IPC 桥接)
│   │   └── __tests__/
│   │       └── state-manager.test.js  # StateManager 单元测试
│   └── renderer/
│       ├── index.html              # 渲染进程 HTML
│       ├── renderer.js             # Live2D渲染 + 状态动画 + UI
│       └── styles.css              # 样式
├── hooks/
│   ├── trae-hook-bridge.ps1        # Hook桥接脚本 (被Trae IDE调用)
│   ├── hooks-template.json         # hooks.json 配置模板
│   └── install-hooks.ps1           # 独立安装脚本
├── scripts/
│   └── download-assets.js          # 下载 Cubism Core SDK
├── assets/
│   ├── libs/                       # 第三方库 (Cubism Core)
│   └── live2d/                     # Live2D 模型文件
└── .userdata/                      # 端到端测试脚本与事件 fixture
    ├── test-flow.ps1               # 5 步状态流转回归脚本
    ├── test-notification.ps1       # Notification 白名单分类测试
    └── ev-*.json                   # 测试用 Hook 事件样本
```

## 自定义 Live2D 模型

默认优先加载本地 `nito` 模型（`assets/live2d/nito.model3.json`），若本地缺失则回退到 CDN 的 Hiyori 模型。要使用自定义模型：

1. 将 Live2D 模型文件放入 `assets/live2d/` 目录
2. 确保 `.model3.json` 文件存在
3. 修改 `src/renderer/renderer.js` 中的 `MODEL_URLS` 数组，将本地路径放在首位

```javascript
const MODEL_URLS = [
  '../../assets/live2d/your-model.model3.json',  // 你的模型
  // ... fallback URLs
];
```

## API 接口

精灵运行一个本地 HTTP 服务器 (`http://127.0.0.1:17521`)：

| 接口 | 方法 | 说明 |
|------|------|------|
| `/hook` | POST | 接收 Hook 事件 |
| `/status` | GET | 获取当前状态快照 |
| `/health` | GET | 健康检查 |
| `/unregister` | POST | 注销会话 |

## 技术栈

- **Electron** - 桌面应用框架
- **PixiJS v7** - 2D 渲染引擎
- **pixi-live2d-display** - Live2D 模型加载与渲染
- **Live2D Cubism Core SDK** - Live2D 运行时核心
- **Trae IDE Hooks** - AI 生命周期事件钩子

## 常见问题

**Q: 精灵不显示 Live2D 模型？**
A: 检查网络连接（首次需要从 CDN 加载模型），或手动下载模型文件到 `assets/live2d/`。

**Q: Hook 安装后没有反应？**
A: 确保重启了 Trae IDE 或开启了新的 AI 会话。检查 `~/.trae-cn/hooks.json` 是否包含 `.trae-pet` 配置。

**Q: 精灵一直显示睡觉？**
A: 确认精灵正在运行（检查 `http://127.0.0.1:17521/health`），并确认 Hook 已正确安装。

**Q: AI 任务完成了，精灵却显示"需要确认"？**
A: Notification 事件若没有可识别的完成/确认特征（`notification_type`、`tool_name`、关键词），会被当作模糊事件处理。默认按"任务完成"处理（不提醒）；若你希望模糊事件也触发提醒，可在 `src/main/config.js` 中将 `ALERT_ON_AMBIGUOUS_NOTIFICATION` 设为 `true`。完成类与确认类的白名单同样在该文件中调整。

**Q: 多个 IDE 同时工作，状态栏显示不全？**
A: 状态栏支持滚动，最多显示约 5 个项目。超出部分可滚动查看。
