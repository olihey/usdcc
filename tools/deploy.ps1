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
          python\       the Python OpenUSD was built with (Lib, DLLs, and python.exe,
                        which uv needs to install packages), and in
                        python\Lib\site-packages: usdcc, pxr, PySide6, shiboken6
          plugins\      plugins shipped with the app, always looked in: V-Ray
                        Render and RTX Render
          examples\plugins\   the example plugins, loaded with --plugin-path or
                        added in Settings > Plugins
          sdk\          headers, import libraries and CMake files for C++ plugins
          tools\ruff\   ruff.exe and its LICENSE, for the Script Editor's Ruff
                        Format and Ruff Check (the build's pinned version)
          tools\uv\     uv.exe and its licenses, which install the Python packages
                        plugins ask for (the build's pinned version)

    OpenUSD finds its plugins relative to usd_plug.dll (bin\usd and
    ..\plugin\usd), and usdcc uses python\ instead of the build's Python when
    it finds it next to bin\, so nothing needs setting up on the target
    machine. The folder is replaced if it holds an earlier deploy; any other
    non-empty folder is refused.

.PARAMETER Destination
    Folder to deploy into. Default: .\deploy (relative to the current folder).

.PARAMETER Configuration
    Build configuration to deploy. Default: RelWithDebInfo.

.PARAMETER UsdInstall
    Optional path to the OpenUSD install/build directory to take OpenUSD's
    files from. Default: the one the build was configured with (USD_INSTALL in
    build/CMakeCache.txt, as tools/build.ps1 -UsdInstall sets it). usdcc is
    compiled against that one, so another one gets a warning: it may not run
    with it.

.EXAMPLE
    tools/deploy.ps1

.EXAMPLE
    tools/deploy.ps1 D:\dist\usdcc -Configuration Release

.EXAMPLE
    tools/deploy.ps1 -UsdInstall C:\usd\install
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Destination = 'deploy',

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$UsdInstall
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$buildDir = Join-Path $repoRoot 'build'
$appDir = Join-Path $buildDir "src/cpp/app/$Configuration"
# Combine, not Join-Path: it keeps an absolute $Destination as it is.
$deployDir = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine((Get-Location).Path, $Destination))

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

$builtUsdInstall = Get-CacheValue 'USD_INSTALL'
if ($UsdInstall) {
    if (-not (Test-Path (Join-Path $UsdInstall 'lib/usd_plug.dll'))) {
        throw "No OpenUSD in '$UsdInstall' (no lib\usd_plug.dll)."
    }
    $usdInstall = [System.IO.Path]::GetFullPath($UsdInstall).TrimEnd('\', '/')
    $builtFrom = if ($builtUsdInstall) { [System.IO.Path]::GetFullPath($builtUsdInstall).TrimEnd('\', '/') } else { '' }
    if ($usdInstall -ne $builtFrom) {
        Write-Warning ("usdcc was built against the OpenUSD in '$builtUsdInstall', not '$usdInstall'; " +
            "the deploy may not run with it. Build with tools/build.ps1 -UsdInstall '$UsdInstall' first to match.")
    }
} else {
    $usdInstall = $builtUsdInstall
    if (-not $usdInstall -or -not (Test-Path (Join-Path $usdInstall 'lib/usd_plug.dll'))) {
        throw "OpenUSD not found (USD_INSTALL in build/CMakeCache.txt: '$usdInstall'); pass -UsdInstall."
    }
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

# The pinned uv cmake/modules/UsdccUv.cmake downloaded into build/uv.
$uvExe = Get-CacheValue 'USDCC_UV_EXE_PATH'
$hasUv = $uvExe -and (Test-Path $uvExe)

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

Write-Host "  OpenUSD libraries ($usdInstall)"
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
# uv can't work without an interpreter, not even to resolve. Its DLLs are in bin,
# which the app puts on PATH when it runs uv (docs/PYTHON_PACKAGES.md).
Copy-Item (Join-Path $pythonHome 'python.exe') $pythonDir
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

if ($hasUv) {
    Write-Host '  uv'
    $uvDir = Join-Path $deployDir 'tools/uv'
    New-Item -ItemType Directory -Force $uvDir | Out-Null
    Copy-Item $uvExe $uvDir
    Get-ChildItem -Path (Split-Path $uvExe) -Filter 'LICENSE*' | Copy-Item -Destination $uvDir
} else {
    Write-Warning "No uv in the build ('$uvExe'; reconfigure with USDCC_BUNDLE_UV=ON while online): the deploy works, but installing Python packages needs a uv on PATH."
}

# --- plugins ---------------------------------------------------------------

# plugins\ is where the app always looks for the plugins it ships (docs/PLUGINS.md).
# The examples sit apart, so they don't load unless asked: usdcc.cmd --plugin-path
# examples\plugins, or Settings > Plugins.
$shippedPlugins = @('vray_render', 'rtx_render')
Write-Host '  Shipped plugins'
foreach ($plugin in $shippedPlugins) {
    Copy-Tree -From (Join-Path $repoRoot "src/plugins/$plugin") -To (Join-Path $deployDir "plugins/$plugin") -ExcludeDirs @('__pycache__')
    # An older deploy had them among the examples.
    Remove-Item -Recurse -Force (Join-Path $deployDir "examples/plugins/$plugin") -ErrorAction SilentlyContinue
}
Write-Host '  Example plugins'
Copy-Tree -From (Join-Path $repoRoot 'src/plugins') -To (Join-Path $deployDir 'examples/plugins') -ExcludeDirs (@('__pycache__', 'cpp_example') + $shippedPlugins)
Copy-Tree -From (Join-Path $buildDir 'plugins') -To (Join-Path $deployDir 'examples/plugins') -ExcludeFiles $buildOnlyFiles

# --- SDK for C++ plugins (docs/PLUGIN_SDK.md) --------------------------------

Write-Host '  Plugin SDK'
$sdkDir = Join-Path $deployDir 'sdk'
foreach ($library in @('core', 'tools', 'ui', 'usd', 'plugin_sdk')) {
    Copy-Tree -From (Join-Path $repoRoot "src/cpp/$library/include") -To (Join-Path $sdkDir 'include')
}
New-Item -ItemType Directory -Force (Join-Path $sdkDir 'lib'), (Join-Path $sdkDir 'cmake') | Out-Null
foreach ($library in @('core/usdcc_core', 'usd/usdcc_usd', 'tools/usdcc_tools', 'ui/usdcc_ui', 'usd/usdcc_usd_ui')) {
    $folder, $name = $library -split '/'
    Copy-Item (Join-Path $buildDir "src/cpp/$folder/$Configuration/$name.lib") (Join-Path $sdkDir 'lib')
}
Copy-Item (Join-Path $buildDir 'sdk/UsdccPluginConfig.cmake') $sdkDir
Copy-Item (Join-Path $repoRoot 'cmake/modules/UsdccPlugin.cmake'), (Join-Path $repoRoot 'cmake/modules/FindUSD.cmake') (Join-Path $sdkDir 'cmake')
Copy-Tree -From (Join-Path $repoRoot 'src/plugins/cpp_example') -To (Join-Path $sdkDir 'example')

# --- launcher and checks ---------------------------------------------------

Set-Content -Path (Join-Path $deployDir 'usdcc.cmd') -Encoding ascii -Value @(
    '@echo off',
    'start "" "%~dp0bin\usdcc.exe" %*'
)

$required = @(
    'bin/usdcc.exe', 'bin/usd_plug.dll', 'bin/QtNodes.dll', 'bin/platforms/qwindows.dll', 'bin/usd/plugInfo.json',
    'plugin/usd/hdStorm.dll', 'python/python.exe', 'python/Lib/encodings/__init__.py', 'python/Lib/site-packages/usdcc/__init__.py',
    'python/Lib/site-packages/pxr/__init__.py', 'python/Lib/site-packages/usdcc/_vendor/packaging/version.py',
    'plugins/vray_render/usdcc-plugin.json', 'plugins/rtx_render/usdcc-plugin.json',
    'examples/plugins/measure_example/usdcc-plugin.json', 'examples/plugins/cpp_example/usdcc-plugin.json',
    'sdk/UsdccPluginConfig.cmake', 'sdk/lib/usdcc_ui.lib', 'sdk/include/usdcc/plugin/plugin.h'
)
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $deployDir $_)) })
if ($missing) {
    throw "The deploy is incomplete; missing: $($missing -join ', ')"
}

# python.exe and uv together, as the app runs them: with bin on PATH for the DLLs.
if ($hasUv) {
    $savedPath = $env:PATH
    $env:PATH = "$binDir;$env:SystemRoot\System32"
    try {
        $version = & (Join-Path $deployDir 'tools/uv/uv.exe') python find --no-config --no-python-downloads (Join-Path $pythonDir 'python.exe') 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "uv can't use the deploy's python.exe: $version"
        }
    } finally {
        $env:PATH = $savedPath
        $global:LASTEXITCODE = 0
    }
}

$bytes = (Get-ChildItem -Path $deployDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ('Done: {0} ({1:N0} MB). Start it with usdcc.cmd or bin\usdcc.exe.' -f $deployDir, ($bytes / 1MB))
