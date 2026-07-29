<#
.SYNOPSIS
  TraePet 开发启动脚本
  D 盘源码 -> C 盘临时目录 -> Electron 开发模式

.DESCRIPTION
  TRAE SOLO CN 中 D 盘是虚拟化文件系统，Electron/Chromium 无法在虚拟化路径上运行。
  本脚本将 D 盘源码同步到 C 盘真实文件系统，然后启动 Electron 开发模式。

.PARAMETER Dev
  以开发模式启动（打开 DevTools）

.PARAMETER Clean
  清理 C 盘临时目录后重新同步（首次运行或依赖变更时使用）

.EXAMPLE
  .\run.ps1            # 普通启动
  .\run.ps1 -Dev       # 开发模式（带 DevTools）
  .\run.ps1 -Clean     # 清理后重新同步
  .\run.ps1 -Dev -Clean
#>

param(
  [switch]$Dev,
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

# === 路径配置 ===
$SourceDir = "D:\src\traeSprite"
$TempDir = "$env:LOCALAPPDATA\TraePet\dev"
$ElectronExe = "$TempDir\node_modules\electron\dist\electron.exe"

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "  TraePet Dev Launcher" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

# === 检查源目录 ===
if (-not (Test-Path "$SourceDir\package.json")) {
  Write-Host "[ERROR] Source not found: $SourceDir\package.json" -ForegroundColor Red
  exit 1
}

# === 清理模式 ===
if ($Clean -and (Test-Path $TempDir)) {
  Write-Host "[1/4] Cleaning temp directory..." -ForegroundColor Yellow
  Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
  Write-Host "  Cleaned: $TempDir" -ForegroundColor Green
}

# === 同步源码到 C 盘 ===
Write-Host "[1/4] Syncing source to C: drive..." -ForegroundColor Yellow

$needFullSync = -not (Test-Path "$TempDir\node_modules\electron\dist\electron.exe")

if ($needFullSync) {
  Write-Host "  First run or missing dependencies, full sync..." -ForegroundColor DarkYellow
  robocopy $SourceDir $TempDir /E /MT:16 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
} else {
  Write-Host "  Incremental sync (source files only)..." -ForegroundColor DarkYellow
  robocopy "$SourceDir\src" "$TempDir\src" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\assets" "$TempDir\assets" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\hooks" "$TempDir\hooks" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\scripts" "$TempDir\scripts" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  Copy-Item "$SourceDir\package.json" "$TempDir\package.json" -Force
}

Write-Host "  Synced to: $TempDir" -ForegroundColor Green

# === 检查 Electron 二进制 ===
Write-Host "[2/4] Checking Electron binary..." -ForegroundColor Yellow
if (-not (Test-Path $ElectronExe)) {
  Write-Host "  Electron not found, running npm install..." -ForegroundColor DarkYellow
  Push-Location $TempDir
  npm install 2>&1 | Out-Null
  Pop-Location
}
if (-not (Test-Path $ElectronExe)) {
  Write-Host "  [ERROR] Electron binary still missing after npm install!" -ForegroundColor Red
  exit 1
}
Write-Host "  Electron: OK" -ForegroundColor Green

# === 清理旧的用户数据（避免锁文件冲突）===
Write-Host "[3/4] Cleaning old user data..." -ForegroundColor Yellow
Remove-Item "$env:APPDATA\trae-pet" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:APPDATA\Electron" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:APPDATA\TraePet" -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "  Done" -ForegroundColor Green

# === 启动 Electron ===
Write-Host "[4/4] Launching Electron..." -ForegroundColor Yellow
$arguments = @(".")
if ($Dev) {
  $arguments += "--dev"
  Write-Host "  Mode: DEV (with DevTools)" -ForegroundColor Magenta
} else {
  Write-Host "  Mode: Normal" -ForegroundColor Magenta
}
Write-Host "  WorkingDir: $TempDir" -ForegroundColor DarkGray
Write-Host ""

Set-Location $TempDir
Start-Process -FilePath $ElectronExe -ArgumentList $arguments -WorkingDirectory $TempDir

Write-Host "TraePet launched! Check the bottom-right corner of your desktop." -ForegroundColor Green
Write-Host "HTTP API: http://127.0.0.1:17521/health" -ForegroundColor DarkGray