<#
.SYNOPSIS
    Copies everything usdcc needs to run into one folder, to distribute it.

.DESCRIPTION
    Collects a built usdcc (run tools/build.ps1 first) with its runtime
    dependencies into a self-contained folder that runs on a machine without
    the dev environment:

        <Destination>\
          usdcc.cmd     starts bin\usdcc.exe
          LICENSE.txt   usdcc's licence (BSD-3-Clause)
          licenses\     the licences of everything else in the folder, and in
                        SOURCES.txt where each library's source is
                        (docs/RELEASE.md section 6)
          bin\          usdcc.exe, its DLLs (app, Qt, vcpkg, OpenUSD and the
                        libraries OpenUSD uses), Qt's platform plugin
          bin\usd\      OpenUSD's built-in plugins (schemas, Hydra, MaterialX
                        standard library, ...)
          plugin\usd\   OpenUSD's other plugins (Storm, Embree, image readers)
          python\       the Python OpenUSD was built with (Lib, DLLs, and python.exe,
                        which uv needs to install packages), and in
                        python\Lib\site-packages: usdcc, pxr, PySide6, shiboken6
          plugins\      V-Ray Render and, when enabled, native RTX Render
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

.PARAMETER OmitRtx
    Omit RTX even when enabled in the CMake cache; ignore stale native build files.

.PARAMETER BundleRtxRuntime
    Explicitly copy pinned native SDK bin trees, metadata and licenses into RTX.
    Otherwise configure external runtime roots on the target.

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

    [string]$UsdInstall,

    # Native RTX is optional. Runtime bundling is explicit; otherwise use external roots.
    [switch]$OmitRtx,
    [switch]$BundleRtxRuntime
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

$includeRtx = -not $OmitRtx -and (Get-CacheValue 'USDCC_BUILD_RTX_PLUGIN') -match '^(ON|1|YES|TRUE)$'
$nativeRtx = Join-Path $buildDir 'plugins/rtx_render_native'
if ($BundleRtxRuntime -and -not $includeRtx) { throw '-BundleRtxRuntime requires an enabled native RTX build without -OmitRtx.' }
if ($includeRtx -and -not (Test-Path (Join-Path $nativeRtx 'bin/windows-x64/rtx_render_native.dll'))) {
    throw 'Native RTX is enabled but not built. Build rtx_render_native first, or pass -OmitRtx.'
}
$runtimeRoots = @{}
if ($BundleRtxRuntime) {
    $pins = Get-Content -Raw (Join-Path $repoRoot 'deps/rtx-native.json') | ConvertFrom-Json
    foreach ($name in 'ovrtx', 'ovstage') {
        $root = Get-CacheValue "USDCC_RTX_$($name.ToUpper())_ROOT"
        $info = Get-Content -Raw (Join-Path $root "$name-build-info.json") | ConvertFrom-Json
        $hash = (Get-FileHash -LiteralPath (Join-Path $root "bin/$name-dynamic.dll") -Algorithm SHA256).Hash
        if ($info.build_string -ne $pins.$name.build -or $hash -ne $pins.$name.loader_sha256) {
            throw "The $name runtime does not match deps/rtx-native.json."
        }
        foreach ($license in 'LICENSE', 'THIRD-PARTY-NOTICES.txt') {
            if (-not (Test-Path (Join-Path $root $license))) { throw "Missing $name/$license" }
        }
        $runtimeRoots[$name] = $root
    }
}

# Before replacing a deploy, reject roots and folders containing source/dependencies.
$deployDir = $deployDir.TrimEnd('\', '/')
foreach ($protected in @([string]$repoRoot, [string]$buildDir, [string]$usdInstall, [string]$pythonHome)) {
    $absolute = [IO.Path]::GetFullPath($protected).TrimEnd('\', '/')
    if ($absolute -eq $deployDir -or $absolute.StartsWith($deployDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe deploy destination '$deployDir' contains '$absolute'."
    }
}
if ($deployDir -eq [IO.Path]::GetPathRoot($deployDir).TrimEnd('\', '/')) { throw 'Cannot deploy to a drive root.' }

if (Test-Path $deployDir) {
    $previous = Join-Path $deployDir 'bin/usdcc.exe'
    if (Test-Path $previous) {
        Write-Host "Replacing the earlier deploy in $deployDir"
        Remove-Item -LiteralPath $deployDir -Recurse -Force -Confirm:$false
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
$shippedPlugins = @('vray_render')
Write-Host '  Shipped plugins'
foreach ($plugin in $shippedPlugins) {
    Copy-Tree -From (Join-Path $repoRoot "src/plugins/$plugin") -To (Join-Path $deployDir "plugins/$plugin") -ExcludeDirs @('__pycache__')
}
if ($includeRtx) {
    $target = Join-Path $deployDir 'plugins/rtx_render'
    Copy-Tree -From $nativeRtx -To $target -ExcludeFiles ($buildOnlyFiles + @('runtime.json')) -ExcludeDirs @('runtime')
    foreach ($name in $runtimeRoots.Keys) {
        $root = $runtimeRoots[$name]
        $runtimeTarget = Join-Path $target "runtime/$name"
        Copy-Tree -From (Join-Path $root 'bin') -To (Join-Path $runtimeTarget 'bin') -ExcludeFiles @('*.log', 'crash_*.txt', '*.dmp', 'ucode.elf')
        foreach ($file in @("$name-build-info.json", 'LICENSE', 'THIRD-PARTY-NOTICES.txt')) {
            Copy-Item -LiteralPath (Join-Path $root $file) -Destination $runtimeTarget
        }
    }
}
Write-Host '  Example plugins'
Copy-Tree -From (Join-Path $repoRoot 'src/plugins') -To (Join-Path $deployDir 'examples/plugins') -ExcludeDirs (@('__pycache__', 'cpp_example', 'rtx_render', 'rtx_render_native') + $shippedPlugins)
Copy-Tree -From (Join-Path $buildDir 'plugins') -To (Join-Path $deployDir 'examples/plugins') -ExcludeFiles $buildOnlyFiles -ExcludeDirs @('rtx_render', 'rtx_render_native')

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

# --- licences (docs/RELEASE.md section 6) ----------------------------------

Write-Host '  Licences'
$licensesDir = Join-Path $deployDir 'licenses'
New-Item -ItemType Directory -Force $licensesDir | Out-Null
Copy-Item (Join-Path $repoRoot 'LICENSE') (Join-Path $deployDir 'LICENSE.txt')

# OpenUSD's own, and its dependencies': deps-usd.yml collects those into the CI
# asset's licenses folder; a local build_usd.py install still has the source
# and build folders to collect them from.
$usdLicenses = Join-Path $licensesDir 'OpenUSD'
New-Item -ItemType Directory -Force $usdLicenses | Out-Null
foreach ($file in 'LICENSE', 'THIRD-PARTY.md') {
    $source = Join-Path $usdInstall $file
    if (Test-Path $source) { Copy-Item $source $usdLicenses }
}
$usdDependencies = Join-Path $licensesDir 'OpenUSD-dependencies'
$hasUsdDependencyLicenses = $true
if (Test-Path (Join-Path $usdInstall 'licenses')) {
    Copy-Tree -From (Join-Path $usdInstall 'licenses') -To $usdDependencies
} elseif (Test-Path (Join-Path $usdInstall 'src')) {
    & (Join-Path $repoRoot 'tools/ci/usd-licenses.ps1') -Src (Join-Path $usdInstall 'src') `
        -Build (Join-Path $usdInstall 'build') -Destination $usdDependencies | Out-Host
} else {
    $hasUsdDependencyLicenses = $false
    Write-Warning "No licences of OpenUSD's dependencies in '$usdInstall' (no licenses or src folder): the deploy works, but can't be distributed as it is."
}

# vcpkg's packages: a copyright file each, and in SOURCES.txt where its source
# is, from the SPDX file vcpkg writes (the LGPL ones, Qt and Qt Advanced
# Docking System, have to say so). vcpkg's own build helpers aren't shipped.
$vcpkgShare = Join-Path $buildDir 'vcpkg_installed/x64-windows/share'
$vcpkgLicenses = Join-Path $licensesDir 'vcpkg'
New-Item -ItemType Directory -Force $vcpkgLicenses | Out-Null
$sources = @(
    'Where the source of each library in this folder is. The LGPL ones (Qt, Qt Advanced Docking',
    'System, PySide6 and shiboken6) are separate DLLs, which you may replace with your own builds',
    'of the same version.',
    '')
foreach ($port in Get-ChildItem $vcpkgShare -Directory | Sort-Object Name) {
    if ($port.Name -match '^(vcpkg-|pkgconf$)') { continue }
    $copyright = Join-Path $port.FullName 'copyright'
    if (-not (Test-Path $copyright)) { continue }
    Copy-Item $copyright (Join-Path $vcpkgLicenses "$($port.Name).txt")
    $spdx = Join-Path $port.FullName 'vcpkg.spdx.json'
    if (Test-Path $spdx) {
        $packages = (Get-Content -Raw $spdx | ConvertFrom-Json).packages
        $sources += "$($port.Name) $($packages[0].versionInfo)"
        $sources += "  vcpkg port: $($packages[0].downloadLocation -replace '^git\+', '')"
        foreach ($package in $packages | Select-Object -Skip 1) {
            if ($package.downloadLocation -and $package.downloadLocation -ne 'NONE') {
                $sources += "  source:     $($package.downloadLocation -replace '^git\+', '')"
            }
        }
    }
}

if (Test-Path (Join-Path $pythonHome 'LICENSE.txt')) {
    New-Item -ItemType Directory -Force (Join-Path $licensesDir 'Python') | Out-Null
    Copy-Item (Join-Path $pythonHome 'LICENSE.txt') (Join-Path $licensesDir 'Python')
}
if ($hasPySide) {
    # tools/build-pyside.ps1 copies them in; an older PySide build has them in
    # pyside-setup's checkout.
    $pysideLicenses = @((Join-Path $pysideDir 'LICENSES'), (Join-Path $repoRoot 'external/pyside-setup/LICENSES')) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($pysideLicenses) {
        Copy-Tree -From $pysideLicenses -To (Join-Path $licensesDir 'PySide6')
    } else {
        Write-Warning "No PySide6 licences in '$pysideDir\LICENSES': run tools/build-pyside.ps1 again."
    }
    $pysideVersion = (& $pythonExe -I -c "import sys; sys.path.insert(0, sys.argv[1]); import PySide6; print(PySide6.__version__)" $pysideDir 2>$null)
    if ($pysideVersion) {
        $sources += "PySide6 and shiboken6 $pysideVersion, built by tools/build-pyside.ps1 (https://github.com/olihey/usdcc)"
        $sources += "  source:     https://code.qt.io/cgit/pyside/pyside-setup.git/tag/?h=v$pysideVersion"
    }
}
$qwindowkit = Join-Path $repoRoot 'external/qwindowkit/LICENSE'
if (Test-Path $qwindowkit) {
    New-Item -ItemType Directory -Force (Join-Path $licensesDir 'QWindowKit') | Out-Null
    Copy-Item $qwindowkit (Join-Path $licensesDir 'QWindowKit')
}
foreach ($tool in 'ruff', 'uv') {
    $toolDir = Join-Path $deployDir "tools/$tool"
    if (Test-Path $toolDir) {
        New-Item -ItemType Directory -Force (Join-Path $licensesDir $tool) | Out-Null
        Get-ChildItem $toolDir -Filter 'LICENSE*' | Copy-Item -Destination (Join-Path $licensesDir $tool)
    }
}
Set-Content -Path (Join-Path $licensesDir 'SOURCES.txt') -Encoding utf8 -Value $sources

# --- launcher and checks ---------------------------------------------------

Set-Content -Path (Join-Path $deployDir 'usdcc.cmd') -Encoding ascii -Value @(
    '@echo off',
    'start "" "%~dp0bin\usdcc.exe" %*'
)

$required = @(
    'bin/usdcc.exe', 'bin/usd_plug.dll', 'bin/QtNodes.dll', 'bin/platforms/qwindows.dll', 'bin/usd/plugInfo.json',
    'plugin/usd/hdStorm.dll', 'python/python.exe', 'python/Lib/encodings/__init__.py', 'python/Lib/site-packages/usdcc/__init__.py',
    'python/Lib/site-packages/pxr/__init__.py', 'python/Lib/site-packages/usdcc/_vendor/packaging/version.py',
    'plugins/vray_render/usdcc-plugin.json',
    'examples/plugins/measure_example/usdcc-plugin.json', 'examples/plugins/cpp_example/usdcc-plugin.json',
    'sdk/UsdccPluginConfig.cmake', 'sdk/lib/usdcc_ui.lib', 'sdk/include/usdcc/plugin/plugin.h',
    'LICENSE.txt', 'licenses/OpenUSD/LICENSE', 'licenses/vcpkg/qtbase.txt', 'licenses/SOURCES.txt'
)
if ($includeRtx) { $required += @('plugins/rtx_render/usdcc-plugin.json', 'plugins/rtx_render/bin/windows-x64/rtx_render_native.dll') }
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
