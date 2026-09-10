# DutyOn — 同步最新程序到设备（源码整树推送 + 设备端增量编译 + 部署重启）
#
# 由 PC 端桌宠菜单「同步程序到设备」触发，也可手动执行：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .userdata\sync-device.ps1
#
# 凭据（不入库）：同目录 deploy.env，字段见 deploy.env.example
#   DUTYON_DEVICE=root@192.168.7.1
#   DUTYON_DEVICE_PASS=<设备SSH口令>
# 无 deploy.env 时先尝试 ssh 免密；都不行则报错退出。
#
# 日志：-LogFile 指定（默认 %TEMP%\dutyon-device-sync.log），统一 UTF-8；
# 最后一行固定输出 RESULT=OK 或 RESULT=FAIL:<原因>，供调用方解析弹窗。
#
# 注意：本文件必须保存为「UTF-8 带 BOM」—— Windows PowerShell 5.1 对无
# BOM 的 .ps1 按系统 ANSI(GBK) 解码，中文字节序列会破坏字符串解析。

param(
    [string]$Repo = "",
    [string]$LogFile = ""
)

$ErrorActionPreference = 'Continue'

if (-not $Repo)    { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not $LogFile) { $LogFile = Join-Path $env:TEMP 'dutyon-device-sync.log' }

function Log([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg
    # 统一 UTF-8（Out-File/Tee-Object 默认 UTF-16，会与 cmd 追加的字节流混编）
    $line | Out-File -FilePath $LogFile -Append -Encoding utf8
    Write-Host $line
}
function Fail([string]$msg) { Log "RESULT=FAIL:$msg"; exit 1 }

"=== device sync $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" | Out-File $LogFile -Encoding utf8

# ---- 凭据 ----
$Device = $null; $Pass = $null
$envFile = Join-Path $PSScriptRoot 'deploy.env'
if (Test-Path $envFile) {
    foreach ($l in (Get-Content $envFile)) {
        if ($l -match '^\s*DUTYON_DEVICE=(.+)$')          { $Device = $Matches[1].Trim() }
        elseif ($l -match '^\s*DUTYON_DEVICE_PASS=(.+)$') { $Pass   = $Matches[1].Trim() }
    }
}
if (-not $Device) { $Device = 'root@192.168.7.1' }

# ---- 传输通道：ssh 免密优先，否则 plink -pw ----
$plink = Join-Path $Repo 'tools\plink.exe'
& ssh -o BatchMode=yes -o ConnectTimeout=4 -o StrictHostKeyChecking=no $Device "echo ok" *> $null
$usePlink = ($LASTEXITCODE -ne 0)
if ($usePlink -and (-not $Pass -or -not (Test-Path $plink))) {
    Fail "cannot connect $Device (ssh key auth failed, and missing deploy.env password or tools/plink.exe)"
}
Log "device=$Device transport=$(if ($usePlink) {'plink'} else {'ssh'}) repo=$Repo"

# 远端命令组装（只含安全字符，避免多层引号地狱）
$sshArgs = if ($usePlink) { @('-batch', '-pw', $Pass, $Device) }
           else           { @('-o', 'ConnectTimeout=8', '-o', 'StrictHostKeyChecking=no', $Device) }
$sshExe  = if ($usePlink) { $plink } else { 'ssh' }

# ---- 第 1 步：tar 推送源码包（二进制走 cmd 管道，PS5.1 管道会毁字节流）----
# 整树推 device/src：设备 CMake 是显式源文件清单，多推的 PC 端文件不参与编译
$tarPipe = "tar -cf - -C `"$Repo`" device/CMakeLists.txt device/src frontend/assets/device .userdata/deploy.sh | `"$sshExe`" $($sshArgs -join ' ') `"cat > /tmp/dutyon-sync.tar`""
Log "push source tarball..."
cmd /c "$tarPipe >> `"$LogFile`" 2>&1"
if ($LASTEXITCODE -ne 0) { Fail "source push failed (rc=$LASTEXITCODE)" }

# ---- 第 2 步：远端解包 + 增量编译 + 部署重启 ----
# 脚本 base64 化后传输，彻底规避本地 PS/远端 bash 双层引号转义问题；
# trap EXIT 兜底：任何一步失败都回传 SYNC-FAIL + 编译日志尾巴
$remoteScript = @'
trap 'RC=$?; if [ $RC -ne 0 ]; then echo SYNC-FAIL; tail -15 /tmp/sync-build.log 2>/dev/null; exit $RC; fi' EXIT
set -e
mkdir -p /opt/dutyon-src
cd /opt/dutyon-src
tar -xf /tmp/dutyon-sync.tar
rm -f /tmp/dutyon-sync.tar
sed -i 's/\r$//' .userdata/deploy.sh
cd device
: > /tmp/sync-build.log
cmake -B build -DCMAKE_BUILD_TYPE=Release >> /tmp/sync-build.log 2>&1
cmake --build build -j2 >> /tmp/sync-build.log 2>&1
bash ../.userdata/deploy.sh >> /tmp/sync-build.log 2>&1
systemctl restart dutyon
sleep 3
systemctl is-active --quiet dutyon
echo SYNC-OK
'@ -replace "`r`n", "`n"

$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remoteScript))
Log "remote build+deploy (incremental, 1-5 min typical)..."
& $sshExe @sshArgs "echo $b64 | base64 -d | bash" 2>&1 |
    Out-File -FilePath $LogFile -Append -Encoding utf8
if ($LASTEXITCODE -ne 0) { Fail "remote build/deploy failed (rc=$LASTEXITCODE)" }

# ---- 结果确认（回执丢失败判，不回传 SYNC-OK 一律算失败）----
$tail = Get-Content $LogFile -Tail 30
if ($tail -match 'SYNC-FAIL') { Fail "device reported failure" }
if (-not ($tail -match 'SYNC-OK')) { Fail "no success ack from device" }
Log "RESULT=OK"
exit 0
