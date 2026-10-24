<#
Regenerates Refarter's original "save failed" chime (assets/failed-chime.wav) with
plain PowerShell: no Python, downloads or build dependency. The checked-in WAV is
embedded in the executable. Same format and bell timbre as the save chime
(scripts/generate_save_chime.ps1), but two softer notes falling a minor third (G5 to
E5), a gentle "uh-oh". No samples or melodies from other games are used.
Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\generate_failed_chime.ps1
#>
$sampleRate = 24000
$duration = 0.48
$output = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\failed-chime.wav'

function Get-Bell([double]$time, [double]$frequency, [double]$amplitude) {
    if ($time -lt 0) { return 0.0 }
    $attack = [Math]::Min($time / 0.008, 1.0)
    $tau = 2 * [Math]::PI
    # Softer upper partials than the success chime: rounder, never sharp.
    return $amplitude * $attack * (
        [Math]::Sin($tau * $frequency * $time) * [Math]::Exp(-$time / 0.11) +
        0.08 * [Math]::Sin($tau * $frequency * 2 * $time) * [Math]::Exp(-$time / 0.045) +
        0.015 * [Math]::Sin($tau * $frequency * 3 * $time) * [Math]::Exp(-$time / 0.025))
}

$count = [int][Math]::Round($sampleRate * $duration)
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$ascii = [System.Text.Encoding]::ASCII
$writer.Write($ascii.GetBytes('RIFF')); $writer.Write([int](36 + $count * 2)); $writer.Write($ascii.GetBytes('WAVE'))
$writer.Write($ascii.GetBytes('fmt ')); $writer.Write([int]16); $writer.Write([int16]1); $writer.Write([int16]1)
$writer.Write([int]$sampleRate); $writer.Write([int]($sampleRate * 2)); $writer.Write([int16]2); $writer.Write([int16]16)
$writer.Write($ascii.GetBytes('data')); $writer.Write([int]($count * 2))
$peak = 0.0; $sum = 0.0
for ($index = 0; $index -lt $count; $index++) {
    $time = $index / $sampleRate
    # G5, then E5 a little later with a brief overlap, and a smooth final fade.
    $value = (Get-Bell $time 783.991 0.225) + (Get-Bell ($time - 0.13) 659.255 0.215)
    $remaining = $duration - $time
    $fade = 0.5 - 0.5 * [Math]::Cos([Math]::PI * [Math]::Min($remaining / 0.08, 1.0))
    $sample = [int][Math]::Round([Math]::Max(-1.0, [Math]::Min(1.0, $value * $fade)) * 32767)
    $writer.Write([int16]$sample)
    $peak = [Math]::Max($peak, [Math]::Abs($sample) / 32767.0); $sum += ($sample / 32767.0) * ($sample / 32767.0)
}
$writer.Flush()
[System.IO.File]::WriteAllBytes($output, $stream.ToArray())
'Generated {0}: {1} bytes, {2:N2} seconds, peak {3:N3}, rms {4:N3}' -f $output, $stream.Length, $duration, $peak, [Math]::Sqrt($sum / $count)
