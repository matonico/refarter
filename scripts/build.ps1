# Builds the Windows GUI (Refarter.exe) and the capture harness (refarter-record.exe)
# into build\<Configuration>\ with CMake and Visual Studio 2022 (x64).
#   .\scripts\build.ps1                          # Release
#   .\scripts\build.ps1 -Configuration Debug
#   .\scripts\build.ps1 -VersionSuffix beta      # footer/version resource suffix; "" for none
# Finds CMake on PATH or in Visual Studio 2022, and first runs build-ffmpeg.ps1,
# which builds the player's minimal FFmpeg and dav1d into .deps\ once (internet needed
# the first time, a no-op afterwards). Close a running Refarter before rebuilding.
# Tests: ctest --test-dir build -C Release --output-on-failure
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    # Pre-release suffix for the footer and version resource; omit to keep the cached value (empty by default).
    [string]$VersionSuffix
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }

if (-not $cmakePath) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $installPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidatePath = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidatePath) { $cmakePath = $candidatePath }
        }
    }
}

if (-not $cmakePath) {
    throw 'Install Visual Studio 2022 Desktop development with C++, including CMake tools and a Windows SDK, or put CMake on PATH.'
}

# The player's minimal FFmpeg: built once into .deps\ffmpeg, a no-op afterwards.
& (Join-Path $PSScriptRoot 'build-ffmpeg.ps1')

$buildPath = Join-Path $projectRoot 'build'
$configureArgs = @()
if ($PSBoundParameters.ContainsKey('VersionSuffix')) { $configureArgs += "-DREFARTER_VERSION_SUFFIX=$VersionSuffix" }
& $cmakePath -S $projectRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64 @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmakePath --build $buildPath --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }

Write-Host "Built: $(Join-Path $buildPath "$Configuration\Refarter.exe")"
Write-Host "Built: $(Join-Path $buildPath "$Configuration\refarter-record.exe") (capture test harness)"
