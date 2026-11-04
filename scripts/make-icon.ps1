# Draws the WSL Manager application icon and writes src\gui\app.ico.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make-icon.ps1 [-Preview <folder>]
#
# The icon is a Fluent-blue rounded tile with a white shell prompt (">_"). From
# 32 px up, two tiles stacked behind it stand for the instances WSL Manager looks
# after. Each size is drawn separately, so the small ones stay sharp, and stored
# as a PNG frame (supported by Windows Vista and later). -Preview also saves each
# frame as a PNG in that folder.

param([string]$Preview)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$out = Join-Path $PSScriptRoot '..\src\gui\app.ico'

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = [single]$size
    $stacked = $size -ge 32
    # Front tile: the whole canvas at small sizes; bottom-left with room for the stack above.
    $margin = if ($size -le 20) { 0.5 } else { [Math]::Max(1.0, $s * 0.03) }
    $tile = if ($stacked) { $s * 0.80 } else { $s - 2 * $margin }
    $step = ($s - 2 * $margin - $tile) / 2
    $radius = $tile * 0.22

    if ($stacked) {
        # Back tiles, lighter and further back, offset up and to the right.
        $backColors = @([System.Drawing.Color]::FromArgb(255, 0x9C, 0xC9, 0xF0),
                        [System.Drawing.Color]::FromArgb(255, 0x5A, 0xA7, 0xE6))
        for ($i = 2; $i -ge 1; --$i) {
            $x = $margin + $i * $step
            $y = $margin + (2 - $i) * $step
            $p = New-RoundRect $x $y $tile $tile $radius
            $b = New-Object System.Drawing.SolidBrush $backColors[2 - $i]
            $g.FillPath($b, $p)
            $b.Dispose(); $p.Dispose()
        }
    }

    $x0 = $margin
    $y0 = $s - $margin - $tile
    $front = New-RoundRect $x0 $y0 $tile $tile $radius
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.PointF $x0, $y0),
        (New-Object System.Drawing.PointF ($x0 + $tile), ($y0 + $tile)),
        [System.Drawing.Color]::FromArgb(255, 0x2B, 0x88, 0xD8),
        [System.Drawing.Color]::FromArgb(255, 0x0C, 0x4A, 0x86))
    $g.FillPath($grad, $front)
    $grad.Dispose()
    if ($size -ge 48) {
        # A faint top highlight, as on Fluent app icons.
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(70, 255, 255, 255)), ([single]($s / 128))
        $inset = $s / 256
        $hl = New-RoundRect ($x0 + $inset) ($y0 + $inset) ($tile - 2 * $inset) ($tile - 2 * $inset) ($radius - $inset)
        $g.DrawPath($pen, $hl)
        $pen.Dispose(); $hl.Dispose()
    }
    $front.Dispose()

    # The prompt: a chevron and an underscore, centred in the front tile.
    $stroke = [single]([Math]::Max(1.6, $tile * 0.11))
    $white = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), $stroke
    $white.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $white.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $white.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $cx = $x0 + $tile * 0.27
    $cy = $y0 + $tile * 0.50
    $arm = $tile * 0.17
    $points = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF $cx, ($cy - $arm)),
        (New-Object System.Drawing.PointF ($cx + $arm), $cy),
        (New-Object System.Drawing.PointF $cx, ($cy + $arm)))
    $g.DrawLines($white, $points)
    $uy = $cy + $arm
    $g.DrawLine($white, ($x0 + $tile * 0.52), $uy, ($x0 + $tile * 0.75), $uy)
    $white.Dispose()

    $g.Dispose()
    return $bmp
}

$frames = foreach ($size in $sizes) {
    $bmp = Draw-Icon $size
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($Preview) {
        New-Item -ItemType Directory -Force $Preview | Out-Null
        $bmp.Save((Join-Path $Preview "icon-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $bmp.Dispose()
    [pscustomobject]@{ Size = $size; Data = $ms.ToArray() }
}

# ICO file: ICONDIR, one ICONDIRENTRY per frame, then the PNG data.
$file = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $file
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }
    $w.Write([byte]$dim); $w.Write([byte]$dim)
    $w.Write([byte]0); $w.Write([byte]0)            # no palette, reserved
    $w.Write([uint16]1); $w.Write([uint16]32)       # planes, bits per pixel
    $w.Write([uint32]$f.Data.Length); $w.Write([uint32]$offset)
    $offset += $f.Data.Length
}
foreach ($f in $frames) { $w.Write($f.Data) }
$w.Flush()
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($out), $file.ToArray())
"Wrote $([System.IO.Path]::GetFullPath($out)) ($($sizes -join ', ') px)"
