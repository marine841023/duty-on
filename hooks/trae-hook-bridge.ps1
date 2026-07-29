<#
.SYNOPSIS
    Trae IDE Hook Bridge - Forwards hook events to the Trae Pet desktop application.
.DESCRIPTION
    This script is called by Trae IDE's Hook system for each AI lifecycle event.
    It reads the hook event JSON from stdin, enriches it with project information,
    and sends it to the Trae Pet's local HTTP server (http://127.0.0.1:17521/hook).
    The script is designed to be fast and non-blocking - if the pet is not running,
    it exits silently without affecting the AI's workflow.
#>

$ErrorActionPreference = 'SilentlyContinue'

# Read JSON from stdin
$stdinText = [Console]::In.ReadToEnd()

if ([string]::IsNullOrWhiteSpace($stdinText)) {
    # No input, nothing to do
    exit 0
}

try {
    $event = $stdinText | ConvertFrom-Json
} catch {
    # Invalid JSON, exit silently
    exit 0
}

# Get project information from environment
$projectDir = $env:TRAE_PROJECT_DIR
if ([string]::IsNullOrEmpty($projectDir)) {
    $projectDir = $env:CLAUDE_PROJECT_DIR
}
if ([string]::IsNullOrEmpty($projectDir)) {
    $projectDir = $event.cwd
}

$projectName = ''
if (-not [string]::IsNullOrEmpty($projectDir)) {
    $projectName = Split-Path $projectDir -Leaf
}

# Enrich the event with project info
$event | Add-Member -NotePropertyName "project_path" -NotePropertyValue $projectDir -Force
$event | Add-Member -NotePropertyName "project_name" -NotePropertyValue $projectName -Force

# Add timestamp
$event | Add-Member -NotePropertyName "timestamp" -NotePropertyValue ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) -Force

# Convert back to JSON for sending
$bodyJson = $event | ConvertTo-Json -Depth 10 -Compress

# Send to Trae Pet's HTTP server (with short timeout to avoid blocking AI)
try {
    $response = Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' `
        -Method Post `
        -Body $bodyJson `
        -ContentType 'application/json' `
        -TimeoutSec 2 `
        -ErrorAction Stop
} catch {
    # Pet not running or error - exit silently, don't affect AI workflow
    exit 0
}

# Exit successfully with no stdout output (to not interfere with AI)
exit 0
