<#
.SYNOPSIS
    Names the prebuilt PySide6 release asset for the current Qt, PySide tag and
    Python.
.DESCRIPTION
    tools/build-pyside.ps1 builds PySide6/shiboken6 against vcpkg's Qt, so the
    asset has to match the Qt usdcc builds against (docs/CI.md, phase 4). The
    name carries the PySide tag and the Qt version, and a hash of those with
    qtbase's vcpkg port version, the Python version (deps/usd.json), and this
    script and build-pyside.ps1 themselves, so a change to any of them gives a
    new asset. Line endings are normalised before hashing.

    Prints the result as JSON, and with -GitHubOutput also appends it to
    $env:GITHUB_OUTPUT for later workflow steps:
      name    pyside-<tag>-qt<qt version>-<hash>-win64-py312
      tag     deps/pyside-<tag>-qt<qt version>-<hash>
      zip     <name>.zip
      pyside, qt, python
.EXAMPLE
    tools/ci/pyside-asset.ps1
#>
[CmdletBinding()]
param(
    [switch]$GitHubOutput
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildScript = Join-Path $repoRoot 'tools/build-pyside.ps1'

function Get-NormalizedText([string]$path) {
    return [IO.File]::ReadAllText($path) -replace "`r`n", "`n"
}

# The PySide tag and module subset build-pyside.ps1 builds by default.
$buildText = Get-NormalizedText $buildScript
if ($buildText -notmatch "\`$pysideTag = '([^']+)'") { throw "No `$pysideTag in $buildScript" }
$pysideTag = $Matches[1]

# qtbase as vcpkg builds it: an overlay port in cmake/ports wins over vcpkg's own.
$qtbasePort = Join-Path $repoRoot 'cmake/ports/qtbase/vcpkg.json'
if (-not (Test-Path $qtbasePort)) { $qtbasePort = Join-Path $repoRoot 'external/vcpkg/ports/qtbase/vcpkg.json' }
if (-not (Test-Path $qtbasePort)) { throw "No qtbase port at $qtbasePort (is the vcpkg submodule checked out?)" }
$qtbase = Get-Content -Raw $qtbasePort | ConvertFrom-Json
$qtVersion = @($qtbase.version, $qtbase.'version-semver', $qtbase.'version-string') | Where-Object { $_ } | Select-Object -First 1
$qtPortVersion = if ($qtbase.'port-version') { $qtbase.'port-version' } else { 0 }

$usd = Get-Content -Raw (Join-Path $repoRoot 'deps/usd.json') | ConvertFrom-Json
$python = $usd.python.Split('.')

$inputs = "pyside=$pysideTag`nqt=$qtVersion#$qtPortVersion`npython=$($usd.python)`n" +
    $buildText + (Get-NormalizedText $PSCommandPath)
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($inputs))[0..3] | ForEach-Object { $_.ToString('x2') })

$pyside = $pysideTag.TrimStart('v')
$base = "pyside-$pyside-qt$qtVersion-$hash"
$name = "$base-win64-py$($python[0])$($python[1])"

$result = [ordered]@{
    name   = $name
    tag    = "deps/$base"
    zip    = "$name.zip"
    pyside = $pysideTag
    qt     = "$qtVersion#$qtPortVersion"
    python = $usd.python
}

if ($GitHubOutput) {
    foreach ($key in $result.Keys) {
        Add-Content -Path $env:GITHUB_OUTPUT -Value "$key=$($result[$key])" -Encoding utf8
    }
}
$result | ConvertTo-Json
