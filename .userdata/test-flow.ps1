<#
.SYNOPSIS
    End-to-end regression for the 5-step session lifecycle.
.DESCRIPTION
    Drives the HTTP server (127.0.0.1:17521) through:
      1. SessionStart   -> idle      (overall: sleeping)
      2. UserPromptSubmit -> working  (overall: working)
      3. Notification+tool_name -> confirmation-needed (overall: alert)
      4. Stop           -> idle      (overall: sleeping)
      5. unregister     -> cleanup
.NOTES
    Prerequisites: the pet app must already be running (`cargo tauri dev` in another
    terminal). Run from the project root:
        .\.userdata\test-flow.ps1
#>
$base = 'http://127.0.0.1:17521'
$evDir = $PSScriptRoot
function Send-Hook($file) {
    $body = Get-Content -Raw $file
    Invoke-RestMethod -Uri "$base/hook" -Method Post -Body $body -ContentType 'application/json' | Out-Null
}
function Show-Status {
    $s = Invoke-RestMethod -Uri "$base/status"
    "  overallState = $($s.overallState)  | sessions = $($s.sessions.Count)"
    foreach ($sess in $s.sessions) {
        "    - $($sess.projectName): status=$($sess.status) lastEvent=$($sess.lastEvent) alert=$($sess.alertMessage)"
    }
}

Write-Host '=== 1. SessionStart (expect: status=idle, overall=sleeping) ===' -ForegroundColor Cyan
Send-Hook "$evDir\ev-session.json"
Show-Status

Write-Host '=== 2. UserPromptSubmit (expect: status=working, overall=working) ===' -ForegroundColor Cyan
Send-Hook "$evDir\ev-working.json"
Show-Status

Write-Host '=== 3. Notification + tool_name (expect: status=confirmation-needed, overall=alert) ===' -ForegroundColor Cyan
Send-Hook "$evDir\ev-alert.json"
Show-Status

Write-Host '=== 4. Stop (expect: status=idle, overall=sleeping) ===' -ForegroundColor Cyan
Send-Hook "$evDir\ev-stop.json"
Show-Status

Write-Host '=== 5. Cleanup: unregister test-1 ===' -ForegroundColor Cyan
Invoke-RestMethod -Uri "$base/unregister" -Method Post -Body '{"session_id":"test-1"}' -ContentType 'application/json' | Out-Null
Show-Status
