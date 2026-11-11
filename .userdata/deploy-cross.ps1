# deploy-cross.ps1 —— 交叉编译产物手动部署到设备
# 用法: powershell -File .userdata\deploy-cross.ps1 [-Device root@192.168.7.1]
# 前提: device/build-cross/dutyon-pet 已本地构建（见 cmake/aarch64-toolchain.cmake 注释）
# 流程: scp 二进制 → 设备端 deploy.sh(资产+安装+WiFi脚本) → 重启服务 → 回显日志
param([string]$Device = 'root@192.168.7.1', [string]$Repo = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
$sshOpts = @('-o','BatchMode=yes','-o','ConnectTimeout=8','-o','StrictHostKeyChecking=no')
$bin = Join-Path $Repo 'device\build-cross\dutyon-pet'
if (-not (Test-Path $bin)) { Write-Host "RESULT=FAIL:missing $bin (先本地交叉构建)"; exit 1 }

Write-Host "[1/3] push binary + wifi scripts + deploy.sh..."
scp @sshOpts $bin "${Device}:/opt/dutyon-src/device/build/dutyon-pet"
if ($LASTEXITCODE -ne 0) { Write-Host "RESULT=FAIL:scp push failed"; exit 1 }
# Wi-Fi 脚本（共享锁、热点兼容性等）与 deploy.sh 需随二进制同步
scp @sshOpts "${Repo}\device\scripts\wifi-common.sh" "${Repo}\device\scripts\wifi-ap.sh" "${Repo}\device\scripts\wifi-client.sh" "${Repo}\device\scripts\wifi-off.sh" "${Repo}\device\scripts\arp-keepalive.sh" "${Device}:/opt/dutyon-src/device/scripts/"
if ($LASTEXITCODE -ne 0) { Write-Host "RESULT=FAIL:scp wifi scripts failed"; exit 1 }
ssh @sshOpts $Device "mkdir -p /opt/dutyon-src/.userdata"
scp @sshOpts "${Repo}\.userdata\deploy.sh" "${Device}:/opt/dutyon-src/.userdata/deploy.sh"
if ($LASTEXITCODE -ne 0) { Write-Host "RESULT=FAIL:scp deploy.sh failed"; exit 1 }

Write-Host "[2/3] remote deploy + restart..."
ssh @sshOpts $Device "bash /opt/dutyon-src/.userdata/deploy.sh >/tmp/deploy.log 2>&1 && systemctl restart dutyon && sleep 2 && systemctl is-active dutyon"
if ($LASTEXITCODE -ne 0) {
    ssh @sshOpts $Device "tail -15 /tmp/deploy.log"
    Write-Host "RESULT=FAIL:deploy/restart failed"
    exit 1
}

Write-Host "[3/3] service log (last 25 lines):"
ssh @sshOpts $Device "journalctl -u dutyon -n 25 --no-pager"
Write-Host "RESULT=OK"
