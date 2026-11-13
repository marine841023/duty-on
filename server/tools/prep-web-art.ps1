# 把透明底的角色渲染图裁到内容边界并缩放，输出给官网使用。
# 用法：prep-web-art.ps1 -In <png> -Out <png> [-Height 520]
param([string]$In, [string]$Out, [int]$Height = 520)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In).Path)
$rect = New-Object System.Drawing.Rectangle(0, 0, $src.Width, $src.Height)
$data = $src.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($data.Stride * $src.Height)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$src.UnlockBits($data)
$x0 = $src.Width; $y0 = $src.Height; $x1 = 0; $y1 = 0
for ($y = 0; $y -lt $src.Height; $y++) {
    $row = $y * $data.Stride
    for ($x = 0; $x -lt $src.Width; $x++) {
        if ($bytes[$row + $x * 4 + 3] -gt 8) {
            if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }
     if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y }
        }
    }
}
$pad = 6
$x0 = [Math]::Max(0, $x0 - $pad); $y0 = [Math]::Max(0, $y0 - $pad)
$x1 = [Math]::Min($src.Width - 1, $x1 + $pad); $y1 = [Math]::Min($src.Height - 1, $y1 + $pad)
$cw = $x1 - $x0 + 1; $ch = $y1 - $y0 + 1
$scale = [Math]::Min(1.0, $Height / $ch)
$ow = [int]($cw * $scale); $oh = [int]($ch * $scale)
$dst = New-Object System.Drawing.Bitmap($ow, $oh, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $ow, $oh)), $x0, $y0, $cw, $ch, [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose(); $src.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $dst.Dispose()
Write-Host "$Out ${ow}x${oh}"
