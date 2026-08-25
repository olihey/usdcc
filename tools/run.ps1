<#
.SYNOPSIS
    Builds usdcc and launches it.

.DESCRIPTION
    Thin wrapper around build.ps1 that always passes -Run. See build.ps1 for
    parameter details.

.EXAMPLE
    tools/run.ps1

.EXAMPLE
    tools/run.ps1 -Configuration Debug -Clean

.EXAMPLE
    tools/run.ps1 -Scene test_data\cube_and_sphere.usda
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$Generator = 'Visual Studio 17 2022',

    [string]$Arch = 'x64',

    [string]$UsdInstall,

    [string]$Scene,

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$buildScript = Join-Path $PSScriptRoot 'build.ps1'

$buildParams = @{
    Configuration = $Configuration
    Generator     = $Generator
    Arch          = $Arch
    Run           = $true
}
if ($UsdInstall) { $buildParams.UsdInstall = $UsdInstall }
if ($Scene) { $buildParams.Scene = $Scene }
if ($Clean) { $buildParams.Clean = $true }

& $buildScript @buildParams
