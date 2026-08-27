# Create GitHub releases with assets via REST API (runs under proxy).
# ASCII-only script body; release notes are read from UTF-8 files.
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$proxy = 'http://127.0.0.1:7000'

# -- token from git credential store (never hardcode) --
# NB: piping the query from nested powershell mangles line endings for
# credential helpers (GCM sees no protocol field); use a temp file +
# Start-Process stdin redirect instead (cmd /c is blocked by policy).
$credQuery = Join-Path $env:TEMP 'dutyon-cred-github.txt'
$credOut = Join-Path $env:TEMP 'dutyon-cred-github-out.txt'
[System.IO.File]::WriteAllText($credQuery, "protocol=https`nhost=github.com`n")
Start-Process git -ArgumentList 'credential','fill' -RedirectStandardInput $credQuery `
  -RedirectStandardOutput $credOut -NoNewWindow -Wait | Out-Null
$fill = Get-Content $credOut
Remove-Item $credQuery, $credOut -ErrorAction SilentlyContinue
$token = ($fill | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
if (-not $token) { throw 'no credential token for github.com' }

$headers = @{
  'Authorization'       = "Bearer $token"
  'Accept'              = 'application/vnd.github+json'
  'X-GitHub-Api-Version' = '2022-11-28'
  'User-Agent'          = 'dutyon-release-script'
}

$repo = 'marine841023/duty-on'
$api  = "https://api.github.com/repos/$repo"
$up   = "https://uploads.github.com/repos/$repo"

$releases = @(
  @{ Tag = 'v2.0.4'; Name = 'DutyOn v2.0.4 - Closed projects vanish in seconds'
     Notes = 'D:\src\traeSprite\docs\release-notes\v2.0.4.md'
     Zip   = 'D:\src\traeSprite\tools\dist\DutyOn-v2.0.4.zip'
     Latest = $true }
)

foreach ($r in $releases) {
  # notes body: drop the leading H1 (release name already covers it)
  $lines = Get-Content -LiteralPath $r.Notes -Encoding UTF8
  $bodyText = (($lines | Where-Object { $_ -notmatch '^# ' }) -join "`n")

  # reuse existing release for the tag if present, else create
  try {
    $rel = Invoke-RestMethod -Uri "$api/releases/tags/$($r.Tag)" -Headers $headers -Proxy $proxy -Method Get
    Write-Output "[$($r.Tag)] release already exists (id $($rel.id)) - reusing"
  } catch {
    $payload = @{
      tag_name   = $r.Tag
      name       = $r.Name
      body       = $bodyText
      draft      = $false
      prerelease = $false
      make_latest = if ($r.Latest) { 'true' } else { 'false' }
    } | ConvertTo-Json -Depth 4
    $jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
    $rel = Invoke-RestMethod -Uri "$api/releases" -Headers $headers -Proxy $proxy `
      -Method Post -ContentType 'application/json; charset=utf-8' -Body $jsonBytes
    Write-Output "[$($r.Tag)] release created (id $($rel.id))"
  }

  # upload zip asset unless one with the same name exists
  $zipName = Split-Path -Leaf $r.Zip
  $existing = $rel.assets | Where-Object { $_.name -eq $zipName }
  if ($existing) {
    Write-Output "[$($r.Tag)] asset $zipName already uploaded - skipping"
  } else {
    Invoke-RestMethod -Uri "$up/releases/$($rel.id)/assets?name=$zipName" `
      -Headers $headers -Proxy $proxy -Method Post `
      -ContentType 'application/zip' -InFile $r.Zip | Out-Null
    Write-Output "[$($r.Tag)] asset uploaded: $zipName ($([math]::Round((Get-Item $r.Zip).Length/1MB,2)) MB)"
  }
}
Write-Output 'DONE'
