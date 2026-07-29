<#
.SYNOPSIS
    Standalone installer for Trae Pet Hook integration.
.DESCRIPTION
    Copies the hook bridge script and creates the hooks.json configuration
    for Trae IDE. Can be run independently of the Electron app.
.EXAMPLE
    .\install-hooks.ps1
#>

$ErrorActionPreference = 'Stop'

$userHome = $env:USERPROFILE
$hookDir = Join-Path $userHome ".trae-pet\hooks"
$bridgeSrc = Join-Path $PSScriptRoot "trae-hook-bridge.ps1"
$bridgeDst = Join-Path $hookDir "trae-hook-bridge.ps1"
$traeHooksPath = Join-Path $userHome ".trae-cn\hooks.json"

Write-Host "=== Trae Pet Hook Installer ===" -ForegroundColor Cyan

# 1. Create hook directory and copy bridge script
Write-Host "[1/3] Installing bridge script..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $hookDir | Out-Null
Copy-Item -Path $bridgeSrc -Destination $bridgeDst -Force
Write-Host "  Bridge script: $bridgeDst" -ForegroundColor Green

# 2. Read existing hooks.json or create new
Write-Host "[2/3] Configuring hooks.json..." -ForegroundColor Yellow

$traeHooksDir = Split-Path $traeHooksPath -Parent
New-Item -ItemType Directory -Force -Path $traeHooksDir | Out-Null

$existingHooks = $null
if (Test-Path $traeHooksPath) {
    try {
        $rawContent = Get-Content $traeHooksPath -Raw
        $existingHooks = $rawContent | ConvertFrom-Json
        Write-Host "  Found existing hooks.json, merging..." -ForegroundColor Gray
    } catch {
        Write-Host "  Existing hooks.json is invalid, creating new one..." -ForegroundColor Gray
    }
}

if ($null -eq $existingHooks) {
    $existingHooks = [PSCustomObject]@{
        version = 1
        hooks = [PSCustomObject]@{}
    }
}

# Build hook command
$hookCommand = "& `"$bridgeDst`""

# List of events to hook
$events = @('SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PostToolUse', 'Stop', 'Notification')

foreach ($eventName in $events) {
    # Check if this event already has hooks
    $existingArray = $existingHooks.hooks.$eventName

    # Filter out any existing trae-pet hooks
    $filteredArray = @()
    if ($existingArray) {
        foreach ($group in $existingArray) {
            $hasTraePet = $false
            if ($group.hooks) {
                foreach ($h in $group.hooks) {
                    if ($h.command -and $h.command -like '*.trae-pet*') {
                        $hasTraePet = $true
                        break
                    }
                }
            }
            if (-not $hasTraePet) {
                $filteredArray += $group
            }
        }
    }

    # Add our hook
    $newHookGroup = [PSCustomObject]@{
        hooks = @(
            [PSCustomObject]@{
                type = 'command'
                command = $hookCommand
                timeout = 5
            }
        )
    }

    $filteredArray += $newHookGroup

    # Update the hooks object
    if ($existingHooks.hooks.PSObject.Properties[$eventName]) {
        $existingHooks.hooks.$eventName = $filteredArray
    } else {
        $existingHooks.hooks | Add-Member -NotePropertyName $eventName -NotePropertyValue $filteredArray
    }
}

# Write the updated hooks.json
$jsonOutput = $existingHooks | ConvertTo-Json -Depth 10
Set-Content -Path $traeHooksPath -Value $jsonOutput -Encoding UTF8
Write-Host "  Hooks config: $traeHooksPath" -ForegroundColor Green

# 3. Verify
Write-Host "[3/3] Verifying installation..." -ForegroundColor Yellow
$bridgeExists = Test-Path $bridgeDst
$hooksContent = Get-Content $traeHooksPath -Raw
$hooksValid = $hooksContent -like '*.trae-pet*'

if ($bridgeExists -and $hooksValid) {
    Write-Host ""
    Write-Host "=== Installation Successful! ===" -ForegroundColor Green
    Write-Host "Trae IDE hooks are now configured." -ForegroundColor Green
    Write-Host "Restart Trae IDE (or start a new AI session) for hooks to take effect." -ForegroundColor Cyan
} else {
    Write-Host ""
    Write-Host "=== Installation may have issues ===" -ForegroundColor Red
    if (-not $bridgeExists) { Write-Host "  Bridge script not found at: $bridgeDst" -ForegroundColor Red }
    if (-not $hooksValid) { Write-Host "  Hooks config missing trae-pet entry" -ForegroundColor Red }
}

Write-Host ""
Write-Host "Hook events configured:" -ForegroundColor Gray
Write-Host "  - SessionStart     (IDE session starts)" -ForegroundColor Gray
Write-Host "  - UserPromptSubmit (AI begins processing)" -ForegroundColor Gray
Write-Host "  - PreToolUse       (AI tool about to execute)" -ForegroundColor Gray
Write-Host "  - PostToolUse      (AI tool completed)" -ForegroundColor Gray
Write-Host "  - Stop             (AI finished task)" -ForegroundColor Gray
Write-Host "  - Notification     (User confirmation needed)" -ForegroundColor Gray
