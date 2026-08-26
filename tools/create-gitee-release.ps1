# Create Gitee releases with attachments via API v5 (direct connection).
# ASCII-only script body; release notes are read from UTF-8 files.
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# NB: piping the query from nested powershell mangles line endings for
# credential helpers (GCM sees no protocol field); use a temp file + cmd
# redirect instead.
$credQuery = Join-Path $env:TEMP 'dutyon-cred-gitee.txt'
[System.IO.File]::WriteAllText($credQuery, "protocol=https`nhost=gitee.com`n")
$fill = cmd /c "git credential fill < `"$credQuery`"" 2>$null
$token = ($fill | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
Remove-Item $credQuery -ErrorAction SilentlyContinue
if (-not $token) { throw 'no credential token for gitee.com' }

$api = 'https://gitee.com/api/v5/repos/megrezsoft/dutyo'

$releases = @(
  @{ Tag = 'v2.0.4'
     Notes = 'D:\src\traeSprite\docs\release-notes\v2.0.4.md'
     Zip   = 'D:\src\traeSprite\tools\dist\DutyOn-v2.0.4.zip' }
)

$curl = 'C:\Windows\System32\curl.exe'
if (-not (Test-Path $curl)) { throw 'curl.exe not found' }

foreach ($r in $releases) {
  $lines = Get-Content -LiteralPath $r.Notes -Encoding UTF8
  $bodyText = (($lines | Where-Object { $_ -notmatch '^# ' }) -join "`n")
  # release name = the H1 line from the notes file (runtime UTF-8 read is safe)
  $relName = ($lines | Where-Object { $_ -match '^# ' } | Select-Object -First 1) -replace '^# ', ''
  if (-not $relName) { $relName = $r.Tag }

  # check existing release for this tag: GET /releases/tags/{tag}
  $code = & $curl -s -o "$env:TEMP\gitee_rel.json" -w '%{http_code}' `
    "$api/releases/tags/$($r.Tag)?access_token=$token"
  $relId = $null
  if ($code -eq '200') {
    # Gitee returns literal "null" (200) when the tag has no release yet
    $rawRel = Get-Content "$env:TEMP\gitee_rel.json" -Raw -Encoding UTF8
    if ($rawRel -and $rawRel.Trim() -ne 'null') {
      $relId = ($rawRel | ConvertFrom-Json).id
    }
  }
  if ($relId) {
    Write-Output "[$($r.Tag)] release already exists (id $relId) - reusing"
  } else {
    $sha = (git rev-list -n 1 $r.Tag | Select-Object -First 1).Trim()
    $payload = @{
      access_token      = $token
      tag_name          = $r.Tag
      name              = $relName
      target_commitish  = $sha
      body         = $bodyText
      prerelease   = $false
    } | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllBytes("$env:TEMP\gitee_body.json", [System.Text.Encoding]::UTF8.GetBytes($payload))
    $code = & $curl -s -o "$env:TEMP\gitee_rel.json" -w '%{http_code}' -X POST `
      "$api/releases" -H 'Content-Type: application/json' --data-binary "@$env:TEMP\gitee_body.json"
    if ($code -ne '201') {
      Write-Output "[$($r.Tag)] create failed HTTP $code"
      Get-Content "$env:TEMP\gitee_rel.json" -Raw
      throw 'release create failed'
    }
    $relId = (Get-Content "$env:TEMP\gitee_rel.json" -Raw -Encoding UTF8 | ConvertFrom-Json).id
    Write-Output "[$($r.Tag)] release created (id $relId)"
  }

  # attach zip (multipart)
  $code = & $curl -s -o "$env:TEMP\gitee_att.json" -w '%{http_code}' -X POST `
    "$api/releases/$relId/attach_files?access_token=$token" -F "file=@$($r.Zip);type=application/zip"
  if ($code -eq '200' -or $code -eq '201') {
    Write-Output "[$($r.Tag)] attachment uploaded: $(Split-Path -Leaf $r.Zip) ($([math]::Round((Get-Item $r.Zip).Length/1MB,2)) MB)"
  } else {
    Write-Output "[$($r.Tag)] attach failed HTTP $code"
    Get-Content "$env:TEMP\gitee_att.json" -Raw
    throw 'attach failed'
  }
}
Write-Output 'DONE'
