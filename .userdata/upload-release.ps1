# Duty On v1.1.4 release: create GitHub Release + upload local installer.
# Token comes from Windows credential store (git credential fill).
$ErrorActionPreference = 'Stop'

$owner = 'marine841023'
$repo  = 'duty-on'
$tag   = 'v1.1.4'
$exe   = 'd:\src\traeSprite\src-tauri\target\release\bundle\nsis\DutyOn_1.1.4_x64-setup.exe'

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
        name     = 'v1.1.4'
        body     = @(
            '## What''s new',
            '',
            '### Features',
            '- Edge-snap ghost preview: while dragging close to a screen edge, a semi-transparent dashed silhouette shows exactly where the dock bar will land — keep dragging to adjust, release to confirm',
            '- Undocking now places the character''s center right under the mouse pointer, so dragging away from the edge feels seamless',
            '',
            '### Bug Fixes',
            '- Fixed the window breaking after a DPI/scale change (remote-desktop connect/disconnect, switching monitors or display scaling): the window is now re-pinned to its intended size and kept visible on screen',
            '',
            '### Docs',
            '- New slogan across the promo page and READMEs: "Your favorite character watches your AI IDE, so you don''t have to."'
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
