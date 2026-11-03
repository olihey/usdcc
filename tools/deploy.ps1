<#
.SYNOPSIS
    Copies everything usdcc needs to run into one folder, to distribute it.

.DESCRIPTION
    Collects a built usdcc (run tools/build.ps1 first) with its runtime
    dependencies into a self-contained folder that runs on a machine without
    the dev environment:

        <Destination>\
          usdcc.cmd     starts bin\usdcc.exe
          bin\          usdcc.exe, its DLLs (app, Qt, vcpkg, OpenUSD and the
                        libraries OpenUSD uses), Qt's platform plugin
          bin\usd\      OpenUSD's built-in plugins (schemas, Hydra, MaterialX
                        standard library, ...)
          plugin\usd\   OpenUSD's other plugins (Storm, Embree, image readers)
          python\       the Python OpenUSD was built with (Lib, DLLs), and in
                        python\Lib\site-packages: usdcc, pxr, PySide6, shiboken6
          tools\ruff\   ruff.exe and its LICENSE, for the Script Editor's Ruff
                        Format and Ruff Check (the build's pinned version)

    OpenUSD finds its plugins relative to usd_plug.dll (bin\usd and
    ..\plugin\usd), and usdcc uses python\ instead of the build's Python when
    it finds it next to bin\, so nothing needs setting up on the target
    machine. The folder is replaced if it holds an earlier deploy; any other
    non-empty folder is refused.

.PARAMETER Destination
    Folder to deploy into. Default: .\deploy (relative to the current folder).

.PARAMETER Configuration
    Build configuration to deploy. Default: RelWithDebInfo.

.EXAMPLE
    tools/deploy.ps1

.EXAMPLE
    tools/deploy.ps1 D:\dist\usdcc -Configuration Release
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Destination = 'deploy',

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$buildDir = Join-Path $repoRoot 'build'
$appDir = Join-Path $buildDir "src/cpp/app/$Configuration"
$deployDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Destination))

# Copies `$From` into `$To`, recursively unless `-TopLevelOnly`, taking the
# files matching `$Files` but not `$ExcludeFiles`, and skipping the folders
# named in `$ExcludeDirs`. robocopy exit codes below 8 mean success.
function Copy-Tree {
    param(
        [Parameter(Mandatory)] [string]$From,
        [Parameter(Mandatory)] [string]$To,
        [string]$Files = '*.*',
        [string[]]$ExcludeFiles = @(),
        [string[]]$ExcludeDirs = @(),
        [switch]$TopLevelOnly
    )
    $arguments = @($From, $To, $Files)
    if (-not $TopLevelOnly) { $arguments += '/E' }
    if ($ExcludeFiles) { $arguments += '/XF'; $arguments += $ExcludeFiles }
    if ($ExcludeDirs) { $arguments += '/XD'; $arguments += $ExcludeDirs }
    $arguments += @('/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1')
    & robocopy @arguments | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "Copying $From to $To failed (robocopy exit code $LASTEXITCODE)"
    }
    $global:LASTEXITCODE = 0
}

# Reads a value from the build's CMake cache.
function Get-CacheValue([string]$Name) {
    $match = Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') -Pattern "^$([regex]::Escape($Name)):[A-Z]+=(.*)$"
    if ($match) { return $match.Matches[0].Groups[1].Value }
    return $null
}

# --- what to deploy --------------------------------------------------------

if (-not (Test-Path (Join-Path $appDir 'usdcc.exe'))) {
    throw "No $Configuration build at $appDir; run tools/build.ps1 -Configuration $Configuration first."
}

$usdInstall = Get-CacheValue 'USD_INSTALL'
if (-not $usdInstall -or -not (Test-Path (Join-Path $usdInstall 'lib/usd_plug.dll'))) {
    throw "OpenUSD not found (USD_INSTALL in build/CMakeCache.txt: '$usdInstall')."
}

# The Python OpenUSD was built with, as cmake/modules/UsdccPython.cmake finds it.
$pythonExe = Get-CacheValue 'Python3_EXECUTABLE'
if (-not $pythonExe -or -not (Test-Path $pythonExe)) {
    throw "OpenUSD's Python not found (Python3_EXECUTABLE in build/CMakeCache.txt: '$pythonExe')."
}
$pythonHome = (& $pythonExe -c 'import sys; print(sys.base_prefix)').Trim()
if (-not (Test-Path (Join-Path $pythonHome 'Lib/encodings/__init__.py'))) {
    throw "No Python standard library in $pythonHome."
}

$pysideDir = Get-CacheValue 'USDCC_PYSIDE_DIR'
$hasPySide = $pysideDir -and (Test-Path (Join-Path $pysideDir 'PySide6/__init__.py'))

# The pinned ruff cmake/modules/UsdccRuff.cmake downloaded into build/ruff.
$ruffExe = Get-CacheValue 'USDCC_RUFF_EXE_PATH'
$hasRuff = $ruffExe -and (Test-Path $ruffExe)

$packageDir = Join-Path $buildDir 'python/usdcc'
if (-not (Test-Path (Join-Path $packageDir '__init__.py'))) {
    throw "The usdcc Python package is missing from $packageDir; build first."
}

# --- the destination ----------------------------------------------------

if (Test-Path $deployDir) {
    $previous = Join-Path $deployDir 'bin/usdcc.exe'
    if (Test-Path $previous) {
        Write-Host "Replacing the earlier deploy in $deployDir"
        Remove-Item -Recurse -Force -Confirm:$false $deployDir
    } elseif (Get-ChildItem -Force $deployDir | Select-Object -First 1) {
        throw "$deployDir exists and isn't an earlier deploy (no bin\usdcc.exe); pick an empty or new folder."
    }
}

$binDir = Join-Path $deployDir 'bin'
$pythonDir = Join-Path $deployDir 'python'
$sitePackages = Join-Path $pythonDir 'Lib/site-packages'
$buildOnlyFiles = @('*.pdb', '*.lib', '*.exp', '*.ilk')

Write-Host "Deploying usdcc ($Configuration) to $deployDir"

# --- bin: OpenUSD first, then the app's own files ------------------------
# The app's DLLs win where both have one (z.dll, libpng16.dll, ...), as they
# do when the build runs: its folder comes before OpenUSD's on the DLL path.

Write-Host '  OpenUSD libraries'
Copy-Tree -From (Join-Path $usdInstall 'lib') -To $binDir -Files '*.dll' -TopLevelOnly
Copy-Tree -From (Join-Path $usdInstall 'bin') -To $binDir -Files '*.dll' -TopLevelOnly

Write-Host '  OpenUSD plugins'
Copy-Tree -From (Join-Path $usdInstall 'lib/usd') -To (Join-Path $binDir 'usd') -ExcludeFiles $buildOnlyFiles
Copy-Tree -From (Join-Path $usdInstall 'plugin/usd') -To (Join-Path $deployDir 'plugin/usd') -ExcludeFiles $buildOnlyFiles

Write-Host '  usdcc'
Copy-Tree -From $appDir -To $binDir -ExcludeFiles $buildOnlyFiles
foreach ($dll in 'vcruntime140.dll', 'vcruntime140_1.dll') {
    $source = Join-Path $pythonHome $dll
    if (-not (Test-Path (Join-Path $binDir $dll)) -and (Test-Path $source)) {
        Copy-Item $source $binDir
    }
}

# --- python: the standard library and the packages -----------------------

Write-Host "  Python ($pythonHome)"
Copy-Tree -From (Join-Path $pythonHome 'Lib') -To (Join-Path $pythonDir 'Lib') `
    -ExcludeDirs @('site-packages', '__pycache__', 'test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip')
Copy-Tree -From (Join-Path $pythonHome 'DLLs') -To (Join-Path $pythonDir 'DLLs') -ExcludeFiles $buildOnlyFiles
if (Test-Path (Join-Path $pythonHome 'LICENSE.txt')) {
    Copy-Item (Join-Path $pythonHome 'LICENSE.txt') $pythonDir
}

Write-Host '  Python packages: usdcc, pxr'
# The usdcc package folder also holds copies of DLLs for a plain python.exe;
# the deployed app has them in bin (debug builds' *d.dll aren't needed at all).
$binDlls = @(Get-ChildItem -Path $binDir -Filter '*.dll' | ForEach-Object Name)
$packageDlls = @(Get-ChildItem -Path $packageDir -Filter '*.dll' | ForEach-Object Name)
$skipDlls = @($packageDlls | Where-Object {
        ($binDlls -contains $_) -or ($_ -match '^(.+)d\.dll$' -and $packageDlls -contains "$($Matches[1]).dll")
    })
# Python imports an extension before a .py of the same name, so a module left
# over from before it got a .py front (ui.pyd next to ui.py) would hide it.
$shadowing = @(Get-ChildItem -Path $packageDir -Filter '*.pyd' | Where-Object {
        Test-Path (Join-Path $packageDir "$($_.BaseName).py")
    } | ForEach-Object Name)
foreach ($name in $shadowing) {
    Write-Warning "Leaving out $name, a leftover build output that would hide $([IO.Path]::GetFileNameWithoutExtension($name)).py."
}
Copy-Tree -From $packageDir -To (Join-Path $sitePackages 'usdcc') -ExcludeFiles ($buildOnlyFiles + $skipDlls + $shadowing) `
    -ExcludeDirs @('__pycache__')
Copy-Tree -From (Join-Path $usdInstall 'lib/site-packages') -To $sitePackages -ExcludeFiles $buildOnlyFiles -ExcludeDirs @('__pycache__')

if ($hasPySide) {
    Write-Host '  Python packages: PySide6, shiboken6'
    # Headers, typesystems and import libraries are only for building bindings.
    $pysideBuildOnly = @('__pycache__', 'include', 'typesystems', 'glue', 'lib', 'doc', 'scripts')
    Copy-Tree -From (Join-Path $pysideDir 'PySide6') -To (Join-Path $sitePackages 'PySide6') `
        -ExcludeFiles ($buildOnlyFiles + 'rcc.exe', 'uic.exe') -ExcludeDirs $pysideBuildOnly
    Copy-Tree -From (Join-Path $pysideDir 'shiboken6') -To (Join-Path $sitePackages 'shiboken6') `
        -ExcludeFiles $buildOnlyFiles -ExcludeDirs $pysideBuildOnly
} else {
    Write-Warning "No PySide6 in '$pysideDir' (tools/build-pyside.ps1): the deploy works, but scripts can't make panels."
}

# --- tools ---------------------------------------------------------------

if ($hasRuff) {
    Write-Host '  ruff'
    $ruffDir = Join-Path $deployDir 'tools/ruff'
    New-Item -ItemType Directory -Force $ruffDir | Out-Null
    Copy-Item $ruffExe $ruffDir
    $ruffLicense = Join-Path (Split-Path $ruffExe) 'LICENSE'
    if (Test-Path $ruffLicense) { Copy-Item $ruffLicense $ruffDir }
} else {
    Write-Warning "No ruff in the build ('$ruffExe'; reconfigure with USDCC_BUNDLE_RUFF=ON while online): the deploy works, but Ruff Format and Ruff Check need a ruff on PATH."
}

# --- launcher and checks ---------------------------------------------------

Set-Content -Path (Join-Path $deployDir 'usdcc.cmd') -Encoding ascii -Value @(
    '@echo off',
    'start "" "%~dp0bin\usdcc.exe" %*'
)

$required = @(
    'bin/usdcc.exe', 'bin/usd_plug.dll', 'bin/platforms/qwindows.dll', 'bin/usd/plugInfo.json',
    'plugin/usd/hdStorm.dll', 'python/Lib/encodings/__init__.py', 'python/Lib/site-packages/usdcc/__init__.py',
    'python/Lib/site-packages/pxr/__init__.py'
)
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $deployDir $_)) })
if ($missing) {
    throw "The deploy is incomplete; missing: $($missing -join ', ')"
}

$bytes = (Get-ChildItem -Path $deployDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ('Done: {0} ({1:N0} MB). Start it with usdcc.cmd or bin\usdcc.exe.' -f $deployDir, ($bytes / 1MB))
