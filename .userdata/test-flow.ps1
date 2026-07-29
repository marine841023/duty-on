$base = 'http://127.0.0.1:17521'
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
Send-Hook 'd:\src\traeSprite\.userdata\ev-session.json'
Show-Status

Write-Host '=== 2. UserPromptSubmit (expect: status=working, overall=working) ===' -ForegroundColor Cyan
Send-Hook 'd:\src\traeSprite\.userdata\ev-working.json'
Show-Status

Write-Host '=== 3. Notification + tool_name (expect: status=confirmation-needed, overall=alert) ===' -ForegroundColor Cyan
Send-Hook 'd:\src\traeSprite\.userdata\ev-alert.json'
Show-Status

Write-Host '=== 4. Stop (expect: status=idle, overall=sleeping) ===' -ForegroundColor Cyan
Send-Hook 'd:\src\traeSprite\.userdata\ev-stop.json'
Show-Status

Write-Host '=== 5. Cleanup: unregister test-1 ===' -ForegroundColor Cyan
Invoke-RestMethod -Uri "$base/unregister" -Method Post -Body '{"session_id":"test-1"}' -ContentType 'application/json' | Out-Null
Show-Status
