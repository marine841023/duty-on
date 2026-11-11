# DutyOn 云服务

首版提供 Windows 安装包下载与 OTA、用户名密码账号、配置备份、用户动画/音频/Live2D 文件管理。首页内置注册、登录、上传、搜索、下载和删除界面，手机可用。图片生成角色和声音克隆尚未接入。

## 在 ECS 部署

建议 Linux 主机至少 2 核、2 GiB 内存，安装 Docker Engine 与 Compose 插件。准备域名并将 A 记录解析到 ECS 公网 IP，安全组开放 TCP 80/443（HTTP/3 可选 UDP 443）。8787 仅绑定本机回环，无需开放。

```sh
git clone https://github.com/marine841023/duty-on.git
cd duty-on/server
mkdir -p packages
cp .env.example .env
# 编辑 .env，将 DUTYON_DOMAIN 改成你的真实域名
docker compose --profile production up -d --build
docker compose ps
docker compose logs --tail=100 cloud caddy
curl -f https://你的域名/healthz
```

Caddy 自动申请和续期 HTTPS 证书。域名必须可从公网访问且指向本机。`172.30.84.0/24` 是 Compose 内网；若与 ECS/VPC 网络冲突，需同时修改 compose 中的子网、两个固定 IP 和 `--trusted-proxy`。服务端只信任指定代理的 X-Real-IP，Caddy 会覆盖用户传入的同名头。

没有域名时可先 `docker compose up -d --build cloud`，在 ECS 本机使用 `http://127.0.0.1:8787`，或通过 SSH 转发到本机测试。公网账号使用 HTTPS。

## 发布安装包和 OTA

先在 Windows 执行 `tools/build-package.ps1`，将所需包复制到服务器 `server/packages/`。三个类型分开发布；更新包根目录必须有 `dutyon-pet.exe`，安装包 ZIP 根目录必须有对应版本的 NSIS 安装器。

```sh
docker compose exec cloud dutyon-cloud --data /data --kind installer --publish /packages/DutyOn_2.0.15_x64-setup.exe --notes '云服务与异步提问提醒'
docker compose exec cloud dutyon-cloud --data /data --kind bundle --publish /packages/DutyOn-v2.0.15.zip --notes '安装包 ZIP'
docker compose exec cloud dutyon-cloud --data /data --kind update --publish /packages/DutyOn-update-v2.0.15.zip --notes '云服务与异步提问提醒'
curl -f https://你的域名/api/app/releases
```

版本取自文件名。发布先校验包结构，再写入 SHA-256 寻址文件，最后更新当前版本指针。客户端检查后的下载固定 SHA-256，新发布不会换掉下载中的包。旧版本文件保留用于已有下载；暂不自动清理。发布命令由服务器管理员通过 SSH 执行，**没有公开的发布 HTTP 接口**，一次只运行一个发布命令。可重新发布已保存的旧包回退当前指针；已升级客户端不会自动降级。

ZIP 外包装是安装器压缩包，不是便携版。服务端检查 ZIP 目录和所需文件，不替代杀毒、代码签名或发布者对包内容的审核。OTA 依赖 HTTPS 保证来源真实性，SHA-256 用于传输完整性校验。

## 配置桌宠

在桌宠云账号登录窗口填写 `https://你的域名`（不带路径），可注册或登录。未登录也可检查 OTA，只要已配置服务地址。构建官方分发包时，可在 CMake 配置阶段传入：

```sh
cmake -S device -B device/build -DDUTYON_CLOUD_URL=https://你的域名
cmake --build device/build --config Release
```

当前源码默认主地址为空，等域名确定后再预置；没有虚构线上服务地址。云端文件在桌宠中选择“从云端恢复”下载。上传文件不会自动建立角色配置：自定义角色的动作映射仍在桌宠里设置，然后随配置备份。恢复会覆盖同名本地文件和云同步白名单配置，请先保存需要的本机内容。

桌宠每分钟备份本机内容，保留云端独有文件。云端同名文件发生变化时，客户端提示先恢复，避免覆盖网页/另一台电脑的修改。网页删除云文件后，本机同名副本仍可能重新上传；永久删除需同步删除本机副本。配置快照采用最后写入覆盖，多台电脑应手动协调配置修改。

## 存储与备份

服务是单实例 JSON/文件存储；不能运行多个 cloud 副本共同写同一数据卷。普通请求中的写入有进程内互斥及临时文件替换。数据卷包含密码哈希、会话令牌、配置、上传内容和发布包，应限制访问并定期备份；不要提交到 Git。

```sh
# 暂停服务后备份，确保跨文件一致。不要执行 down -v。
docker compose stop cloud
docker compose run -T --rm --no-deps --user 0 --entrypoint tar cloud -C /data -czf - . > dutyon-backup.tar.gz
docker compose start cloud
```

恢复到停机状态的数据卷，可将归档以 stdin 传入一次性容器：

```sh
docker compose stop cloud
cat dutyon-backup.tar.gz | docker compose run -T --rm --no-deps --user 0 --entrypoint sh cloud -c 'tar -xzf - -C /data && chown -R 10001:10001 /data'
docker compose start cloud
```

恢复应使用空数据卷或先保留旧卷，否则归档不含的旧文件仍会存在。升级服务端使用 `docker compose up -d --build cloud`，不删除数据卷。

## API 与限制

| 接口 | 用途 |
| --- | --- |
| `GET /healthz` | 存活检查 |
| `POST /api/auth/register` / `login` | `{username,password}` → `{token,username}` |
| `POST /api/auth/logout` | 注销当前 Bearer token |
| `GET /api/profile` | 用户名、配额和用量 |
| `GET/PUT /api/sync/config` | 获取 `{config:{...}}` / 保存白名单配置对象 |
| `GET /api/sync/manifest` | `[{path,size,sha256}]` |
| `GET/PUT/DELETE /api/sync/file?path=...` | 文件下载、原始字节上传、幂等删除 |
| `GET /api/app/version` / `download` | 当前更新包；下载可用 `?sha256=...` 固定版本 |
| `GET /api/app/releases` | update、installer、bundle 当前元数据及下载链接 |
| `GET /api/app/artifact?kind=...&sha256=...` | 不可变发布包 |

profile/sync 使用 `Authorization: Bearer <token>`。账号名 3–32 位字母数字/下划线/连字符；密码 6–1024 字节，PBKDF2-HMAC-SHA256 加盐存储。令牌有效期约 90 天，活动时续期、按小时节流持久化。网页令牌仅放当前标签页 sessionStorage。首版无密码找回、邮件验证、账号删除或公开内容市场。

文件只允许 `animations/` 和 `live2d/` 内具体路径，拒绝路径穿越和 Windows 保留文件名。单文件 50 MiB、每账号 500 MiB；配置快照最大 1 MiB，不接受本机账号密钥等非白名单字段。支持 PUT `If-Match: "sha256"` 或 `If-None-Match: *` 防止并发覆盖，冲突返回 412。认证按来源 IP 限流，并限制哈希及文件传输并发；大量公开注册需进一步接入运营侧防滥用措施。

## 本地验证

```sh
cd server
go test ./...
go vet ./...
go run . --addr 127.0.0.1:8787 --data ./data
```

测试覆盖账号持久化/注销、跨用户隔离、配置过滤、非法 JSON/路径、配额、并发清单、上传前置条件、三类发布与固定版本下载。Docker 部署需在具备 Docker 的 Linux 环境执行上述验收步骤。
