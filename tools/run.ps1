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
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo',

    [string]$Generator = 'Visual Studio 17 2022',

    [string]$Arch = 'x64',

    [string]$UsdInstall,

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
if ($Clean) { $buildParams.Clean = $true }

& $buildScript @buildParams
