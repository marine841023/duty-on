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
$fill = "protocol=https`nhost=gitee.com`n" | git credential fill
$token = ($fill | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
$ge = Invoke-RestMethod -Uri "https://gitee.com/api/v5/repos/megrezsoft/dutyo/releases?access_token=$token&page=1&per_page=20&direction=desc"
foreach ($r in $ge) {
  $assets = ($r.assets | ForEach-Object { $_.name }) -join ', '
  Write-Output ("{0} | {1} | assets: {2}" -f $r.tag_name, $r.name, $assets)
}
