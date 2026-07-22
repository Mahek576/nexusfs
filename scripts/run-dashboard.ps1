Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot =
    Split-Path `
        -Parent `
        $PSScriptRoot

$dashboardRoot =
    Join-Path `
        $repoRoot `
        "dashboard"

Set-Location $dashboardRoot

try
{
    $Host.UI.RawUI.WindowTitle =
        "NexusFS Dashboard"
}
catch
{
}

if (-not (Test-Path -LiteralPath ".\package.json" -PathType Leaf))
{
    throw "dashboard\package.json was not found."
}

if (-not (Test-Path -LiteralPath ".\.env.local" -PathType Leaf))
{
    throw "dashboard\.env.local was not found. Run scripts\start-local.ps1 first."
}

if ($null -eq (Get-Command "node.exe" -ErrorAction SilentlyContinue))
{
    throw "Node.js is not installed or is not available in PATH."
}

if ($null -eq (Get-Command "npm.cmd" -ErrorAction SilentlyContinue))
{
    throw "npm is not installed or is not available in PATH."
}

$listener =
    Get-NetTCPConnection `
        -LocalPort 3000 `
        -State Listen `
        -ErrorAction SilentlyContinue

if ($null -ne $listener)
{
    $processIds =
        $listener |
        Select-Object `
            -ExpandProperty OwningProcess `
            -Unique

    throw "Port 3000 is already in use by process ID(s): $($processIds -join ', ')."
}

if (-not (Test-Path -LiteralPath ".\node_modules" -PathType Container))
{
    Write-Host "Installing dashboard dependencies with npm ci..."
    npm ci

    if ($LASTEXITCODE -ne 0)
    {
        throw "Dashboard dependency installation failed."
    }
}

Write-Host ""
Write-Host "Starting NexusFS dashboard"
Write-Host "URL: http://127.0.0.1:3000"
Write-Host ""

npm run dev -- --hostname 127.0.0.1 --port 3000

if ($LASTEXITCODE -ne 0)
{
    throw "NexusFS dashboard exited with code $LASTEXITCODE."
}
