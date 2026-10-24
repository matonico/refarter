# Rebuild the Windows icon from the simple rect/polygon logo in assets/refarter.svg.
# The checked-in .ico means normal C++ builds do not need this script.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$projectRoot = Split-Path -Parent $PSScriptRoot
[xml]$logo = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'assets/refarter.svg')
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.ScaleTransform($size / 64.0, $size / 64.0)
    $rect = $logo.svg.rect
    $x = [float]$rect.x; $y = [float]$rect.y
    $width = [float]$rect.width; $height = [float]$rect.height
    $diameter = [float]$rect.rx * 2
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc($x + $width - $diameter, $y, $diameter, $diameter, 270, 90)
    $path.AddArc($x + $width - $diameter, $y + $height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($x, $y + $height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    $orange = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($rect.fill))
    $dark = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($logo.svg.polygon.fill))
    $points = [Drawing.PointF[]]@($logo.svg.polygon.points.Split(' ') | ForEach-Object {
        $xy = $_.Split(',')
        [Drawing.PointF]::new([float]$xy[0], [float]$xy[1])
    })
    $graphics.FillPath($orange, $path)
    $graphics.FillPolygon($dark, $points)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    $images += ,$stream.ToArray()
    $stream.Dispose(); $dark.Dispose(); $orange.Dispose(); $path.Dispose()
    $graphics.Dispose(); $bitmap.Dispose()
}
$output = [IO.File]::Create((Join-Path $projectRoot 'assets/refarter.ico'))
$writer = [IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($imageBytes in $images) { $writer.Write([byte[]]$imageBytes) }
} finally { $writer.Dispose() }
Write-Host 'generated assets/refarter.ico (9 sizes, 16–256px)'
