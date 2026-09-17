# DutyOn — 推送单个文件到设备（大文件走 base64 stdin，绕过 plink 命令行长度限制）
#
# 用途：plink 直接把 base64 当命令行参数传时会撞上 Windows 命令行长度上限
#   （1MB 的 wav 编码后约 1.4M 字符，报"文件名或扩展名太长"）。本脚本改为把
#   base64 灌进 plink 的 stdin，由远端 `base64 -d` 还原，无长度限制。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .userdata\push-file.ps1 `
#       -LocalFile frontend\assets\device\sounds\attention.wav `
#       -RemotePath /opt/dutyon-src/frontend/assets/device/sounds/attention.wav
#
# 凭据（不入库）：同目录 deploy.env，字段见 deploy.env.example
#   DUTYON_DEVICE=root@192.168.7.1
#   DUTYON_DEVICE_PASS=<设备SSH口令>
#
# 注意：必须用 .NET Process 直接写字节流。PowerShell 的 `|` 管道会按
#   $OutputEncoding 追加 BOM/CRLF，远端 base64 会报 "invalid input"。

param(
    [Parameter(Mandatory = $true)][string]$LocalFile,
    [Parameter(Mandatory = $true)][string]$RemotePath
)
$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $PSScriptRoot
$plink = Join-Path $Repo 'tools\plink.exe'

# ---- 凭据：只从 deploy.env 读，绝不硬编码（该文件已被 .gitignore 排除）----
$Device = $null; $Pass = $null
$envFile = Join-Path $PSScriptRoot 'deploy.env'
if (Test-Path $envFile) {
    foreach ($l in (Get-Content $envFile)) {
        if ($l -match '^\s*DUTYON_DEVICE=(.+)$') { $Device = $Matches[1].Trim() }
        elseif ($l -match '^\s*DUTYON_DEVICE_PASS=(.+)$') { $Pass = $Matches[1].Trim() }
    }
}
if (-not $Device) { $Device = 'root@192.168.7.1' }
if (-not $Pass) { throw "missing DUTYON_DEVICE_PASS in $envFile (see deploy.env.example)" }
if (-not (Test-Path $plink)) { throw "plink not found: $plink" }
if (-not (Test-Path $LocalFile)) { throw "local file not found: $LocalFile" }

$b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($LocalFile))

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $plink
$psi.Arguments = "-batch -pw $Pass $Device `"base64 -d > $RemotePath`""
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$p = [System.Diagnostics.Process]::Start($psi)

# ASCII 写字节流：base64 字母表全在 ASCII 内，不会引入 BOM
$bytes = [System.Text.Encoding]::ASCII.GetBytes($b64)
$p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
$p.StandardInput.Close()

$stdout = $p.StandardOutput.ReadToEnd()
$stderr = $p.StandardError.ReadToEnd()
$p.WaitForExit()
if ($p.ExitCode -ne 0) {
    if ($stdout) { Write-Host "STDOUT: $stdout" }
    if ($stderr) { Write-Host "STDERR: $stderr" }
    throw "plink failed rc=$($p.ExitCode)"
}
$sz = (Get-Item $LocalFile).Length
Write-Host "pushed $LocalFile ($sz bytes) -> $RemotePath"
