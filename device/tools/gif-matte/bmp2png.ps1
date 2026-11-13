# BMP → PNG（查看 dump.js 的输出用）。用法：bmp2png.ps1 a.bmp [b.bmp ...]
Add-Type -AssemblyName System.Drawing
foreach ($f in $args) {
    $p = (Resolve-Path $f).Path
    $b = [System.Drawing.Bitmap]::FromFile($p)
    $b.Save([IO.Path]::ChangeExtension($p, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
}
