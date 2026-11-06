<#
.SYNOPSIS
    Sets up vcpkg's binary cache for a CI job and names its actions/cache keys.
.DESCRIPTION
    vcpkg's files cache holds one zip per package build, named after the
    package's ABI hash (which covers its port, its dependencies and the
    compiler); docs/CI.md section 4 describes how the workflows keep it.

    Points VCPKG_BINARY_SOURCES at $env:RUNNER_TEMP\vcpkg-cache (through
    $env:GITHUB_ENV, for the later steps) and appends to $env:GITHUB_OUTPUT:
      dir    the cache folder
      deps   the key prefix for these dependencies: vcpkg.json, the overlay
             ports in cmake/ports with vcpkg-configuration.json, and the vcpkg
             submodule commit
      any    the key prefix for any dependencies
      key    deps plus this run's id and attempt, for saving
.PARAMETER Access
    readwrite for a job that builds packages and saves the cache, read for one
    that only uses it.
#>
[CmdletBinding()]
param(
    [ValidateSet('readwrite', 'read')]
    [string]$Access = 'readwrite'
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$commit = git -C $repoRoot rev-parse HEAD:external/vcpkg
if ($LASTEXITCODE) { throw 'No vcpkg submodule commit' }

$files = @('vcpkg.json', 'vcpkg-configuration.json') | ForEach-Object { Join-Path $repoRoot $_ } | Where-Object { Test-Path $_ }
$ports = Join-Path $repoRoot 'cmake/ports'
if (Test-Path $ports) { $files += @(Get-ChildItem -Recurse -File $ports | ForEach-Object FullName) }
$stream = New-Object IO.MemoryStream
foreach ($file in $files | Sort-Object) {
    $relative = $file.Substring($repoRoot.Length).Replace('\', '/')
    $text = "$relative`n" + ([IO.File]::ReadAllText($file) -replace "`r`n", "`n")
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $stream.Write($bytes, 0, $bytes.Length)
}
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash($stream.ToArray())[0..7] | ForEach-Object { $_.ToString('x2') })

$dir = Join-Path $env:RUNNER_TEMP 'vcpkg-cache'
New-Item -ItemType Directory -Force $dir | Out-Null
Add-Content -Path $env:GITHUB_ENV -Value "VCPKG_BINARY_SOURCES=clear;files,$dir,$Access" -Encoding utf8

$any = "vcpkg-x64-windows-$env:ImageOS-"
$deps = "$any$hash-$commit-"
$result = [ordered]@{
    dir  = $dir
    any  = $any
    deps = $deps
    key  = "$deps$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"
}
foreach ($key in $result.Keys) {
    Add-Content -Path $env:GITHUB_OUTPUT -Value "$key=$($result[$key])" -Encoding utf8
}
$result | ConvertTo-Json
