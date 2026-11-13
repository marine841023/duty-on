# 放大截图局部（最近邻，不做平滑）看像素级细节
# 用法：zoom.ps1 -In a.png -Out b.png -X 0 -Y 0 -W 130 -H 130 -Scale 4
param([string]$In, [string]$Out, [int]$X = 0, [int]$Y = 0, [int]$W = 100, [int]$H = 100, [int]$Scale = 4)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In))
$dst = New-Object System.Drawing.Bitmap(($W * $Scale), ($H * $Scale))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, ($W * $Scale), ($H * $Scale))), $X, $Y, $W, $H, [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose(); $src.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $dst.Dispose()
