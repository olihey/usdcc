<#
.SYNOPSIS
    Names the prebuilt OpenUSD release asset that deps/usd.json describes.
.DESCRIPTION
    The name carries a hash of deps/usd.json and every patch it lists, so any
    change to the pin, the flags or a patch gives a new asset, and a stale one is
    never used (docs/CI.md section 2). Line endings are normalised before hashing,
    so a CRLF checkout gives the same name as an LF one.

    Prints the result as JSON, and with -GitHubOutput also appends it to
    $env:GITHUB_OUTPUT for later workflow steps:
      name    usd-<short commit>-<hash>-win64-py312
      tag     deps/usd-<short commit>-<hash>
      zip     <name>.zip
      commit, repository, python, flags (space-separated)
.EXAMPLE
    tools/ci/usd-asset.ps1
#>
[CmdletBinding()]
param(
    [string]$Pin = (Join-Path $PSScriptRoot '../../deps/usd.json'),
    [switch]$GitHubOutput
)

$ErrorActionPreference = 'Stop'

$pinPath = (Resolve-Path $Pin).Path
$depsDir = Split-Path $pinPath
$spec = Get-Content -Raw $pinPath | ConvertFrom-Json

function Get-NormalizedBytes([string]$path) {
    $text = [IO.File]::ReadAllText($path) -replace "`r`n", "`n"
    return [Text.Encoding]::UTF8.GetBytes($text)
}

$patchPaths = @()
foreach ($patch in $spec.patches) { $patchPaths += Join-Path $depsDir $patch }

$stream = New-Object IO.MemoryStream
foreach ($file in @($pinPath) + $patchPaths) {
    if (-not (Test-Path $file)) { throw "deps/usd.json lists a patch that doesn't exist: $file" }
    $bytes = Get-NormalizedBytes $file
    $stream.Write($bytes, 0, $bytes.Length)
}
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash($stream.ToArray())[0..3] | ForEach-Object { $_.ToString('x2') })

$python = $spec.python.Split('.')
$shortCommit = $spec.commit.Substring(0, 9)
$name = "usd-$shortCommit-$hash-$($spec.platform)-py$($python[0])$($python[1])"

$result = [ordered]@{
    name       = $name
    tag        = "deps/usd-$shortCommit-$hash"
    zip        = "$name.zip"
    commit     = $spec.commit
    repository = $spec.repository
    python     = $spec.python
    flags      = ($spec.flags -join ' ')
    patches    = (($patchPaths | ForEach-Object { $_ -replace '\\', '/' }) -join ';')
}

if ($GitHubOutput) {
    foreach ($key in $result.Keys) {
        Add-Content -Path $env:GITHUB_OUTPUT -Value "$key=$($result[$key])" -Encoding utf8
    }
}
$result | ConvertTo-Json
