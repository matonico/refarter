# Builds Refarter (Release x64), runs the unit tests and packages a release:
#   build\dist\Refarter-<version>-setup.exe     Inno Setup installer (per user, no admin)
#   build\dist\Refarter-<version>-portable.zip  the same files to unzip and run
#   build\dist\SHA256SUMS.txt
#   build\dist\Refarter-<version>-sources.zip   the FFmpeg, dav1d and RNNoise sources built into
#                                                the exe, with Refarter's patch to that FFmpeg source
#   .\scripts\build-installer.ps1 0.1.0-alpha
# Needs Inno Setup 6 (winget install JRSoftware.InnoSetup). The release
# workflow runs this on a release/<version> branch.
param(
    [Parameter(Mandatory)][string]$Version,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)([-.][0-9A-Za-z.]+)?$') { throw 'pass a version such as 0.1.0 or 0.1.0-alpha' }
$numeric = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
$suffix = $Matches[4] -replace '^[-.]', ''
# The exe reports the project VERSION from CMakeLists.txt, so an installer named for any
# other numeric version would ship an exe that says something else. Bump it first.
$cmakeText = Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')
if ($cmakeText -notmatch '(?m)^\s*project\(Refarter\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'could not read the project VERSION from CMakeLists.txt' }
if ($Matches[1] -ne $numeric) { throw "CMakeLists.txt project VERSION is $($Matches[1]) but the release is $numeric; bump CMakeLists.txt first" }
$release = Join-Path $root 'build\Release'
$dist = Join-Path $root 'build\dist'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -VersionSuffix $suffix
    # ctest sits next to whichever cmake build.ps1 used.
    $ctest = Get-Command ctest -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $ctest) {
        $vs = & (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe') -latest -products '*' -property installationPath
        $ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
    }
    & $ctest --test-dir (Join-Path $root 'build') -C Release --output-on-failure --no-tests=error
    if ($LASTEXITCODE) { throw 'unit tests failed; nothing was packaged' }
}
foreach ($file in 'Refarter.exe') {
    if (-not (Test-Path (Join-Path $release $file))) { throw "missing build\Release\$file; build first" }
}

# Source and cached binaries can disagree, particularly with -SkipBuild. Read the resource we will ship,
# including its pre-release suffix, before clearing old artefacts or invoking the installer compiler.
$expectedVersion = $numeric + $(if ($suffix) { "-$suffix" } else { '' })
$binaryVersion = (Get-Item -LiteralPath (Join-Path $release 'Refarter.exe')).VersionInfo.ProductVersion
if ($binaryVersion -cne $expectedVersion) {
    throw "build\Release\Refarter.exe reports '$binaryVersion' but the release is '$expectedVersion'; rebuild with the matching version and suffix before packaging"
}

$iscc = Get-Command iscc -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $iscc) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw 'Inno Setup 6 not found: winget install JRSoftware.InnoSetup' }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory $dist | Out-Null
& $iscc /Qp "/DAppVersion=$Version" "/DAppNumericVersion=$numeric" "/DSourceDir=$release" "/DRepoDir=$root" "/O$dist" (Join-Path $root 'installer\refarter.iss')
if ($LASTEXITCODE) { throw 'Inno Setup failed' }

# Portable zip: the files the installer copies, in a Refarter folder.
$stage = Join-Path $dist 'Refarter'
New-Item -ItemType Directory $stage | Out-Null
Copy-Item (Join-Path $release 'Refarter.exe'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root '.deps\ffmpeg\FFMPEG-COPYING.LGPLv2.1') (Join-Path $stage 'FFMPEG-LICENSE.txt')
Compress-Archive -Path $stage -DestinationPath (Join-Path $dist "Refarter-$Version-portable.zip")
Remove-Item $stage -Recurse -Force

# The exact FFmpeg, dav1d and RNNoise sources built into Refarter.exe, as their licences ask, in one zip.
$stage = Join-Path $dist 'sources'
New-Item -ItemType Directory $stage | Out-Null
foreach ($pattern in 'ffmpeg-*.tar.xz', 'dav1d-*.tar.xz') {
    $source = Get-ChildItem (Join-Path $root '.deps\downloads') -Filter $pattern -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $source) { throw "missing .deps\downloads\$pattern; run scripts\build-ffmpeg.ps1" }
    Copy-Item $source.FullName $stage
}
# RNNoise is fetched by CMake at configure time (FetchContent, hash-checked) into the build folder.
$rnnoise = Get-ChildItem (Join-Path $root 'build\_deps') -Recurse -Filter 'rnnoise-*.tar.gz' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $rnnoise) { throw 'missing the RNNoise archive under build\_deps; configure the build first' }
Copy-Item $rnnoise.FullName $stage
Copy-Item (Join-Path $PSScriptRoot 'ffmpeg-*.patch') $stage
Set-Content (Join-Path $stage 'README.txt') -Encoding ascii -Value @(
    "The source code of the libraries built into Refarter $Version's Refarter.exe:",
    'FFmpeg (LGPL 2.1 or later) with Refarter''s patch applied before building, dav1d (BSD 2-Clause)',
    'and RNNoise (BSD 3-Clause). Refarter''s own source code is in its repository.')
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath (Join-Path $dist "Refarter-$Version-sources.zip")
Remove-Item $stage -Recurse -Force

Get-ChildItem $dist -File | ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" } |
    Set-Content (Join-Path $dist 'SHA256SUMS.txt') -Encoding ascii
Get-ChildItem $dist -File | ForEach-Object { Write-Host ('{0}  {1:N1} MB' -f $_.Name, ($_.Length / 1MB)) }
