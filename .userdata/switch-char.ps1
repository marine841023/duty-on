param([string]$Id)
$p = Join-Path $env:USERPROFILE '.dutyon\config.json'
$j = Get-Content $p -Raw | ConvertFrom-Json
$j.activeCharacterId = $Id
$s = $j | ConvertTo-Json -Depth 20
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($p, $s, $utf8)
Write-Host ("SET " + $Id)
