<#
.SYNOPSIS
    Validates a packaged native RTX plugin with an isolated profile and Windows-only PATH.
.DESCRIPTION
    Render requires -BundleRtxRuntime in deploy.ps1 and an RTX GPU. MissingRuntime
    checks the actionable error without a GPU; Omitted checks a deploy with -OmitRtx.
    Results and SDK caches remain in -Artifacts; result.json names Qt's isolated profile.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Deploy,
    [ValidateSet('Render', 'MissingRuntime', 'Omitted')] [string]$Mode = 'Render',
    [string]$Artifacts = 'build/rtx-packaged-smoke',
    [int]$TimeoutSeconds = 660
)
$ErrorActionPreference = 'Stop'
$deployDir = (Resolve-Path -LiteralPath $Deploy).Path
$work = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).Path, $Artifacts))
New-Item -ItemType Directory -Force $work, "$work/profile/local", "$work/profile/roaming" | Out-Null
$script = (Resolve-Path (Join-Path $PSScriptRoot '../../tests/rtx_packaged_smoke.py')).Path
$fixture = (Resolve-Path (Join-Path $PSScriptRoot '../../test_data/rtx_native_m1.usda')).Path
$result = Join-Path $work 'result.json'
foreach ($name in 'result.json', 'progress.json', 'clean.png') {
    $ownedFile = Join-Path $work $name
    if (Test-Path -LiteralPath $ownedFile) { Remove-Item -LiteralPath $ownedFile }
}
$saved = @{}
$names = @('PATH', 'APPDATA', 'LOCALAPPDATA', 'PYTHONHOME', 'PYTHONPATH', 'PYTHONNOUSERSITE',
    'PXR_PLUGINPATH_NAME', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_QPA_PLATFORM_PLUGIN_PATH',
    'USDCC_RTX_OVRTX_ROOT', 'USDCC_RTX_OVSTAGE_ROOT', 'USDCC_RTX_VULKAN_VALIDATION',
    'USDCC_RTX_SMOKE_ROOT', 'USDCC_RTX_SMOKE_WORK', 'USDCC_RTX_SMOKE_MODE', 'OMNICLIENT_HUB_MODE',
    'USDCC_TEST_PROFILE', 'USDCC_PLUGIN_PATH', 'USDCC_PYTHON_PATH')
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null) }
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot;$env:SystemRoot/System32/Wbem"
    $env:APPDATA = "$work/profile/roaming"; $env:LOCALAPPDATA = "$work/profile/local"
    $env:PYTHONNOUSERSITE = '1'; $env:OMNICLIENT_HUB_MODE = 'disabled'
    $env:USDCC_TEST_PROFILE = 'rtx-' + [Guid]::NewGuid().ToString('N')
    $env:USDCC_RTX_SMOKE_ROOT = $deployDir; $env:USDCC_RTX_SMOKE_WORK = $work; $env:USDCC_RTX_SMOKE_MODE = $Mode
    $process = Start-Process -FilePath (Join-Path $deployDir 'bin/usdcc.exe') -WindowStyle Hidden -PassThru `
        -WorkingDirectory $work -ArgumentList @('--no-startup-scripts', '--script', "`"$script`"", "`"$fixture`"") `
        -RedirectStandardOutput (Join-Path $work 'stdout.log') -RedirectStandardError (Join-Path $work 'stderr.log')
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill(); $process.WaitForExit(); throw "Packaged RTX timed out after $TimeoutSeconds seconds; see $work."
    }
    if ($process.ExitCode -ne 0) { throw "Packaged app exited with $($process.ExitCode); see $work." }
    if (-not (Test-Path -LiteralPath $result)) { throw "No packaged RTX result; see $work." }
    $checked = Get-Content -Raw -LiteralPath $result | ConvertFrom-Json
    $logs = Join-Path $checked.profile_data 'logs'
    if (Test-Path -LiteralPath $logs) { Copy-Item -LiteralPath $logs -Destination (Join-Path $work 'app-logs') -Recurse -Force }
    if (-not $checked.passed) { throw "Packaged RTX failed: $($checked.error)" }
    Write-Host "Packaged RTX $Mode passed; results: $result"
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
