Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot =
    Split-Path `
        -Parent `
        $PSScriptRoot

Set-Location $repoRoot

try
{
    $Host.UI.RawUI.WindowTitle =
        "NexusFS Daemon"
}
catch
{
}

$daemonPath =
    Join-Path `
        $repoRoot `
        "build\Debug\nexusfsd.exe"

$environmentPath =
    Join-Path `
        $repoRoot `
        "dashboard\.env.local"

if (-not (Test-Path -LiteralPath $daemonPath -PathType Leaf))
{
    throw "nexusfsd.exe was not found. Build NexusFS before starting the local product environment."
}

if (-not (Test-Path -LiteralPath $environmentPath -PathType Leaf))
{
    throw "dashboard\.env.local was not found. Run scripts\start-local.ps1 first."
}

$adminTokenLine =
    Get-Content -LiteralPath $environmentPath |
    Where-Object {
        $_ -match '^\s*NEXUSFS_ADMIN_TOKEN='
    } |
    Select-Object -First 1

if ([string]::IsNullOrWhiteSpace($adminTokenLine))
{
    throw "NEXUSFS_ADMIN_TOKEN is missing from dashboard\.env.local."
}

$separatorIndex =
    $adminTokenLine.IndexOf('=')

if ($separatorIndex -lt 0)
{
    throw "NEXUSFS_ADMIN_TOKEN is malformed in dashboard\.env.local."
}

$adminToken =
    $adminTokenLine.Substring(
        $separatorIndex + 1
    ).Trim()

if ([string]::IsNullOrWhiteSpace($adminToken))
{
    throw "NEXUSFS_ADMIN_TOKEN is empty in dashboard\.env.local."
}

if ([string]::IsNullOrWhiteSpace($env:NEXUSFS_CLUSTER_SECRET))
{
    $randomBytes =
        New-Object byte[] 32

    $generator =
        [System.Security.Cryptography.RandomNumberGenerator]::Create()

    try
    {
        $generator.GetBytes(
            $randomBytes
        )
    }
    finally
    {
        $generator.Dispose()
    }

    $env:NEXUSFS_CLUSTER_SECRET =
        [System.BitConverter]::ToString(
            $randomBytes
        ).Replace(
            "-",
            ""
        ).ToLowerInvariant()
}

$env:NEXUSFS_ADMIN_TOKEN =
    $adminToken

$listener =
    Get-NetTCPConnection `
        -LocalPort 8080 `
        -State Listen `
        -ErrorAction SilentlyContinue

if ($null -ne $listener)
{
    $processIds =
        $listener |
        Select-Object `
            -ExpandProperty OwningProcess `
            -Unique

    throw "Port 8080 is already in use by process ID(s): $($processIds -join ', ')."
}

Write-Host ""
Write-Host "Starting NexusFS daemon"
Write-Host "API:     http://127.0.0.1:8080"
Write-Host "Storage: $repoRoot\nexusfs_data"
Write-Host ""

& $daemonPath `
    --address 127.0.0.1 `
    --port 8080 `
    --storage-root .\nexusfs_data `
    --chunk-size 1024

if ($LASTEXITCODE -ne 0)
{
    throw "NexusFS daemon exited with code $LASTEXITCODE."
}
