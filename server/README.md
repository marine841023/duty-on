# TraePet Update Server

零依赖的 Node.js HTTP 服务器，为 TraePet 桌面宠物提供版本检查和安装包下载服务。

## 快速开始

```bash
# 启动服务器（默认端口 17522）
cd server
node server.js

# 自定义端口和上传 token
PORT=8080 UPLOAD_TOKEN=my-secret node server.js
```

## 工作原理

```
┌─────────────┐        GET /updates/latest.yml        ┌───────────────┐
│  TraePet    │ ──────────────────────────────────────→│  Update Server │
│  (客户端)    │←──────────────────────────────────────│  (server.js)  │
│             │        返回版本信息 (version)           └───────┬───────┘
│             │                                              │
│             │        GET /updates/Setup-1.0.1.exe          │ 读取
│             │ ──────────────────────────────────────→       │ uploads/
│             │←──────────────────────────────────────        │ latest.yml
│             │        下载安装包                            │ *.exe
└─────────────┘                                              └───────┘
```

## API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/updates/latest.yml` | 版本清单（electron-updater 检查更新用） |
| GET | `/updates/<filename>` | 下载安装包/blockmap（支持 Range 断点续传） |
| POST | `/publish` | 上传新版本（需 Bearer token 认证） |
| GET | `/health` | 健康检查 |
| GET | `/` | 状态页面（显示当前版本和文件列表） |

## 发布新版本

### 步骤 1：构建安装包

```bash
# 在项目根目录
npm run dist
# 生成 release/TraePet-Setup-1.0.1.exe + release/latest.yml
```

### 步骤 2：上传到服务器

```bash
# 方式 A：使用发布脚本（自动上传 latest.yml + exe + blockmap）
node scripts/publish.js

# 方式 B：手动复制文件到 server/uploads/
copy release\latest.yml server\uploads\
copy release\TraePet-Setup-1.0.1.exe server\uploads\
```

### 步骤 3：客户端自动检测

已安装的 TraePet 会从配置的 URL 检查 `latest.yml`。如果有新版本，自动下载并在菜单中提示"点击安装"。

## 配置

### 服务器端

| 环境变量 | 默认值 | 说明 |
|---------|--------|------|
| `PORT` | `17522` | 监听端口 |
| `HOST` | `0.0.0.0` | 监听地址 |
| `UPLOAD_TOKEN` | `trae-pet-upload-token` | 上传认证 token |

### 客户端

更新服务器 URL 配置在 `package.json` 的 `build.publish.url`：

```json
"publish": {
  "provider": "generic",
  "url": "http://your-server.com:17522/updates"
}
```

构建后该 URL 被写入 `resources/app-update.yml`。如需修改已安装版本的 URL，编辑该文件即可。

## latest.yml 格式

```yaml
version: 1.0.1
files:
  - url: TraePet-Setup-1.0.1.exe
    sha512: <base64-hash>
    size: 71572572
path: TraePet-Setup-1.0.1.exe
sha512: <base64-hash>
releaseDate: '2026-07-31T22:38:00.000Z'
```

electron-builder 构建时自动生成此文件，无需手动编写。

## 文件结构

```
server/
├── server.js       # 服务器主程序（零依赖）
├── package.json    # 服务器元信息
├── README.md       # 本文件
└── uploads/        # 存放安装包和版本清单（自动创建）
    ├── latest.yml
    ├── TraePet-Setup-1.0.1.exe
    └── TraePet-Setup-1.0.1.exe.blockmap
```
