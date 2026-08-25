<#
.SYNOPSIS
    Configures and builds usdcc.

.DESCRIPTION
    Bootstraps the vcpkg submodule if needed, then runs CMake configure and
    build. Mirrors the manual steps documented in readme.md.

.PARAMETER Configuration
    CMake build configuration. Default: RelWithDebInfo.

.PARAMETER Generator
    CMake generator. Default: "Visual Studio 17 2022".

.PARAMETER Arch
    Target architecture passed to -A. Default: x64.

.PARAMETER UsdInstall
    Optional path to an OpenUSD install/build directory (containing
    pxrConfig.cmake). Forwarded as -DUSD_INSTALL=<path>. USD-dependent
    targets don't exist yet (milestone M2), so this is optional.

.PARAMETER Clean
    Remove the build directory before configuring.

.PARAMETER Run
    Launch usdcc.exe after a successful build.

.PARAMETER Scene
    Optional path to a USD stage to open on launch (forwarded to usdcc.exe as
    its command-line argument). Only meaningful together with -Run.

.EXAMPLE
    tools/build.ps1

.EXAMPLE
    tools/build.ps1 -Configuration Debug -Run

.EXAMPLE
    tools/build.ps1 -UsdInstall C:\usd\install

.EXAMPLE
    tools/build.ps1 -Run -Scene test_data\cube_and_sphere.usda
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$Generator = 'Visual Studio 17 2022',

    [string]$Arch = 'x64',

    [string]$UsdInstall,

    [string]$Scene,

    [switch]$Clean,

    [switch]$Run
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$buildDir = Join-Path $repoRoot 'build'
$vcpkgDir = Join-Path $repoRoot 'external/vcpkg'
$vcpkgExe = Join-Path $vcpkgDir 'vcpkg.exe'

function Invoke-Checked {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$ArgumentList
    )
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath $($ArgumentList -join ' ')' failed with exit code $LASTEXITCODE"
    }
}

# --- vcpkg submodule -------------------------------------------------------

if (-not (Test-Path (Join-Path $vcpkgDir 'bootstrap-vcpkg.bat'))) {
    Write-Host 'vcpkg submodule not initialized, running git submodule update --init --recursive...'
    Invoke-Checked -FilePath 'git' -ArgumentList @('-C', $repoRoot, 'submodule', 'update', '--init', '--recursive')
}

if (-not (Test-Path $vcpkgExe)) {
    Write-Host 'Bootstrapping vcpkg...'
    Invoke-Checked -FilePath (Join-Path $vcpkgDir 'bootstrap-vcpkg.bat') -ArgumentList @('-disableMetrics')
}

# --- configure ---------------------------------------------------------

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing existing build directory: $buildDir"
    Remove-Item -Recurse -Force -Confirm:$false $buildDir
}

$configureArgs = @('-S', $repoRoot, '-B', $buildDir, '-G', $Generator, '-A', $Arch)
if ($UsdInstall) {
    $configureArgs += "-DUSD_INSTALL=$UsdInstall"
}

Write-Host "Configuring ($Generator, $Arch)..."
Invoke-Checked -FilePath 'cmake' -ArgumentList $configureArgs

# --- build ---------------------------------------------------------------

Write-Host "Building ($Configuration)..."
Invoke-Checked -FilePath 'cmake' -ArgumentList @('--build', $buildDir, '--config', $Configuration)

$exePath = Join-Path $buildDir "src/cpp/app/$Configuration/usdcc.exe"
Write-Host "Build succeeded: $exePath"

if ($Run) {
    Write-Host 'Launching usdcc...'

    # USD_INSTALL is a CMake cache variable: once configured, it sticks
    # around across future builds without needing -UsdInstall again. But
    # the PATH/plugin-path setup below only has the value when the caller
    # passes it explicitly, so read it back from the cache as a fallback —
    # otherwise a later `tools/run.ps1` with no arguments silently launches
    # without USD's DLLs on PATH and fails with "usd_usdImagingGL.dll was
    # not found" even though the build itself succeeds (it's still linked
    # against the same USD install; only the launch environment is wrong).
    $effectiveUsdInstall = $UsdInstall
    if (-not $effectiveUsdInstall) {
        $cacheFile = Join-Path $buildDir 'CMakeCache.txt'
        if (Test-Path $cacheFile) {
            $cacheMatch = Select-String -Path $cacheFile -Pattern '^USD_INSTALL:PATH=(.*)$'
            if ($cacheMatch) {
                $effectiveUsdInstall = $cacheMatch.Matches[0].Groups[1].Value
            }
        }
    }

    if ($effectiveUsdInstall) {
        # USD's DLLs (split across bin/ and lib/) and Hydra render-delegate
        # plugins (e.g. hdEmbree) aren't copied next to usdcc.exe; point the
        # launched process at the USD install directly instead. Modifying
        # $env: here affects Start-Process's child too, since it inherits
        # the calling process's environment by default.
        $env:PATH = "$effectiveUsdInstall\bin;$effectiveUsdInstall\lib;$env:PATH"
        $env:PXR_PLUGINPATH_NAME = "$effectiveUsdInstall\plugin\usd"
    }

    if ($Scene) {
        # Start-Process defaults -WorkingDirectory to the *executable's*
        # directory (deep under build/), not the caller's — a relative
        # -Scene path would silently resolve to a nonexistent file there,
        # UsdStage::Open() would fail quietly, and the app would just look
        # like it launched normally with nothing loaded. Resolve against the
        # caller's cwd first so relative paths (the common case) work.
        $resolvedScene = Resolve-Path -LiteralPath $Scene
        Start-Process -FilePath $exePath -ArgumentList @("`"$resolvedScene`"")
    } else {
        Start-Process -FilePath $exePath
    }
}
