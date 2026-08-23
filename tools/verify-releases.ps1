# Verify releases on GitHub (via proxy) and Gitee (direct).
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

Write-Output '=== GitHub ==='
$gh = Invoke-RestMethod -Uri 'https://api.github.com/repos/marine841023/duty-on/releases?per_page=3' `
  -Proxy 'http://127.0.0.1:7000' -Headers @{ 'User-Agent' = 'dutyon-verify' }
foreach ($r in $gh) {
  $assets = ($r.assets | ForEach-Object { "$($_.name)[$($_.state)]" }) -join ', '
  Write-Output ("{0} | {1} | assets: {2}" -f $r.tag_name, $r.name, $assets)
}

Write-Output '=== Gitee ==='
# NB: same nested-powershell pipe mangling as the release scripts - use a
# temp file + cmd redirect for the credential query.
$credQuery = Join-Path $env:TEMP 'dutyon-cred-gitee.txt'
[System.IO.File]::WriteAllText($credQuery, "protocol=https`nhost=gitee.com`n")
$fill = cmd /c "git credential fill < `"$credQuery`"" 2>$null
$token = ($fill | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
Remove-Item $credQuery -ErrorAction SilentlyContinue
$ge = Invoke-RestMethod -Uri "https://gitee.com/api/v5/repos/megrezsoft/dutyo/releases?access_token=$token&page=1&per_page=20&direction=desc"
foreach ($r in $ge) {
  $assets = ($r.assets | ForEach-Object { $_.name }) -join ', '
  Write-Output ("{0} | {1} | assets: {2}" -f $r.tag_name, $r.name, $assets)
}
