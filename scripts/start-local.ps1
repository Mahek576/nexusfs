Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function New-SecureHexToken
{
    param(
        [ValidateRange(16, 256)]
        [int]$ByteCount = 32
    )

    $bytes =
        New-Object byte[] $ByteCount

    $generator =
        [System.Security.Cryptography.RandomNumberGenerator]::Create()

    try
    {
        $generator.GetBytes(
            $bytes
        )
    }
    finally
    {
        $generator.Dispose()
    }

    return (
        [System.BitConverter]::ToString(
            $bytes
        ).Replace(
            "-",
            ""
        ).ToLowerInvariant()
    )
}

$repoRoot =
    Split-Path `
        -Parent `
        $PSScriptRoot

Set-Location $repoRoot

$daemonPath =
    Join-Path `
        $repoRoot `
        "build\Debug\nexusfsd.exe"

$dashboardRoot =
    Join-Path `
        $repoRoot `
        "dashboard"

$environmentPath =
    Join-Path `
        $dashboardRoot `
        ".env.local"

$daemonScript =
    Join-Path `
        $PSScriptRoot `
        "run-daemon.ps1"

$dashboardScript =
    Join-Path `
        $PSScriptRoot `
        "run-dashboard.ps1"

if (-not (Test-Path -LiteralPath $daemonPath -PathType Leaf))
{
    throw "build\Debug\nexusfsd.exe was not found. Build NexusFS before running the local product environment."
}

if (-not (Test-Path -LiteralPath $dashboardRoot -PathType Container))
{
    throw "The dashboard directory was not found."
}

foreach ($requiredScript in @($daemonScript, $dashboardScript))
{
    if (-not (Test-Path -LiteralPath $requiredScript -PathType Leaf))
    {
        throw "Required launcher script was not found: $requiredScript"
    }
}

foreach ($port in @(8080, 3000))
{
    $listener =
        Get-NetTCPConnection `
            -LocalPort $port `
            -State Listen `
            -ErrorAction SilentlyContinue

    if ($null -ne $listener)
    {
        $processIds =
            $listener |
            Select-Object `
                -ExpandProperty OwningProcess `
                -Unique

        throw "Port $port is already in use by process ID(s): $($processIds -join ', ')."
    }
}

$adminToken =
    $null

if (Test-Path -LiteralPath $environmentPath -PathType Leaf)
{
    $tokenLine =
        Get-Content -LiteralPath $environmentPath |
        Where-Object {
            $_ -match '^\s*NEXUSFS_ADMIN_TOKEN='
        } |
        Select-Object -First 1

    if (-not [string]::IsNullOrWhiteSpace($tokenLine))
    {
        $separatorIndex =
            $tokenLine.IndexOf('=')

        if ($separatorIndex -ge 0)
        {
            $candidate =
                $tokenLine.Substring(
                    $separatorIndex + 1
                ).Trim()

            if (-not [string]::IsNullOrWhiteSpace($candidate))
            {
                $adminToken =
                    $candidate
            }
        }
    }
}

if ([string]::IsNullOrWhiteSpace($adminToken))
{
    $adminToken =
        New-SecureHexToken `
            -ByteCount 32

    $environmentContent =
        @(
            "NEXUSFS_API_BASE_URL=http://127.0.0.1:8080"
            "NEXUSFS_ADMIN_TOKEN=$adminToken"
            ""
        ) -join "`n"

    [System.IO.File]::WriteAllText(
        $environmentPath,
        $environmentContent,
        [System.Text.UTF8Encoding]::new(
            $false
        )
    )

    Write-Host "Created dashboard\.env.local with a secure local administrator token."
}
else
{
    Write-Host "Reusing the administrator token from dashboard\.env.local."
}

$currentPowerShell =
    (Get-Process -Id $PID).Path

$daemonArguments =
    "-NoExit -ExecutionPolicy Bypass -File `"$daemonScript`""

$dashboardArguments =
    "-NoExit -ExecutionPolicy Bypass -File `"$dashboardScript`""

$daemonProcess =
    Start-Process `
        -FilePath $currentPowerShell `
        -ArgumentList $daemonArguments `
        -WorkingDirectory $repoRoot `
        -PassThru

Start-Sleep -Seconds 2

if ($daemonProcess.HasExited)
{
    throw "The NexusFS daemon launcher exited before startup completed."
}

$dashboardProcess =
    Start-Process `
        -FilePath $currentPowerShell `
        -ArgumentList $dashboardArguments `
        -WorkingDirectory $repoRoot `
        -PassThru

Start-Sleep -Seconds 2

if ($dashboardProcess.HasExited)
{
    throw "The NexusFS dashboard launcher exited before startup completed."
}

Write-Host ""
Write-Host "NexusFS local product environment started."
Write-Host ""
Write-Host "Daemon:    http://127.0.0.1:8080"
Write-Host "Dashboard: http://127.0.0.1:3000"
Write-Host ""
Write-Host "Daemon process ID:    $($daemonProcess.Id)"
Write-Host "Dashboard process ID: $($dashboardProcess.Id)"
Write-Host ""
Write-Host "Close the two launcher windows or press Ctrl+C in each to stop the environment."
