# sync-device.ps1 —— 设备程序自动更新（本地交叉编译 + 直推部署）
#
# 由 PC 端 dutyon-pet 的 launchDeviceSync() 调用（设备连上 PC 且源码哈希与
# 设备上报版本不一致时触发），CLI 接口与进度标记沿用旧设备端编译版：
#   powershell -File sync-device.ps1 -Repo <仓库根> -LogFile <进度日志> -Version <源码哈希>
# 日志标记：PROGRESS=<0-100> <stage>（stage ∈ remote/configure/build/deploy/
#   restart/done，首词映射进度窗文案）+ 最后一行 RESULT=OK / RESULT=FAIL:<原因>
#
# 与旧版差异（设备性能低，不再用设备编译）：
#   旧版: tar 源码 → 推设备 → 设备 cmake 编译(1-5 分钟) → deploy.sh → 重启
#   新版: 本地交叉编译(增量 ~10-60 秒) → scp 单二进制 → deploy.sh → 重启
# 流程: SSH 探活 → (必要时) configure → 本地增量构建 → 推送二进制+脚本 →
#   远端 deploy.sh → 写 /opt/dutyon/VERSION(哈希) → 重启服务

param(
    [string]$Repo = 'd:\src\traeSprite',
    [string]$LogFile = '',
    [string]$Version = '',
    [string]$Device = 'root@192.168.7.1'
)

$ErrorActionPreference = 'Stop'

function Log([string]$msg) {
    if ($LogFile) { Add-Content -Path $LogFile -Value $msg -Encoding ASCII }
    Write-Host $msg
}
function Fail([string]$msg) { Log "RESULT=FAIL:$msg"; exit 1 }

# VS BuildTools 自带 cmake（PATH 里没有）；找不到则回退 PATH cmake
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

$sshOpts = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', '-o', 'StrictHostKeyChecking=no')
$buildDir = Join-Path $Repo 'device\build-cross'
$bin = Join-Path $buildDir 'dutyon-pet'

# ---- 1. SSH 探活（设备不可达时干净失败，进度窗 4 秒后自动关）----
Log 'PROGRESS=5 remote'
ssh @sshOpts $Device 'echo ok' *> $null
if ($LASTEXITCODE -ne 0) { Fail 'device unreachable' }

# ---- 2. 本地交叉构建（增量；首次/工具链变更时自动 configure）----
$ninja = Join-Path $buildDir 'build.ninja'
if (-not (Test-Path $ninja)) {
    Log 'PROGRESS=15 configure'
    # FetchContent 本地源码覆盖（离线加速；目录不存在时留空走网络拉取）
    $jsonSrc = Join-Path $Repo '.userdata\deps-src\json'
    $stbSrc = Join-Path $Repo 'device\third_party\deps\stb-master'
    $extra = @()
    if (Test-Path $jsonSrc) { $extra += "-DFETCHCONTENT_SOURCE_DIR_JSON=$jsonSrc" }
    if (Test-Path $stbSrc) { $extra += "-DFETCHCONTENT_SOURCE_DIR_STB=$stbSrc" }
    & $cmake -G Ninja -S (Join-Path $Repo 'device') -B $buildDir `
        -DCMAKE_TOOLCHAIN_FILE="$Repo\device\cmake\aarch64-toolchain.cmake" `
        -DCMAKE_BUILD_TYPE=Release -DCPR_ENABLE_SSL=OFF @extra `
        *> $null
    if ($LASTEXITCODE -ne 0) { Fail 'cross configure failed' }
}

Log 'PROGRESS=30 build'
# 构建输出进临时文件防污染进度日志（解析器只认 PROGRESS= 行）
$buildLog = Join-Path $env:TEMP 'dutyon-cross-build.log'
& $cmake --build $buildDir -j *> $buildLog
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bin)) {
    Get-Content $buildLog -Tail 15 | ForEach-Object { Log "  $_" }
    Fail 'cross build failed'
}

# ---- 3. 推送二进制 + wifi 脚本 + deploy.sh ----
Log 'PROGRESS=70 deploy'
scp @sshOpts $bin "${Device}:/opt/dutyon-src/device/build/dutyon-pet" *> $null
if ($LASTEXITCODE -ne 0) { Fail 'scp binary failed' }
scp @sshOpts `
    "$Repo\device\scripts\wifi-ap.sh" `
    "$Repo\device\scripts\wifi-client.sh" `
    "$Repo\device\scripts\wifi-off.sh" `
    "${Device}:/opt/dutyon-src/device/scripts/" *> $null
if ($LASTEXITCODE -ne 0) { Fail 'scp wifi scripts failed' }
ssh @sshOpts $Device 'mkdir -p /opt/dutyon-src/.userdata' *> $null
scp @sshOpts "$Repo\.userdata\deploy.sh" "${Device}:/opt/dutyon-src/.userdata/deploy.sh" *> $null
if ($LASTEXITCODE -ne 0) { Fail 'scp deploy.sh failed' }

# ---- 4. 远端部署 + 写版本哈希 + 重启 ----
if (-not $Version) { $Version = 'unknown' }
$remote = "bash /opt/dutyon-src/.userdata/deploy.sh >/tmp/deploy.log 2>&1 && printf '%s' '$Version' > /opt/dutyon/VERSION && systemctl restart dutyon && sleep 2 && systemctl is-active dutyon"
Log 'PROGRESS=90 restart'
ssh @sshOpts $Device $remote *> $null
if ($LASTEXITCODE -ne 0) {
    ssh @sshOpts $Device 'tail -15 /tmp/deploy.log' | ForEach-Object { Log "  $_" }
    Fail 'deploy/restart failed'
}

Log 'PROGRESS=100 done'
Log 'RESULT=OK'
