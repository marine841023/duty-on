# White-list Notification classification test.
# Verifies StateManager::checkConfirmationNeeded: task_complete -> idle,
# ambiguous -> idle, tool_name -> alert.
# Prerequisites: dutyon-pet.exe must already be running (embedded HTTP server
# on 127.0.0.1:17521). Run from the project root:
#   .\.userdata\test-notification.ps1
$base = 'http://127.0.0.1:17521'
function Send-Json($obj) {
    $body = $obj | ConvertTo-Json -Compress
    Invoke-RestMethod -Uri "$base/hook" -Method Post -Body $body -ContentType 'application/json' | Out-Null
}
function Show-Status {
    $s = Invoke-RestMethod -Uri "$base/status"
    "  overall=$($s.overallState) sessions=$($s.sessions.Count)"
    foreach ($sess in $s.sessions) {
        "    - $($sess.projectName): status=$($sess.status) lastEvent=$($sess.lastEvent)"
    }
}

$sid = 'test-notify'
$proj = @{ session_id=$sid; project_path='D:\proj'; project_name='proj' }

Write-Host '=== A. SessionStart -> expect idle ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='SessionStart' })
Show-Status

Write-Host '=== B. UserPromptSubmit -> expect working ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='UserPromptSubmit' })
Show-Status

Write-Host '=== C. Notification + notification_type=task_complete (while working) -> expect idle ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='Notification'; notification_type='task_complete' })
Show-Status

Write-Host '=== D. UserPromptSubmit -> expect working ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='UserPromptSubmit' })
Show-Status

Write-Host '=== E. Notification with no features (ambiguous) -> expect idle (default false) ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='Notification' })
Show-Status

Write-Host '=== F. UserPromptSubmit -> expect working ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='UserPromptSubmit' })
Show-Status

Write-Host '=== G. Notification + tool_name -> expect alert ===' -ForegroundColor Cyan
Send-Json ($proj + @{ hook_event_name='Notification'; tool_name='WriteFile' })
Show-Status

Write-Host '=== H. Cleanup ===' -ForegroundColor Cyan
Invoke-RestMethod -Uri "$base/unregister" -Method Post -Body (@{session_id=$sid}|ConvertTo-Json -Compress) -ContentType 'application/json' | Out-Null
Show-Status
