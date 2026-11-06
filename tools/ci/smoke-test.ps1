<#
.SYNOPSIS
    Starts a deploy (tools/deploy.ps1) the way a user would, and checks it.
.DESCRIPTION
    Runs <Deploy>\bin\usdcc.exe with a PATH of Windows' own folders only, on
    Qt's offscreen platform, with a script that records usdcc's version and
    where pxr, usdcc and PySide6 were loaded from, then quits the app. Fails
    if the app doesn't get there, if the version isn't -ExpectVersion, or if
    anything came from outside the deploy: a DLL or Python file the build's
    own folders were hiding (docs/RELEASE.md section 4).

    The deploy only has Qt's windows platform plugin; the offscreen one comes
    from -PlatformPlugins (vcpkg's Qt6\plugins\platforms).
.EXAMPLE
    tools/ci/smoke-test.ps1 -Deploy deploy -ExpectVersion 0.2.0 -PlatformPlugins build\vcpkg_installed\x64-windows\Qt6\plugins\platforms
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Deploy,
    [string]$ExpectVersion,
    [Parameter(Mandatory)] [string]$PlatformPlugins,
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'

$deployDir = (Resolve-Path $Deploy).Path
$exe = Join-Path $deployDir 'bin\usdcc.exe'
if (-not (Test-Path $exe)) { throw "No usdcc.exe in $deployDir\bin" }
if (-not (Test-Path (Join-Path $PlatformPlugins 'qoffscreen.dll'))) { throw "No qoffscreen.dll in $PlatformPlugins" }

$work = Join-Path ([IO.Path]::GetTempPath()) "usdcc-smoke-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$result = Join-Path $work 'result.json'
$script = Join-Path $work 'check.py'
Set-Content -Path $script -Encoding utf8 -Value @"
import json
import sys

from PySide6 import QtCore, QtWidgets
import PySide6
import pxr
from pxr import Usd
import usdcc
import usdcc.app

result = {
    "version": usdcc.app.version(),
    "release": usdcc.app.is_release_build(),
    "usd": ".".join(str(part) for part in Usd.GetVersion()),
    "python": sys.version.split()[0],
    "pyside": PySide6.__version__,
    "files": {"pxr": pxr.__file__, "usdcc": usdcc.__file__, "PySide6": PySide6.__file__},
}
with open(r"$result", "w") as f:
    json.dump(result, f)
QtCore.QTimer.singleShot(0, QtWidgets.QApplication.quit)
"@

# Only what a user's machine has: Windows, and the deploy itself.
$saved = @{}
foreach ($name in 'PATH', 'PYTHONHOME', 'PYTHONPATH', 'PXR_PLUGINPATH_NAME', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_QPA_PLATFORM_PLUGIN_PATH') {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $env:PYTHONHOME = $null; $env:PYTHONPATH = $null; $env:PXR_PLUGINPATH_NAME = $null; $env:QT_PLUGIN_PATH = $null
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = (Resolve-Path $PlatformPlugins).Path
    $started = Get-Date
    $process = Start-Process -FilePath $exe -ArgumentList '--no-startup-scripts', '--script', $script -PassThru
    $finished = $process.WaitForExit($TimeoutSeconds * 1000)
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}

$problems = @()
if (-not $finished) {
    $process.Kill()
    $problems += "usdcc didn't quit within $TimeoutSeconds s"
} elseif ($process.ExitCode -ne 0) {
    $problems += "usdcc exited with $($process.ExitCode)"
}
$checked = $null
if (Test-Path $result) {
    $checked = Get-Content -Raw $result | ConvertFrom-Json
    Write-Host "usdcc $($checked.version) (release: $($checked.release)), OpenUSD $($checked.usd), Python $($checked.python), PySide6 $($checked.pyside)"
    if ($ExpectVersion -and $checked.version -ne $ExpectVersion) {
        $problems += "the version is $($checked.version), not $ExpectVersion"
    }
    foreach ($module in $checked.files.PSObject.Properties) {
        $file = [IO.Path]::GetFullPath($module.Value)
        Write-Host "  $($module.Name): $file"
        if (-not $file.StartsWith($deployDir, [StringComparison]::OrdinalIgnoreCase)) {
            $problems += "$($module.Name) came from outside the deploy: $file"
        }
    }
} else {
    $problems += 'the check script wrote nothing'
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue

if ($problems) {
    $log = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'usdcc\logs') -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-5) } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($log) {
        Write-Host "--- $($log.FullName)"
        Get-Content $log.FullName -Tail 60 | Write-Host
    }
    throw "The deploy's smoke test failed: $($problems -join '; ')"
}
Write-Host 'The deploy starts and runs from its own folder.'
