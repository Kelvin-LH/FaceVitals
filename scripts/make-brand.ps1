# 仅构建时使用：生成可复现的 PNG 与多尺寸 Windows ICO，无额外运行时依赖。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot '../assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
function New-BrandBitmap([int]$size) {
    $bmp = [System.Drawing.Bitmap]::new($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.ScaleTransform($size / 512.0, $size / 512.0)
    $shape = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $shape.AddArc(8,8,112,112,180,90); $shape.AddArc(392,8,112,112,270,90)
    $shape.AddArc(392,392,112,112,0,90); $shape.AddArc(8,392,112,112,90,90); $shape.CloseFigure()
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(0,0),[System.Drawing.Point]::new(512,512),[System.Drawing.ColorTranslator]::FromHtml('#173b50'),[System.Drawing.ColorTranslator]::FromHtml('#081322'))
    $g.FillPath($brush,$shape)
    $border = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#2d6474'),5)
    $g.DrawPath($border,$shape)
    $face = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $face.AddBezier(315,117,215,65,119,137,142,233)
    $face.AddBezier(142,233,145,272,173,299,185,329)
    $face.AddLine(185,329,185,391)
    $face.AddLine(185,391,294,391)
    $face.AddLine(294,391,294,346)
    $face.AddBezier(294,346,341,349,350,328,347,300)
    $face.AddLine(347,300,375,286)
    $face.AddLine(375,286,342,240)
    $face.AddBezier(342,240,349,209,345,181,332,160)
    $pen = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#5ef2d6'),19)
    $pen.StartCap = $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPath($pen,$face)
    $wave = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(80,263),[System.Drawing.PointF]::new(200,263),[System.Drawing.PointF]::new(222,233),[System.Drawing.PointF]::new(245,299),[System.Drawing.PointF]::new(268,210),[System.Drawing.PointF]::new(290,263),[System.Drawing.PointF]::new(421,263))
    $under = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#0d2030'),34)
    $under.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawLines($under,$wave)
    $signal = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#8bbcff'),15)
    $signal.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $signal.StartCap = $signal.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLines($signal,$wave)
    $g.Dispose(); $shape.Dispose(); $face.Dispose(); $brush.Dispose(); $border.Dispose(); $pen.Dispose(); $under.Dispose(); $signal.Dispose()
    return $bmp
}
$large = New-BrandBitmap 512
$large.Save((Join-Path $assets 'facevitals.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$large.Dispose()
$frames = @()
foreach ($size in @(16,24,32,48,64,128,256)) {
    $bmp = New-BrandBitmap $size
    $stream = [IO.MemoryStream]::new()
    $bmp.Save($stream,[System.Drawing.Imaging.ImageFormat]::Png)
    $frames += [PSCustomObject]@{size=$size;data=$stream.ToArray()}
    $stream.Dispose(); $bmp.Dispose()
}
$file = [IO.File]::Create((Join-Path $assets 'facevitals.ico'))
$writer = [IO.BinaryWriter]::new($file)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($frame in $frames) {
    $edge = if ($frame.size -eq 256) { 0 } else { $frame.size }
    $writer.Write([byte]$edge); $writer.Write([byte]$edge); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$frame.data.Length); $writer.Write([uint32]$offset)
    $offset += $frame.data.Length
}
foreach ($frame in $frames) { $writer.Write([byte[]]$frame.data) }
$writer.Dispose(); $file.Dispose()
