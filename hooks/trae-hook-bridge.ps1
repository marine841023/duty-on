<#
.SYNOPSIS
    Trae IDE Hook Bridge - Forwards hook events to the DutyOn (开工啦) desktop application.
.DESCRIPTION
    This script is called by Trae IDE's Hook system for each AI lifecycle event.
    It reads the hook event JSON from stdin, enriches it with project information,
    and sends it to the DutyOn app's local HTTP server (http://127.0.0.1:17521/hook).
    The script is designed to be fast and non-blocking - if the pet is not running,
    it exits silently without affecting the AI's workflow.

    Diagnostic log is written to ~/.dutyon/hooks/bridge.log so we can confirm
    whether Trae IDE is actually invoking the hook.

    The -Ide argument ("trae" / "qoder") is passed by the installed hook
    command so the pet can badge each session with its source IDE.
#>

param([string]$Ide = '')

$ErrorActionPreference = 'SilentlyContinue'

# --- Diagnostic logging (never breaks the hook on failure) ---
function Write-BridgeLog($msg) {
  try {
    $logDir = Join-Path $env:USERPROFILE '.dutyon\hooks'
    $logPath = Join-Path $logDir 'bridge.log'
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
    # --- Log rotation: if the file exceeds ~500 lines, trim it down to the
    # last 250 lines so the diagnostic log can't grow unbounded across
    # many hook invocations. `@(...)` wraps so an empty file reports Count 0.
    if (Test-Path $logPath) {
      $lines = @(Get-Content -Path $logPath -ErrorAction SilentlyContinue)
      if ($lines.Count -gt 500) {
        $lines[-250..-1] | Set-Content -Path $logPath -Encoding UTF8 -ErrorAction SilentlyContinue
      }
    }
    $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    Add-Content -Path $logPath -Value "[$ts] $msg" -Encoding UTF8
  } catch { }
}

Write-BridgeLog "INVOKED pid=$PID"

# Read JSON from stdin as raw bytes and decode as UTF-8. The IDEs emit UTF-8
# JSON, but [Console]::In.ReadToEnd() decodes with the console code page (GBK
# on zh-CN Windows), which mangles any non-ASCII content (Chinese user names,
# prompts, tool output) and makes ConvertFrom-Json fail on every such event.
$stdinText = ''
try {
  $stdIn = [Console]::OpenStandardInput()
  $ms = New-Object System.IO.MemoryStream
  $stdIn.CopyTo($ms)
  $stdinText = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
} catch {
  Write-BridgeLog "binary stdin read failed: $($_.Exception.Message); falling back to text read"
  $stdinText = [Console]::In.ReadToEnd()
}
Write-BridgeLog "stdin: $stdinText"

if ([string]::IsNullOrWhiteSpace($stdinText)) {
  Write-BridgeLog "empty stdin, exit"
  exit 0
}

try {
  $event = $stdinText | ConvertFrom-Json
} catch {
  Write-BridgeLog "JSON parse failed: $_"
  exit 0
}

Write-BridgeLog "event=$($event.hook_event_name) session=$($event.session_id)"

# Get project information from environment
# QODER_PROJECT_DIR is set when invoked by Qoder's hook runner; TRAE_PROJECT_DIR
# / CLAUDE_PROJECT_DIR when invoked by Trae IDE. Fall back to the event's cwd.
$projectDir = $env:TRAE_PROJECT_DIR
if ([string]::IsNullOrEmpty($projectDir)) {
  $projectDir = $env:QODER_PROJECT_DIR
}
if ([string]::IsNullOrEmpty($projectDir)) {
  $projectDir = $env:CLAUDE_PROJECT_DIR
}
if ([string]::IsNullOrEmpty($projectDir)) {
  $projectDir = $event.cwd
}

$projectName = ''
if (-not [string]::IsNullOrEmpty($projectDir)) {
  $projectName = Split-Path $projectDir -Leaf
}

# IDE kind: the explicit -Ide argument wins; fall back to the runner's env
# marker (covers legacy hook commands installed without the argument).
if ([string]::IsNullOrEmpty($Ide)) {
  if (-not [string]::IsNullOrEmpty($env:QODER_PROJECT_DIR)) {
    $Ide = 'qoder'
  } elseif (-not [string]::IsNullOrEmpty($env:TRAE_PROJECT_DIR) -or -not [string]::IsNullOrEmpty($env:CLAUDE_PROJECT_DIR)) {
    $Ide = 'trae'
  }
}

# Enrich the event with project info
$event | Add-Member -NotePropertyName "project_path" -NotePropertyValue $projectDir -Force
$event | Add-Member -NotePropertyName "project_name" -NotePropertyValue $projectName -Force
if (-not [string]::IsNullOrEmpty($Ide)) {
  $event | Add-Member -NotePropertyName "ide" -NotePropertyValue $Ide -Force
}

# Add timestamp
$event | Add-Member -NotePropertyName "timestamp" -NotePropertyValue ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) -Force

# Convert back to JSON for sending
$bodyJson = $event | ConvertTo-Json -Depth 10 -Compress

# Send to DutyOn's HTTP server (with short timeout to avoid blocking AI)
try {
  $response = Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' `
    -Method Post `
    -Body $bodyJson `
    -ContentType 'application/json' `
    -TimeoutSec 2 `
    -ErrorAction Stop
  Write-BridgeLog "POST ok"
} catch {
  Write-BridgeLog "POST failed: $($_.Exception.Message)"
  # Pet not running or error - exit silently, don't affect AI workflow
  exit 0
}

# Exit successfully with no stdout output (to not interfere with AI)
exit 0
