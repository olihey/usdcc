<#
.SYNOPSIS
    Collects the licences of OpenUSD's dependencies from build_usd.py's folders.
.DESCRIPTION
    build_usd.py installs OpenUSD's own LICENSE and THIRD-PARTY.md, but not
    the licences of most of the libraries it builds (TBB, OpenImageIO,
    OpenColorIO, MaterialX, OpenSubdiv, Boost, ...). Those are in their source
    folders, and the small libraries OpenColorIO and OpenEXR bundle are in the
    build folder. This copies each one's licence files into
    <Destination>\<library>\, for the release zip (docs/RELEASE.md section 6).

    deps-usd.yml runs it before deleting the source and build folders, into the
    install's licenses folder; tools/deploy.ps1 runs it for a local OpenUSD
    install that still has them.
.PARAMETER Src
    build_usd.py's source folder (--src, or <install>\src).
.PARAMETER Build
    build_usd.py's build folder (--build, or <install>\build). Optional.
.PARAMETER Destination
    Where the licences go.
.EXAMPLE
    tools/ci/usd-licenses.ps1 -Src C:\b\src -Build C:\b\build -Destination C:\deps\usd\licenses
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Src,
    [string]$Build,
    [Parameter(Mandatory)] [string]$Destination
)

$ErrorActionPreference = 'Stop'

# Licence texts: LICENSE, COPYING.txt, LICENSE-NVIDIA, ..., not code such as
# Boost's licence_info.cpp.
function Test-LicenceFile([IO.FileInfo]$File) {
    return $File.Name -match '^(LICEN[SC]E|COPYING|NOTICE|COPYRIGHT)' -and
        $File.Extension -notmatch '^\.(c|cc|cpp|h|hpp|py|xml|rtf|html?|cmake|in)$'
}

function Copy-Licences([string]$From, [string]$Name, [switch]$Recurse) {
    $listing = if ($Recurse) {
        Get-ChildItem -Path $From -File -Recurse -Depth 3 -ErrorAction SilentlyContinue
    } else {
        Get-ChildItem -Path $From -File -ErrorAction SilentlyContinue
    }
    $files = @($listing | Where-Object { (Test-LicenceFile $_) -and $_.FullName -notmatch '[\\/](tests?|docs?|examples?)[\\/]' })
    if (-not $files) { return 0 }
    $to = Join-Path $Destination $Name
    New-Item -ItemType Directory -Force $to | Out-Null
    foreach ($file in $files) { Copy-Item $file.FullName (Join-Path $to $file.Name) -Force }
    return $files.Count
}

New-Item -ItemType Directory -Force $Destination | Out-Null
$found = [ordered]@{}

# Each extracted source: a folder per library, its licence at the top. The
# OpenUSD checkout itself is installed with its own LICENSE.
foreach ($folder in Get-ChildItem -Path $Src -Directory) {
    if ($folder.Name -match '^OpenUSD') { continue }
    $count = Copy-Licences -From $folder.FullName -Name $folder.Name
    if ($count) { $found[$folder.Name] = $count }
}

# Libraries the others build and link in themselves: OpenColorIO's
# ext/build/<library> and CMake's FetchContent _deps/<library>-src.
if ($Build -and (Test-Path $Build)) {
    foreach ($bundled in Get-ChildItem -Path $Build -Directory -Recurse -Depth 3 -ErrorAction SilentlyContinue) {
        $parent = Split-Path $bundled.FullName -Parent
        $name = $null
        if ((Split-Path $parent -Leaf) -eq 'build' -and (Split-Path (Split-Path $parent -Parent) -Leaf) -eq 'ext') {
            $name = $bundled.Name
        } elseif ((Split-Path $parent -Leaf) -eq '_deps' -and $bundled.Name -match '^(.+)-src$') {
            $name = $Matches[1]
        }
        if (-not $name -or $found.Contains($name)) { continue }
        $count = Copy-Licences -From $bundled.FullName -Name $name -Recurse
        if ($count) { $found[$name] = $count }
    }
}

Write-Host "Licences of $($found.Count) of OpenUSD's dependencies: $($found.Keys -join ', ')"
