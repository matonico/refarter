<#
Regenerates Refarter's original save-request chime (assets/save-chime.wav) with plain
PowerShell: no Python, downloads or build dependency. The checked-in WAV is embedded in
the executable. A soft rising fourth (E5 to A5) in a bell timbre, with a brief overlap and
a smooth final fade. Same format and algorithm as scripts/generate_failed_chime.ps1. No
samples or melodies from other games are used.
Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\generate_save_chime.ps1
Pass -Output to write somewhere other than assets\save-chime.wav.
#>
param([string]$Output = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\save-chime.wav'))
$sampleRate = 24000
$duration = 0.48

function Get-Bell([double]$time, [double]$frequency, [double]$amplitude) {
    if ($time -lt 0) { return 0.0 }
    $attack = [Math]::Min($time / 0.008, 1.0)
    $tau = 2 * [Math]::PI
    return $amplitude * $attack * (
        [Math]::Sin($tau * $frequency * $time) * [Math]::Exp(-$time / 0.11) +
        0.15 * [Math]::Sin($tau * $frequency * 2 * $time) * [Math]::Exp(-$time / 0.045) +
        0.025 * [Math]::Sin($tau * $frequency * 3 * $time) * [Math]::Exp(-$time / 0.025))
}

$count = [int][Math]::Round($sampleRate * $duration)
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$ascii = [System.Text.Encoding]::ASCII
$writer.Write($ascii.GetBytes('RIFF')); $writer.Write([int](36 + $count * 2)); $writer.Write($ascii.GetBytes('WAVE'))
$writer.Write($ascii.GetBytes('fmt ')); $writer.Write([int]16); $writer.Write([int16]1); $writer.Write([int16]1)
$writer.Write([int]$sampleRate); $writer.Write([int]($sampleRate * 2)); $writer.Write([int16]2); $writer.Write([int16]16)
$writer.Write($ascii.GetBytes('data')); $writer.Write([int]($count * 2))
for ($index = 0; $index -lt $count; $index++) {
    $time = $index / $sampleRate
    # E5, then A5 a little later with a brief overlap, and a smooth final fade.
    $value = (Get-Bell $time 659.255 0.20) + (Get-Bell ($time - 0.07) 880.0 0.23)
    $remaining = $duration - $time
    $fade = 0.5 - 0.5 * [Math]::Cos([Math]::PI * [Math]::Min($remaining / 0.08, 1.0))
    $writer.Write([int16][Math]::Round([Math]::Max(-1.0, [Math]::Min(1.0, $value * $fade)) * 32767))
}
$writer.Flush()
[System.IO.File]::WriteAllBytes($Output, $stream.ToArray())
'Generated {0}: {1} bytes, {2:N2} seconds' -f $Output, $stream.Length, $duration
