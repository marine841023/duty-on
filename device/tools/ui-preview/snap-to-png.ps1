# 把设备抓回的 32 位 BMP（alpha 可能为 0）转成不透明 PNG；-Rotate 把物理横屏转回竖屏看
param([string]$In, [string]$Out, [switch]$Rotate)
Add-Type -AssemblyName System.Drawing
$bytes = [IO.File]::ReadAllBytes($In)
for ($i = 54 + 3; $i -lt $bytes.Length; $i += 4) { $bytes[$i] = 255 }
$ms = New-Object IO.MemoryStream(,$bytes)
$bmp = New-Object System.Drawing.Bitmap($ms)
if ($Rotate) { $bmp.RotateFlip([System.Drawing.RotateFlipType]::Rotate270FlipNone) }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
