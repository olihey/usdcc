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

.EXAMPLE
    tools/build.ps1

.EXAMPLE
    tools/build.ps1 -Configuration Debug -Run

.EXAMPLE
    tools/build.ps1 -UsdInstall C:\usd\install
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$Generator = 'Visual Studio 17 2022',

    [string]$Arch = 'x64',

    [string]$UsdInstall,

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
    Start-Process -FilePath $exePath
}
