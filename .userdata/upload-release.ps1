# Duty On v1.1.3 release: create GitHub Release + upload local installer.
# Token comes from Windows credential store (git credential fill).
$ErrorActionPreference = 'Stop'

$owner = 'marine841023'
$repo  = 'duty-on'
$tag   = 'v1.1.3'
$exe   = 'd:\src\traeSprite\src-tauri\target\release\bundle\nsis\DutyOn_1.1.3_x64-setup.exe'

if (-not (Test-Path $exe)) { throw "installer not found: $exe" }

# 1. Extract the GitHub PAT stored by git.
$credOut = "protocol=https`nhost=github.com`n`n" | git credential fill
$tokenLine = $credOut | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
if (-not $tokenLine) { throw 'no GitHub credential found in git credential store' }
$token = $tokenLine.Substring('password='.Length).Trim()
$headers = @{ Authorization = "Bearer $token"; 'User-Agent' = 'duty-on-release-script' }

# 2. Create the release if it does not exist yet.
$relUrl = "https://api.github.com/repos/$owner/$repo/releases/tags/$tag"
try {
    $rel = Invoke-RestMethod -Uri $relUrl -Headers $headers
    Write-Host "release already exists: $($rel.html_url)"
} catch {
    $body = @{
        tag_name = $tag
        name     = 'v1.1.3'
        body     = @(
            '## What''s new',
            '',
            '### Features',
            '- Screen edge docking: drag the pet past a monitor''s left/right edge by more than 20% of the window width and it snaps into a compact status bar with a round status light (blue = idle, yellow = working, red = confirmation needed)',
            '- Multi-monitor aware docking: every monitor boundary is a valid snap target, including the boundary between two screens — when the window straddles two monitors, the one holding most of the pet wins',
            '- Context menu now opens beside the character (side chosen by which half of the screen the pet is on) and spans the full height from the character down to the IDE project list, never covering the pet',
            '- Motion menu live preview: hovering a motion plays it in a seamless loop on the character while the menu stays open; the menu only closes via the close button or clicking outside',
            '',
            '### Bug Fixes',
            '- Fixed motion preview stopping after one play — replays are now deferred past the library''s motion-slot cleanup so the loop keeps running'
        ) -join "`n"
    } | ConvertTo-Json -Depth 5
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$owner/$repo/releases" -Method Post -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([System.Text.Encoding]::UTF8.GetBytes($body))
    Write-Host "release created: $($rel.html_url)"
}

# 3. Upload the installer (replace existing asset with the same name).
$assetName = Split-Path $exe -Leaf
foreach ($a in $rel.assets) {
    if ($a.name -eq $assetName) {
        Invoke-RestMethod -Uri "https://api.github.com/repos/$owner/$repo/releases/assets/$($a.id)" -Method Delete -Headers $headers | Out-Null
        Write-Host "removed stale asset $($a.name)"
    }
}
$bytes = [System.IO.File]::ReadAllBytes($exe)
$up = Invoke-RestMethod -Uri "https://uploads.github.com/repos/$owner/$repo/releases/$($rel.id)/assets?name=$assetName" -Method Post -Headers $headers -ContentType 'application/octet-stream' -Body $bytes
Write-Host "uploaded: $($up.name) ($([math]::Round($bytes.Length/1MB,1)) MB) -> $($up.browser_download_url)"
