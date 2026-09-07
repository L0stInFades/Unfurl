param(
    [string]$Output = (Join-Path $PSScriptRoot '..\src\app\Unfurl.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$outputPath = [System.IO.Path]::GetFullPath($Output)
$parent = [System.IO.Path]::GetDirectoryName($outputPath)
New-Item -ItemType Directory -Force -Path $parent | Out-Null

$size = 256
$bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([System.Drawing.Color]::Transparent)

$background = [System.Drawing.Drawing2D.GraphicsPath]::new()
$radius = 44
$background.AddArc(0, 0, $radius, $radius, 180, 90)
$background.AddArc($size - $radius, 0, $radius, $radius, 270, 90)
$background.AddArc($size - $radius, $size - $radius, $radius, $radius, 0, 90)
$background.AddArc(0, $size - $radius, $radius, $radius, 90, 90)
$background.CloseFigure()
$graphics.FillPath([System.Drawing.Brushes]::RoyalBlue, $background)

$pen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, 18)
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$graphics.DrawLines($pen, [System.Drawing.Point[]]@(
        [System.Drawing.Point]::new(60, 74),
        [System.Drawing.Point]::new(128, 116),
        [System.Drawing.Point]::new(196, 74)))
$graphics.DrawLines($pen, [System.Drawing.Point[]]@(
        [System.Drawing.Point]::new(60, 182),
        [System.Drawing.Point]::new(128, 140),
        [System.Drawing.Point]::new(196, 182)))

$graphics.Dispose()
$background.Dispose()
$pen.Dispose()
$pngPath = [System.IO.Path]::ChangeExtension($outputPath, '.png')
$bitmap.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
& ffmpeg -y -loglevel error -i $pngPath -c:v png $outputPath
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed while encoding the icon." }
Remove-Item -LiteralPath $pngPath -Force
Write-Host "Created $outputPath"
