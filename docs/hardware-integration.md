# 外接显示屏幕集成指南

DutyOn 内置一个只读 HTTP API，可以将桌宠状态实时推送到外接屏幕（树莓派、平板、手机、第二显示器等）。屏幕只显示状态和播放声音，不支持菜单操作。

## 快速开始

### 同一台机器（第二屏幕）

1. 用浏览器打开 `docs/hardware-demo.html`
2. 默认地址 `http://127.0.0.1:17521` 无需修改，点击「连接」
3. 点击 🔇 按钮启用声音（浏览器要求用户交互后才能播放音频）

### 局域网其他设备（树莓派 / 平板 / 手机）

1. 在桌宠菜单中勾选「允许外部访问」
2. **重启 DutyOn**（绑定地址变更需要重启生效）
3. 查看本机局域网 IP（如 `192.168.1.100`）
4. 在外接设备的浏览器中打开 `http://192.168.1.100:17521/api/status` 确认可访问
5. 将 `docs/hardware-demo.html` 复制到外接设备，修改地址栏为 `http://192.168.1.100:17521`，点击「连接」

> 「允许外部访问」开启后，HTTP 服务器绑定 `0.0.0.0`，局域网内任何设备都能读取 `/api/*` 只读接口。写入接口（`/hook`、`/unregister`、`/log`）仍受 loopback 保护，外部设备无法注入伪造事件。

## API 接口

所有接口仅支持 GET，CORS 完全开放（`Access-Control-Allow-Origin: *`），任何浏览器都能跨域访问。

### `GET /api/status`

返回当前状态快照（JSON）。

```json
{
  "overallState": "sleeping",
  "sessions": [
    {
      "sessionId": "abc-123",
      "projectName": "my-project",
      "projectPath": "/home/user/my-project",
      "ide": "trae",
      "status": "thinking",
      "lastEvent": "UserPromptSubmit",
      "alertMessage": null
    }
  ],
  "lastEventAt": 1722921600000,
  "timestamp": 1722921601000
}
```

| 字段 | 说明 |
|---|---|
| `overallState` | 总体状态：`sleeping`（空闲）/ `working`（忙碌）/ `alert`（需要确认） |
| `sessions[].status` | 会话状态：`idle` / `thinking` / `tool-use` / `working` / `complete` / `confirmation-needed` |
| `sessions[].ide` | IDE 来源：`trae` / `qoder` / `cursor` / `codex` / `opencode` |
| `sessions[].projectName` | 项目名称 |
| `sessions[].alertMessage` | 提醒消息（仅 `confirmation-needed` 状态有值） |
| `lastEventAt` | 最近事件时间戳（毫秒） |

### `GET /api/events`

Server-Sent Events 流，每次状态变更推送完整快照。保持连接即可实时接收更新，每 15 秒发送 keep-alive 心跳。

```javascript
const es = new EventSource('http://127.0.0.1:17521/api/events');
es.onmessage = (e) => {
  const snap = JSON.parse(e.data);
  // 渲染状态...
};
```

### `GET /api/sounds/:state`

返回指定状态的声音文件（`~/.dutyon/sounds/{state}.{mp3,wav,ogg}`）。没有文件时返回 404，调用方静默处理即可。

| `:state` 参数 | 对应状态 |
|---|---|
| `idle` | 空闲 |
| `thinking` | 思考中 |
| `tool-use` | 执行中 |
| `working` | 忙碌中 |
| `complete` | 完成 |
| `alert` | 需要确认 |
| `confirmation-needed` | 需要确认（同 alert） |

优先级：mp3 > wav > ogg。每个状态只需放一个文件。

### `GET /health`

健康检查，返回 `{"status":"ok","port":17521}`。

## 声音文件

1. 在桌宠菜单中点击「打开声音文件夹」（首次会自动创建 `~/.dutyon/sounds/` 并生成 README）
2. 将声音文件放入该目录，按 `{状态名}.{mp3|wav|ogg}` 命名
3. 外接屏幕在进入对应状态时自动播放

示例：
```
~/.dutyon/sounds/
  alert.mp3         ← 需要确认时播放
  thinking.wav      ← AI 开始思考时播放
  complete.ogg      ← 任务完成时播放
  tool-use.mp3      ← 工具调用时播放
```

声音是可选的——没有文件的状态保持静音。

## 自定义集成

`docs/hardware-demo.html` 是一个自包含的参考实现（单文件，无外部依赖），可以直接使用或作为自定义集成的起点。核心逻辑：

1. **加载时** `fetch('/api/status')` 获取初始状态
2. **实时更新** `new EventSource('/api/events')` 订阅 SSE 流
3. **状态变化时** `new Audio('/api/sounds/' + status).play()` 播放声音
4. **断线重连** EventSource 自动重连 + 手动 3 秒重试

### 最简集成代码

```javascript
const BASE = 'http://127.0.0.1:17521';

// 实时状态
const es = new EventSource(BASE + '/api/events');
es.onmessage = (e) => {
  const { overallState, sessions } = JSON.parse(e.data);
  console.log('状态:', overallState, '会话数:', sessions.length);
};

// 播放声音
function playSound(state) {
  new Audio(BASE + '/api/sounds/' + state).play().catch(() => {});
}
```

## 安全说明

| 接口 | 外部访问 | 说明 |
|---|---|---|
| `GET /api/*` | ✅ 允许 | 只读，任何设备可访问 |
| `GET /health` | ✅ 允许 | 健康检查 |
| `POST /hook` | ❌ 拒绝 | 仅 loopback，外部访问返回 403 |
| `POST /unregister` | ❌ 拒绝 | 仅 loopback |
| `POST /log` | ❌ 拒绝 | 仅 loopback |
| `GET /status` | ❌ 拒绝 | 仅 loopback（内部接口，用 `/api/status` 代替） |

即使「允许外部访问」开启，写入接口也只接受来自 `127.0.0.1` 的请求，外部设备无法注入伪造的 hook 事件。
