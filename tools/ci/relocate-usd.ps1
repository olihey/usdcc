<#
.SYNOPSIS
    Makes an OpenUSD install's CMake files work from any folder.
.DESCRIPTION
    build_usd.py writes absolute paths into some CMake files: the install folder
    itself (pxrConfig.cmake's TBB target, MaterialX_DIR, Imath_DIR; pxrTargets.cmake;
    the boost and OpenImageIO package files) and the Python it was built with. So an
    install only works where it was built. This rewrites them (docs/CI.md section 2):

      - The install folder becomes ${_usdcc_usd_prefix}, which a line added at the
        top of each changed file works out from the file's own folder. A bracket
        argument ([[...]], which CMake doesn't expand) holding it becomes a quoted
        string.
      - pxrConfig.cmake only uses its stored Python paths when they exist on this
        machine; otherwise CMake finds Python itself.
      - The Python include folder in other files becomes ${Python3_INCLUDE_DIRS},
        which pxrConfig.cmake's find_dependency(Python3) has set by then.

    Then it checks that no CMake file still names the install folder, and that the
    Python folder is left only in pxrConfig.cmake's guarded lines. Any leftover
    fails the script.

    Running it twice is harmless: the second run finds nothing to change.
.PARAMETER Install
    The OpenUSD install folder (holding pxrConfig.cmake).
.PARAMETER BuiltAt
    The folder the install was built into, if it has been moved since. Default:
    -Install.
.EXAMPLE
    tools/ci/relocate-usd.ps1 -Install C:\deps\usd
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Install,
    [string]$BuiltAt
)

$ErrorActionPreference = 'Stop'

$Install = (Resolve-Path $Install).Path.TrimEnd('\', '/')
if (-not $BuiltAt) { $BuiltAt = $Install }
$BuiltAt = $BuiltAt.TrimEnd('\', '/')
$pxrConfig = Join-Path $Install 'pxrConfig.cmake'
if (-not (Test-Path $pxrConfig)) { throw "No pxrConfig.cmake in $Install" }

$utf8 = New-Object Text.UTF8Encoding $false
$marker = '# Made relocatable by usdcc tools/ci/relocate-usd.ps1'

# The Python OpenUSD was built with, from pxrConfig.cmake.
$configText = [IO.File]::ReadAllText($pxrConfig)
$pythonMatch = [regex]::Match($configText, 'set\(Python3_EXECUTABLE \[\[(.+?)\]\]\)')
$pythonRoot = $null
if ($pythonMatch.Success) {
    $pythonRoot = (Split-Path $pythonMatch.Groups[1].Value).TrimEnd('\', '/')
}

# A path in either slash style, matched case-insensitively.
function Get-PathPattern([string]$path) {
    $parts = $path -split '[\\/]' | ForEach-Object { [regex]::Escape($_) }
    return '(?i)' + ($parts -join '[\\/]')
}
$prefixPattern = Get-PathPattern $BuiltAt
$pythonPattern = if ($pythonRoot) { Get-PathPattern $pythonRoot } else { $null }

function Get-RelativePath([string]$from, [string]$to) {
    $fromUri = New-Object Uri ($from.TrimEnd('\') + '\')
    $toUri = New-Object Uri ($to.TrimEnd('\') + '\')
    $relative = [Uri]::UnescapeDataString($fromUri.MakeRelativeUri($toUri).ToString()).TrimEnd('/')
    if (-not $relative) { return '.' }
    return $relative
}

$changed = @()
# build_usd.py's own src\ and build\ folders, if still there, aren't part of the install.
$cmakeFiles = Get-ChildItem -Path $Install -Recurse -File -Include '*.cmake' |
    Where-Object { $_.FullName.Substring($Install.Length + 1) -notmatch '^(src|build)[\\/]' }

foreach ($file in $cmakeFiles) {
    $text = [IO.File]::ReadAllText($file.FullName)
    $original = $text

    # [[<prefix>rest]] -> "${_usdcc_usd_prefix}rest" (forward slashes: a quoted
    # CMake string treats a backslash as an escape).
    $text = [regex]::Replace($text, '\[\[' + $prefixPattern + '([^\]]*)\]\]', {
        param($m) '"${_usdcc_usd_prefix}' + ($m.Groups[1].Value -replace '\\', '/') + '"'
    })
    $text = [regex]::Replace($text, $prefixPattern, '${_usdcc_usd_prefix}')

    if ($pythonPattern) {
        if ($file.FullName -eq $pxrConfig) {
            # if (NOT [[<python>...]] STREQUAL "") -> if (EXISTS [[<python>...]])
            $text = [regex]::Replace($text,
                'if \(NOT (\[\[' + $pythonPattern + '[^\]]*\]\]) STREQUAL ""\)', 'if (EXISTS $1)')
        } else {
            $text = [regex]::Replace($text, $pythonPattern + '[\\/]Include', '${Python3_INCLUDE_DIRS}')
        }
    }

    if ($text -ne $original) {
        if ($text.Contains('${_usdcc_usd_prefix}') -and -not $text.StartsWith($marker)) {
            $relative = Get-RelativePath $file.DirectoryName $Install
            $header = $marker + "`n" +
                'get_filename_component(_usdcc_usd_prefix "${CMAKE_CURRENT_LIST_DIR}/' + $relative + '" ABSOLUTE)' + "`n"
            $text = $header + $text
        }
        [IO.File]::WriteAllText($file.FullName, $text, $utf8)
        $changed += $file.FullName.Substring($Install.Length + 1)
    }
}

# Check: nothing names the build folder any more, and the Python folder only
# survives in pxrConfig.cmake's EXISTS-guarded lines.
$leftovers = @()
foreach ($file in $cmakeFiles) {
    $lines = [IO.File]::ReadAllLines($file.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $bad = $line -match $prefixPattern
        if (-not $bad -and $pythonPattern -and $line -match $pythonPattern) {
            $guarded = $file.FullName -eq $pxrConfig -and
                ($line -match '^\s*if \(EXISTS \[\[' -or $line -match '^\s*set\(Python3_\w+ \[\[')
            $bad = -not $guarded
        }
        if ($bad) {
            $leftovers += "$($file.FullName.Substring($Install.Length + 1)):$($i + 1): $($line.Trim())"
        }
    }
}

Write-Host "Rewrote $($changed.Count) file(s):"
$changed | ForEach-Object { Write-Host "  $_" }
if ($leftovers.Count) {
    Write-Host "Absolute paths left:"
    $leftovers | ForEach-Object { Write-Host "  $_" }
    throw "$($leftovers.Count) absolute path(s) left in the install's CMake files"
}
Write-Host 'No absolute build paths left.'
