﻿﻿﻿# DutyOn 2.0 安装包构建脚本
# 用法：powershell -File tools\build-package.ps1 [-Version 2.0.4]
# 产物：tools\dist\DutyOn_<版本>_x64-setup.exe + DutyOn-v<版本>.zip
# 说明：NSIS 工具链在 tools\nsis\nsis-3\（tauri 官方 GitHub 镜像的 NSIS 3
#       便携版，首次运行自动下载）；ZIP 外层包装用于规避 SmartScreen
#       对未签名 exe 的拦截（项目分发约定）。
param([string]$Version = "2.0.4")

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent       # 仓库根
$tools = $PSScriptRoot                         # tools\
$dist  = Join-Path $tools 'dist'

# --- 1. 确保 NSIS 工具链存在（便携版，不入库；zip 解压后带版本子目录，
#        如 nsis-3\nsis-3.08\，故按搜索定位 makensis）---
$makensis = Get-ChildItem (Join-Path $tools 'nsis') -Recurse -Filter makensis.exe `
    -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -notmatch '\\Bin$' } |
    Select-Object -First 1
if (-not $makensis) {
    Write-Host "[1/4] 下载 NSIS 3 便携版（tauri 官方镜像）..." -ForegroundColor Cyan
    $ns = Join-Path $tools 'nsis'
    New-Item -ItemType Directory -Force -Path $ns | Out-Null
    $zip = Join-Path $ns 'nsis-3.zip'
    & curl.exe -L --retry 3 -A "Mozilla/5.0" -o $zip "https://github.com/tauri-apps/binary-releases/releases/download/nsis-3/nsis-3.zip"
    if ((Get-Item $zip -ErrorAction SilentlyContinue).Length -lt 1MB) { throw "NSIS 下载失败" }
    Expand-Archive -Path $zip -DestinationPath (Join-Path $ns 'nsis-3') -Force
    $makensis = Get-ChildItem $ns -Recurse -Filter makensis.exe |
        Where-Object { $_.DirectoryName -notmatch '\\Bin$' } | Select-Object -First 1
    if (-not $makensis) { throw "解压后未找到 makensis.exe" }
} else {
    Write-Host "[1/4] NSIS 已就绪: $($makensis.FullName)" -ForegroundColor Cyan
}

# --- 2. 校验构建产物（先 cmake --build 再来打包）---
$exe = Join-Path $root 'device\build\Release\dutyon-pet.exe'
$glfw = Join-Path $root 'device\build\Release\glfw3.dll'
foreach ($f in @($exe, $glfw, (Join-Path $root 'frontend\assets\live2d'))) {
    if (-not (Test-Path $f)) { throw "缺少 $f —— 请先构建 Release" }
}
Write-Host "[2/4] 构建产物校验通过" -ForegroundColor Cyan

# --- 3. 编译 NSIS 安装包 ---
Write-Host "[3/4] makensis 编译安装器..." -ForegroundColor Cyan

# 3.1 NSI 编码消毒：IDE 的编码往返会把 UTF-8 BOM 反复叠加到文件头
#     （每往返一次 +1 个 BOM），makensis 解析多 BOM 直接报 line 1 错。
#     剥掉全部前导 BOM 后字节级写回单 BOM（不做文本编码往返）。
$nsi = Join-Path $tools 'packager\dutyon.nsi'
$nsiBytes = [System.IO.File]::ReadAllBytes($nsi)
$off = 0
while ($off + 2 -lt $nsiBytes.Length -and $nsiBytes[$off] -eq 0xEF -and
       $nsiBytes[$off+1] -eq 0xBB -and $nsiBytes[$off+2] -eq 0xBF) { $off += 3 }
if ($off -gt 3) {
    $clean = New-Object byte[] ($nsiBytes.Length - $off + 3)
    [Array]::Copy([byte[]](0xEF,0xBB,0xBF), 0, $clean, 0, 3)
    [Array]::Copy($nsiBytes, $off, $clean, 3, $nsiBytes.Length - $off)
    [System.IO.File]::WriteAllBytes($nsi, $clean)
    Write-Host "NSI BOM 消毒: $($off/3) 个叠加 BOM -> 1" -ForegroundColor Yellow
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null
& $makensis.FullName "/DAPP_VERSION=$Version" (Join-Path $tools 'packager\dutyon.nsi')
if ($LASTEXITCODE -ne 0) { throw "makensis 失败（exit $LASTEXITCODE）" }

# --- 4. ZIP 外包装（SmartScreen 规避，分发约定）---
Write-Host "[4/4] 生成 ZIP 分发包..." -ForegroundColor Cyan
$setupExe = Join-Path $dist "DutyOn_${Version}_x64-setup.exe"
$zipPath = Join-Path $dist "DutyOn-v${Version}.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $setupExe -DestinationPath $zipPath

""
"=== 完成 ==="
"安装器  : $setupExe ($([math]::Round((Get-Item $setupExe).Length/1MB,2)) MB)"
"分发 ZIP: $zipPath ($([math]::Round((Get-Item $zipPath).Length/1MB,2)) MB)"
