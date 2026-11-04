<#
.SYNOPSIS
    Trae IDE Hook Bridge - Forwards hook events to the DutyOn (开工啦) desktop application.
.DESCRIPTION
    This script is called by Trae/Qoder/Cursor's Hook system for each AI
    lifecycle event. It reads the hook event JSON from stdin, enriches it
    with project information, and sends it to the DutyOn app's local HTTP
    server (http://127.0.0.1:17521/hook). The script is designed to be fast
    and non-blocking - if the pet is not running, it exits silently without
    affecting the AI's workflow.

    Diagnostic log is written to ~/.dutyon/hooks/bridge.log so we can confirm
    whether the IDE is actually invoking the hook.

    The -Ide argument ("trae" / "qoder" / "cursor" / "codex") is passed by the
installed hook command so the pet can badge each session with its source
    IDE. Cursor's stdin payload carries no event name, so its installed
    commands also pass -HookEvent (normalized to the canonical
    hook_event_name). The parameter must NOT be named `$Event` — that name
    broke JSON parsing in field testing (every event degraded to a String).
#>

param([string]$Ide = '', [string]$HookEvent = '')

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
  # Bound stdin + HTTP time to fit Codex Interrupt/SessionEnd's 3s maximum.
  # Use async copy with a 500ms timeout — if the parent process (IDE hook
  # runner) doesn't close stdin (e.g. Trae IDE bug or orphaned process),
  # CopyTo would block forever, leaking a powershell process and locking
  # bridge.log. Drop incomplete input when this deadline expires.
  $copyTask = $stdIn.CopyToAsync($ms)
  if (-not $copyTask.Wait(500)) {
    Write-BridgeLog 'stdin read timeout; dropping incomplete event'
    try { $stdIn.Close() } catch { }
    exit 0
  }
  $stdinText = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
} catch {
  Write-BridgeLog 'stdin read failed'
  exit 0
}
# Cursor writes its JSON with a UTF-8 BOM, which ConvertFrom-Json rejects
# ("invalid JSON primitive"); strip it before parsing.
$stdinText = $stdinText.TrimStart([char]0xFEFF)

if ([string]::IsNullOrWhiteSpace($stdinText)) {
  Write-BridgeLog "empty stdin, exit"
  exit 0
}

try {
  $event = $stdinText | ConvertFrom-Json -ErrorAction Stop
  if ($null -eq $event -or $event -isnot [System.Management.Automation.PSCustomObject]) {
    exit 0
  }
} catch {
  Write-BridgeLog 'JSON parse failed'
  exit 0
}

Write-BridgeLog "event=$($event.hook_event_name) session=$($event.session_id)"

# Cursor's stdin payload carries its own hook_event_name, but in camelCase
# (postToolUse, stop, ...), which the state machine does not match. The
# installed hook command bakes the event in via -HookEvent; always override
# with the canonical PascalCase name.
if (-not [string]::IsNullOrWhiteSpace($HookEvent)) {
  $normalized = switch ($HookEvent) {
    'sessionStart'       { 'SessionStart' }
    'beforeSubmitPrompt' { 'UserPromptSubmit' }
    'preToolUse'         { 'PreToolUse' }
    'postToolUse'        { 'PostToolUse' }
    'stop'               { 'Stop' }
    default              { $HookEvent }
  }
  $event | Add-Member -NotePropertyName 'hook_event_name' -NotePropertyValue $normalized -Force
}
# Cursor keys sessions by conversation_id; the state machine keys by
# session_id, so backfill to keep a session's events correlatable.
if ($Ide -eq 'cursor' -and [string]::IsNullOrWhiteSpace($event.session_id)) {
  $sid = $event.conversation_id
  $event | Add-Member -NotePropertyName 'session_id' -NotePropertyValue $sid -Force
}
# Cursor tool payloads may nest the tool name (toolInfo.name / tool.name);
# the state machine reads tool_name for the AskUserQuestion alert signal, so
# normalize whatever shape arrives.
if ($Ide -eq 'cursor' -and [string]::IsNullOrWhiteSpace($event.tool_name)) {
  $tn = $event.toolInfo.name
  if ([string]::IsNullOrWhiteSpace($tn)) { $tn = $event.tool.name }
  if (-not [string]::IsNullOrWhiteSpace($tn)) {
    $event | Add-Member -NotePropertyName 'tool_name' -NotePropertyValue $tn -Force
  }
}

# Payload is authoritative. Other IDEs' inherited environment must not steal
# a Codex session's project. Only consult this IDE's environment as a fallback.
$projectDir = $event.project_path
if ([string]::IsNullOrWhiteSpace($projectDir)) { $projectDir = $event.cwd }
if ([string]::IsNullOrEmpty($projectDir)) {
  # Cursor payloads may expose the workspace as workspace_roots instead of cwd.
  $roots = $event.workspace_roots
  if ($roots -and @($roots).Count -gt 0) { $projectDir = @($roots)[0] }
}
if ([string]::IsNullOrWhiteSpace($projectDir)) {
  $projectDir = switch ($Ide) {
    'codex' { $env:CODEX_PROJECT_DIR }
    'cursor' { $env:CURSOR_PROJECT_DIR }
    'qoder' { $env:QODER_PROJECT_DIR }
    default {
      if ($env:TRAE_PROJECT_DIR) { $env:TRAE_PROJECT_DIR }
      elseif ($env:CLAUDE_PROJECT_DIR) { $env:CLAUDE_PROJECT_DIR }
      elseif (-not $Ide) { $env:QODER_PROJECT_DIR }
    }
  }
}

$projectName = ''
if (-not [string]::IsNullOrEmpty($projectDir)) {
  $projectName = ($projectDir.TrimEnd('\', '/') -split '[\\/]')[-1]
}
if ($Ide -eq 'cursor') {
  Write-BridgeLog "cursor normalized event=$($event.hook_event_name) tool=$($event.tool_name) session=$($event.session_id) project=$projectName"
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

# Reject uncorrelatable payloads rather than merging them into a fake session.
if ([string]::IsNullOrWhiteSpace($event.session_id) -or
    [string]::IsNullOrWhiteSpace($event.hook_event_name)) { exit 0 }

# Forward status metadata only. Prompts, tool inputs and tool output are not
# needed by the pet and must not end up in its diagnostic log.
$payload = @{}
foreach ($key in @('session_id', 'hook_event_name', 'project_path', 'project_name',
                  'cwd', 'ide', 'timestamp', 'tool_name', 'tool_use_id',
                  'notification_type', 'message', 'source', 'turn_id')) {
  if ($null -ne $event.$key) { $payload[$key] = $event.$key }
}
$bodyJson = $payload | ConvertTo-Json -Depth 4 -Compress
$bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)

# Send to DutyOn's HTTP server (with short timeout to avoid blocking AI)
try {
  $response = Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' `
    -Method Post `
    -Body $bodyBytes `
    -ContentType 'application/json; charset=utf-8' `
    -TimeoutSec 1 `
    -ErrorAction Stop
  Write-BridgeLog "POST ok"
} catch {
  Write-BridgeLog 'POST failed (pet unavailable or hook rejected)'
  # Pet not running or error - exit silently, don't affect AI workflow
  exit 0
}

# Exit successfully with no stdout output (to not interfere with AI)
exit 0
