# Builds the minimal static FFmpeg (plus dav1d) that the in-app player and
# trimming link against: MP4 in/out, H.264/HEVC/AV1/AAC decoding, AAC encoding
# (a new mix when a trim leaves out a source), D3D11VA, hardware H.264 via
# Media Foundation and swscale for size-limited sharing copies.
# ffmpeg-mf-encoder-index.patch lets the export try the next hardware encoder
# when the first one Windows lists refuses (an AMD iGPU next to an NVIDIA card).
# Output: .deps\ffmpeg (include, lib). Rerunning with unchanged pins is a no-op.
# Needs Visual Studio 2022 C++ tools and internet once; MSYS2 (make, nasm,
# meson) is taken from $env:REFARTER_MSYS2, C:\msys64 or downloaded portably
# into .deps\msys64. Nothing is installed system-wide.
param(
    [string]$DepsPath = (Join-Path (Split-Path -Parent $PSScriptRoot) '.deps')
)

$ErrorActionPreference = 'Stop'

$ffmpegVersion = '8.1.3'
$ffmpegSha256 = '7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3'
$dav1dVersion = '1.5.4'
$dav1dSha256 = '686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5'
$msys2Url = 'https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-base-x86_64-20260927.sfx.exe'
$msys2Sha256 = 'ad336cccfda47758b5e15cda993fbba421115cb0b126697daef1ee4dfe37209f'

# Only what Refarter uses. Every extra decoder/format adds size to Refarter.exe.
$ffmpegOptions = @(
    '--toolchain=msvc', '--arch=x86_64', '--target-os=win64',
    '--enable-static', '--disable-shared', '--disable-debug', '--disable-doc', '--enable-small',
    '--disable-programs', '--disable-network', '--disable-autodetect', '--disable-everything',
    '--disable-avdevice', '--disable-avfilter', '--enable-swscale',
    '--enable-w32threads', '--enable-d3d11va', '--enable-libdav1d',
    '--enable-decoder=h264,hevc,av1,aac,libdav1d', '--enable-encoder=aac,h264_mf', '--enable-mediafoundation',
    '--enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2,av1_d3d11va,av1_d3d11va2',
    '--enable-parser=h264,hevc,av1,aac',
    '--enable-demuxer=mov', '--enable-muxer=mp4', '--enable-protocol=file',
    '--extra-cflags=-MT', '--pkg-config=pkgconf', '--pkg-config-flags=--static'
)
$dav1dOptions = @(
    '--buildtype=release', '--default-library=static', '-Db_vscrt=mt',
    '-Dbitdepths=8', '-Denable_tools=false', '-Denable_tests=false', '-Denable_examples=false'
)

$patch = Join-Path $PSScriptRoot 'ffmpeg-mf-encoder-index.patch'
$patchHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash

$prefix = Join-Path $DepsPath 'ffmpeg'
$stamp = Join-Path $prefix 'refarter-build.txt'
$stampText = "ffmpeg $ffmpegVersion; dav1d $dav1dVersion; patch $patchHash`n$($ffmpegOptions -join ' ')`n$($dav1dOptions -join ' ')"
if ((Test-Path -LiteralPath $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $stampText.Trim())) {
    Write-Host "FFmpeg is up to date: $prefix"
    return
}

$downloads = Join-Path $DepsPath 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null

function Get-Pinned([string]$url, [string]$sha256) {
    $file = Join-Path $downloads ([IO.Path]::GetFileName($url))
    if (-not (Test-Path -LiteralPath $file) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash -ne $sha256) {
        Write-Host "Downloading $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash -ne $sha256) {
            Remove-Item -LiteralPath $file
            throw "Checksum mismatch: $url"
        }
    }
    $file
}

# MSYS2 provides make/nasm for FFmpeg's configure and meson/ninja for dav1d.
$msys2 = $env:REFARTER_MSYS2
if (-not $msys2 -and (Test-Path 'C:\msys64\usr\bin\bash.exe')) { $msys2 = 'C:\msys64' }
if (-not $msys2) {
    $msys2 = Join-Path $DepsPath 'msys64'
    if (-not (Test-Path (Join-Path $msys2 'usr\bin\bash.exe'))) {
        $sfx = Get-Pinned $msys2Url $msys2Sha256
        & $sfx -y "-o$DepsPath" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Extracting MSYS2 failed.' }
    }
}
$bash = Join-Path $msys2 'usr\bin\bash.exe'

# Visual Studio's compiler environment, inherited by the MSYS2 shell.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Install Visual Studio 2022 Desktop development with C++.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$clDirectory = Split-Path -Parent (Get-Command cl.exe).Source

New-Item -ItemType Directory -Force $DepsPath | Out-Null
$stepFile = Join-Path $DepsPath 'step.sh'
function Invoke-Msys([string]$script) {
    $env:MSYSTEM = 'UCRT64'
    $env:MSYS2_PATH_TYPE = 'inherit'
    $env:CHERE_INVOKING = '1'
    # A script file: Windows PowerShell mangles quotes in native arguments.
    # MSVC goes first in PATH because MSYS2's coreutils also has a link.exe.
    $text = "set -e`nexec 2>&1`nexport PATH=`"`$(cygpath -u '$clDirectory'):`$PATH`"`n$script`n"
    [IO.File]::WriteAllText($stepFile, ($text -replace "`r`n", "`n"))
    & $bash -l ($stepFile -replace '\\', '/')
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 step failed: $script" }
}

function Install-MsysBuildTools {
    # Core upgrades can close their shell before the remaining packages are updated.
    # Always finish in a fresh shell, then install tools separately. A second failure is fatal.
    # https://www.msys2.org/docs/updating/
    try { Invoke-Msys 'pacman --noconfirm -Syu' }
    catch { Write-Warning "MSYS2 update did not finish; retrying in a fresh shell: $($_.Exception.Message)" }
    Invoke-Msys 'pacman --noconfirm -Syu'
    Invoke-Msys 'pacman --noconfirm -S --needed make nasm pkgconf diffutils patch mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja'
    Invoke-Msys 'for tool in make nasm pkgconf diff patch meson ninja; do command -v "$tool" >/dev/null; done'
}
Install-MsysBuildTools

$ffmpegArchive = Get-Pinned "https://ffmpeg.org/releases/ffmpeg-$ffmpegVersion.tar.xz" $ffmpegSha256
$dav1dArchive = Get-Pinned "https://downloads.videolan.org/pub/videolan/dav1d/$dav1dVersion/dav1d-$dav1dVersion.tar.xz" $dav1dSha256

$work = Join-Path $DepsPath 'work'
if (Test-Path -LiteralPath $work) { Remove-Item -Recurse -Force -LiteralPath $work }
if (Test-Path -LiteralPath $prefix) { Remove-Item -Recurse -Force -LiteralPath $prefix }
New-Item -ItemType Directory -Force $work, $prefix | Out-Null

$u = { param($path) $full = [IO.Path]::GetFullPath($path); '/' + $full.Substring(0, 1).ToLower() + ($full.Substring(2) -replace '\\', '/') }
$workU = & $u $work
$prefixU = & $u $prefix
$prefixM = $prefix -replace '\\', '/'

Invoke-Msys "cd '$workU' && tar -xf '$(& $u $dav1dArchive)' && tar -xf '$(& $u $ffmpegArchive)'"
Invoke-Msys "cd '$workU/ffmpeg-$ffmpegVersion' && patch -p1 --forward < '$(& $u $patch)'"

Invoke-Msys @"
cd '$workU/dav1d-$dav1dVersion'
CC=cl meson setup build --prefix='$prefixM' --libdir=lib $($dav1dOptions -join ' ')
ninja -C build install
# Meson names MSVC static libraries libX.a; FFmpeg's configure looks for X.lib.
mv '$prefixU/lib/libdav1d.a' '$prefixU/lib/dav1d.lib'
"@

Invoke-Msys @"
mkdir -p '$workU/ffmpeg-build' && cd '$workU/ffmpeg-build'
export PKG_CONFIG_PATH='$prefixU/lib/pkgconfig'
'../ffmpeg-$ffmpegVersion/configure' --prefix='$prefixM' $($ffmpegOptions -join ' ')
make -j`$(nproc)
make install
"@

Copy-Item (Join-Path $work "ffmpeg-$ffmpegVersion\COPYING.LGPLv2.1") (Join-Path $prefix 'FFMPEG-COPYING.LGPLv2.1')
Copy-Item (Join-Path $work "dav1d-$dav1dVersion\COPYING") (Join-Path $prefix 'DAV1D-COPYING')
Remove-Item -Recurse -Force -LiteralPath $work, $stepFile
Set-Content -LiteralPath $stamp -Value $stampText -Encoding utf8
Write-Host "Built FFmpeg $ffmpegVersion (patched) with dav1d $dav1dVersion into $prefix"
