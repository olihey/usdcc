<#
.SYNOPSIS
    Builds PySide6/Shiboken6 from source against usdcc's vcpkg Qt build.

.DESCRIPTION
    pip-installed PySide6 ships its own, independently-compiled Qt binaries.
    Loading those alongside usdcc's vcpkg-built Qt in one process causes
    "DLL load failed: the specified procedure could not be found" at
    essentially random points, because Windows resolves a same-named DLL
    (Qt6Widgets.dll, etc.) to whichever copy is already loaded in the
    process, not necessarily the one usdcc's own code was compiled against.
    There's no reliable way around this other than making sure there's only
    one Qt build in the process — hence building PySide6/Shiboken6
    ourselves against the *same* vcpkg Qt.

    This script automates the recipe that was worked out by hand:
      1. Bootstrap the vcpkg build (for Qt + qtpaths6) if not already done.
      2. Fetch a prebuilt libclang (shiboken6's C++ parser) if not present.
      3. Clone pyside-setup at a matching tag if not present.
      4. Build shiboken6-generator, shiboken6, and pyside6 individually
         against vcpkg's Qt (building them together in one `setup.py build`
         invocation only keeps the *last* stage's packaged output — the
         packaging step wipes and reuses one shared directory per stage).
      5. Assemble PySide6 + shiboken6 + shiboken6_generator into one
         directory, patching in the runtime DLLs pyside-setup's packaging
         step doesn't know to copy for a non-official Qt build (ICU, zstd,
         etc. — whatever vcpkg's Qt actually links against).

    Output: external/pyside-install/{PySide6,shiboken6,shiboken6_generator}
    Point USDCC_PYSIDE_INSTALL_PREFIX (a CMake variable, not used by this
    script) at external/pyside-setup/build/<...>/install to build usdcc's
    Shiboken6 bindings against this instead of a pip-installed PySide6 —
    see src/cpp/ui/python/CMakeLists.txt.

    Known workarounds baked in below, each tied to a specific build failure
    encountered while developing this script:
      - CMAKE_PREFIX_PATH env var: shiboken6_generator's own CMake configure
        step doesn't inherit -DCMAKE_PREFIX_PATH the way the top-level
        setup.py orchestration does.
      - share/Qt6/metatypes junction: pyside-setup's packaging step expects
        Qt's metatypes JSON files there; vcpkg installs them at the
        top-level metatypes/ dir instead.
      - install/plugins/designer: pyside-setup unconditionally copies this
        even when Qt6Designer isn't part of --module-subset.
      - extra runtime DLLs: pyside-setup's packaging step bundles a fixed
        list of third-party DLLs it expects an official Qt build to need;
        vcpkg's Qt has a different (larger) list.

.PARAMETER ModuleSubset
    Qt modules to build PySide6 bindings for. Default: Core,Gui,Widgets
    (matches what usdcc actually uses). Rebuilding with a larger subset
    requires a full re-run, not --reuse-build, since new modules haven't
    been configured/compiled before.

.EXAMPLE
    tools/build-pyside.ps1
#>
[CmdletBinding()]
param(
    [string]$ModuleSubset = "Core,Gui,Widgets"
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$vcpkgInstalled = Join-Path $repoRoot 'build/vcpkg_installed/x64-windows'
$externalDir = Join-Path $repoRoot 'external'
$libclangDir = Join-Path $externalDir 'libclang'
$pysideSrcDir = Join-Path $externalDir 'pyside-setup'
$pysideInstallDir = Join-Path $externalDir 'pyside-install'
$pysideTag = 'v6.11.1'
$libclangVersion = '19.1.0'
$libclangUrl = "https://download.qt.io/development_releases/prebuilt/libclang/libclang-release_$libclangVersion-based-windows-vs2019_64.7z"

function Invoke-Checked {
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)][string[]]$ArgumentList)
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath $($ArgumentList -join ' ')' failed with exit code $LASTEXITCODE"
    }
}

# --- prerequisites ---------------------------------------------------------

if (-not (Test-Path (Join-Path $vcpkgInstalled 'tools/Qt6/bin/qtpaths6.exe'))) {
    throw "vcpkg Qt build not found at $vcpkgInstalled. Run tools/build.ps1 first."
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    Write-Host 'Installing ninja (build tool required by pyside-setup)...'
    Invoke-Checked -FilePath 'python' -ArgumentList @('-m', 'pip', 'install', '--user', 'ninja')
}
$ninjaScripts = & python -c "import ninja, os; print(os.path.join(os.path.dirname(ninja.__file__), '..', 'Scripts'))"
$env:PATH = "$ninjaScripts;$env:PATH"

Write-Host 'Installing pyside-setup Python build dependencies...'
Invoke-Checked -FilePath 'python' -ArgumentList @(
    '-m', 'pip', 'install', '--user', 'packaging', 'ordered-set', 'more-itertools',
    'jaraco.text', 'importlib_metadata', 'importlib_resources', 'tomli', 'platformdirs',
    'wheel', 'setuptools')

if (-not (Test-Path (Join-Path $libclangDir 'lib/cmake/clang/ClangConfig.cmake'))) {
    Write-Host "Downloading libclang $libclangVersion (~700 MB)..."
    $archive = Join-Path $env:TEMP 'libclang.7z'
    Invoke-WebRequest -Uri $libclangUrl -OutFile $archive
    $extractDir = Join-Path $env:TEMP 'libclang-extract'
    if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }
    & 7z x $archive -o"$extractDir" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '7z extraction of libclang failed' }
    New-Item -ItemType Directory -Force $libclangDir | Out-Null
    Get-ChildItem (Join-Path $extractDir 'libclang') | Move-Item -Destination $libclangDir -Force
    Remove-Item -Recurse -Force $extractDir, $archive
}

if (-not (Test-Path (Join-Path $pysideSrcDir 'setup.py'))) {
    Write-Host "Cloning pyside-setup ($pysideTag)..."
    Invoke-Checked -FilePath 'git' -ArgumentList @(
        'clone', '--branch', $pysideTag, '--depth', '1',
        'https://code.qt.io/pyside/pyside-setup.git', $pysideSrcDir)
}

# pyside-setup's packaging step expects Qt's metatypes at share/Qt6/metatypes;
# vcpkg installs them at the top-level metatypes/ dir instead.
$metatypesLink = Join-Path $vcpkgInstalled 'share/Qt6/metatypes'
if (-not (Test-Path $metatypesLink)) {
    cmd /c mklink /J "$metatypesLink" "$(Join-Path $vcpkgInstalled 'metatypes')" | Out-Null
}

$env:LLVM_INSTALL_DIR = $libclangDir
$env:CMAKE_PREFIX_PATH = $vcpkgInstalled

$qtpaths = Join-Path $vcpkgInstalled 'tools/Qt6/bin/qtpaths6.exe'
$cmakeExe = (Get-Command cmake).Source
$jobs = [Environment]::ProcessorCount

function Invoke-PySideSetupBuild {
    param([Parameter(Mandatory)][string]$InternalBuildType)
    Push-Location $pysideSrcDir
    try {
        & python setup.py build `
            --qtpaths="$qtpaths" --cmake="$cmakeExe" --module-subset=$ModuleSubset `
            --limited-api=yes --ignore-git --parallel=$jobs --skip-docs --disable-pyi `
            --reuse-build --internal-build-type=$InternalBuildType
        if ($LASTEXITCODE -ne 0) {
            throw "setup.py build --internal-build-type=$InternalBuildType failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

# pyside-setup unconditionally copies install/plugins/designer during
# packaging even when Qt6Designer isn't in --module-subset.
$buildDirs = Get-ChildItem (Join-Path $pysideSrcDir 'build') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like 'qfpa-*' }
if ($buildDirs) {
    $designerDir = Join-Path $buildDirs[0].FullName 'install/plugins/designer'
    New-Item -ItemType Directory -Force $designerDir | Out-Null
}

Write-Host 'Building shiboken6-generator, shiboken6, and pyside6 against vcpkg Qt...'
Write-Host '(first run compiles all of Qt Core/Gui/Widgets bindings from source; can take 20+ minutes)'
Invoke-PySideSetupBuild -InternalBuildType shiboken6-generator

# The designer plugin dir may only be created by the build above (its own
# build dir didn't exist until now on a first-ever run) — ensure it exists
# before proceeding to the next stage, which also packages.
$buildDir = (Get-ChildItem (Join-Path $pysideSrcDir 'build') -Directory | Where-Object { $_.Name -like 'qfpa-*' })[0].FullName
New-Item -ItemType Directory -Force (Join-Path $buildDir 'install/plugins/designer') | Out-Null

Invoke-PySideSetupBuild -InternalBuildType shiboken6
Invoke-PySideSetupBuild -InternalBuildType pyside6

# --- assemble the redistributable install dir ------------------------------

$packageDir = Join-Path $buildDir 'package'

Write-Host "Assembling $pysideInstallDir ..."
if (Test-Path $pysideInstallDir) { Remove-Item -Recurse -Force $pysideInstallDir }
New-Item -ItemType Directory -Force $pysideInstallDir | Out-Null

# Each of the three stages above packages into the same $packageDir,
# overwriting the previous stage's output — so re-run each stage's install
# step alone (--reuse-build makes this fast) right before harvesting it.
foreach ($stage in @('shiboken6-generator', 'shiboken6', 'pyside6')) {
    Invoke-PySideSetupBuild -InternalBuildType $stage
    $stagePackageName = if ($stage -eq 'pyside6') { 'PySide6' } elseif ($stage -eq 'shiboken6-generator') { 'shiboken6_generator' } else { 'shiboken6' }
    Copy-Item -Recurse -Force (Join-Path $packageDir $stagePackageName) (Join-Path $pysideInstallDir $stagePackageName)
}

# shiboken6.abi3.dll is needed by PySide6's own .pyd files but only gets
# packaged into the shiboken6 folder, not alongside PySide6.
Copy-Item -Force (Join-Path $pysideInstallDir 'shiboken6/shiboken6.abi3.dll') (Join-Path $pysideInstallDir 'PySide6/shiboken6.abi3.dll')

# vcpkg's Qt links against a specific set of third-party DLLs that
# pyside-setup's packaging step doesn't know about (it bundles a fixed list
# tuned for official Qt builds). Mirror what CMake's vcpkg-applocal-deps
# already copies next to usdcc.exe.
$extraRuntimeDlls = @(
    'icuin78.dll', 'icuuc78.dll', 'icudt78.dll', 'z.dll', 'double-conversion.dll',
    'pcre2-16.dll', 'zstd.dll', 'libpng16.dll', 'harfbuzz.dll', 'freetype.dll',
    'bz2.dll', 'brotlidec.dll', 'brotlicommon.dll', 'md4c.dll')
foreach ($dll in $extraRuntimeDlls) {
    $src = Join-Path $vcpkgInstalled "bin/$dll"
    if (Test-Path $src) {
        Copy-Item -Force $src (Join-Path $pysideInstallDir "PySide6/$dll")
    }
}

Write-Host "Done. PySide6/shiboken6/shiboken6_generator built against vcpkg's Qt at:"
Write-Host "  $pysideInstallDir"
Write-Host ''
Write-Host 'To build usdcc against this instead of a pip-installed PySide6:'
Write-Host "  cmake -S . -B build -DUSDCC_PYSIDE_INSTALL_PREFIX=`"$buildDir/install`""
