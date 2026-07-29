<#
.SYNOPSIS
  TraePet 打包脚本
  在 C 盘真实文件系统上执行 electron-builder 打包，完成后将 EXE 复制回 D 盘

.DESCRIPTION
  TRAE SOLO CN 中 D 盘是虚拟化文件系统，electron-builder 无法在虚拟化路径上正常打包。
  本脚本将 D 盘源码同步到 C 盘，在 C 盘执行打包，然后将生成的 EXE 复制回 D 盘 release 目录。

.PARAMETER Clean
  清理 C 盘临时目录后重新全量同步（依赖变更时使用）

.EXAMPLE
  .\dist.ps1            # 增量同步并打包
  .\dist.ps1 -Clean     # 全量同步后打包
#>

param(
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

# === 路径配置 ===
$SourceDir = "D:\src\traeSprite"
$TempDir = "$env:LOCALAPPDATA\TraePet\build"
$ElectronExe = "$TempDir\node_modules\electron\dist\electron.exe"

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "  TraePet Build Script" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

# === 检查源目录 ===
if (-not (Test-Path "$SourceDir\package.json")) {
  Write-Host "[ERROR] Source not found: $SourceDir\package.json" -ForegroundColor Red
  exit 1
}

# === 清理模式 ===
if ($Clean -and (Test-Path $TempDir)) {
  Write-Host "[1/5] Cleaning build directory..." -ForegroundColor Yellow
  Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
  Write-Host "  Cleaned: $TempDir" -ForegroundColor Green
}

# === 同步源码到 C 盘 ===
Write-Host "[1/5] Syncing source to C: drive..." -ForegroundColor Yellow

$needFullSync = -not (Test-Path "$TempDir\node_modules\electron\dist\electron.exe") -or -not (Test-Path "$TempDir\node_modules\electron-builder")

if ($needFullSync) {
  Write-Host "  Full sync (first run or missing deps)..." -ForegroundColor DarkYellow
  robocopy $SourceDir $TempDir /E /MT:16 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
} else {
  Write-Host "  Incremental sync (source files only)..." -ForegroundColor DarkYellow
  robocopy "$SourceDir\src" "$TempDir\src" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\assets" "$TempDir\assets" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\hooks" "$TempDir\hooks" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\build" "$TempDir\build" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  robocopy "$SourceDir\scripts" "$TempDir\scripts" /E /MT:8 /R:1 /W:1 /NP /NFL /NDL /NJH | Out-Null
  Copy-Item "$SourceDir\package.json" "$TempDir\package.json" -Force
}

Write-Host "  Synced to: $TempDir" -ForegroundColor Green

# === 检查依赖 ===
Write-Host "[2/5] Checking dependencies..." -ForegroundColor Yellow
if (-not (Test-Path $ElectronExe) -or -not (Test-Path "$TempDir\node_modules\electron-builder")) {
  Write-Host "  Running npm install..." -ForegroundColor DarkYellow
  Push-Location $TempDir
  npm install 2>&1 | Out-Null
  Pop-Location
}
Write-Host "  Dependencies: OK" -ForegroundColor Green

# === 清理旧的 release ===
Write-Host "[3/5] Cleaning old release..." -ForegroundColor Yellow
Remove-Item "$TempDir\release" -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "  Done" -ForegroundColor Green

# === 执行打包 ===
Write-Host "[4/5] Building portable EXE..." -ForegroundColor Yellow
Push-Location $TempDir
$env:CSC_LINK = ""
$env:CSC_KEY_PASSWORD = ""
npx electron-builder --win portable 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
$buildExit = $LASTEXITCODE
Pop-Location

if ($buildExit -ne 0) {
  Write-Host "  [ERROR] Build failed with exit code $buildExit" -ForegroundColor Red
  exit $buildExit
}

# === 复制结果到 D 盘 ===
Write-Host "[5/5] Copying EXE to D: drive..." -ForegroundColor Yellow
$releaseDir = "$SourceDir\release"
if (-not (Test-Path $releaseDir)) {
  New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
}
Copy-Item "$TempDir\release\*" -Destination $releaseDir -Recurse -Force

$exeFile = Get-ChildItem "$releaseDir" -Filter "*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($exeFile) {
  Write-Host ""
  Write-Host "=========================================" -ForegroundColor Green
  Write-Host "  BUILD SUCCESS!" -ForegroundColor Green
  Write-Host "=========================================" -ForegroundColor Green
  Write-Host "  EXE: $($exeFile.FullName)" -ForegroundColor White
  Write-Host "  Size: $([math]::Round($exeFile.Length/1MB,1)) MB" -ForegroundColor White
  Write-Host ""
} else {
  Write-Host "  [WARNING] No EXE found in release directory" -ForegroundColor Yellow
}