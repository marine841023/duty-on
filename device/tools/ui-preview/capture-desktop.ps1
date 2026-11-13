# 截取桌面端桌宠窗口（PrintWindow）。窗口因设备在线被自动隐藏时，临时显示再还原。
# 用法：powershell -File capture-desktop.ps1 -Out <png>
param([string]$Out)
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DutyCap {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rc);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
[DutyCap]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Drawing
$target = (Get-Process -Name dutyon-pet -ErrorAction Stop | Select-Object -First 1).Id
$script:hwnd = [IntPtr]::Zero
$cb = [DutyCap+EnumProc]{ param($h, $lp)
    $p = [uint32]0; [DutyCap]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq $target) {
        $sb = New-Object System.Text.StringBuilder 64
        [DutyCap]::GetClassName($h, $sb, 64) | Out-Null
        if ($sb.ToString() -like 'GLFW*') {
     $rc = New-Object DutyCap+RECT
            [DutyCap]::GetWindowRect($h, [ref]$rc) | Out-Null
            if (($rc.B - $rc.T) -gt 150) { $script:hwnd = $h }
        }
    }
    return $true }
[DutyCap]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'pet window not found' }
$wasVisible = [DutyCap]::IsWindowVisible($script:hwnd)
if (-not $wasVisible) { [DutyCap]::ShowWindow($script:hwnd, 4) | Out-Null; Start-Sleep -Milliseconds 1500 }
$rc = New-Object DutyCap+RECT
[DutyCap]::GetWindowRect($script:hwnd, [ref]$rc) | Out-Null
$w = $rc.R - $rc.L; $h = $rc.B - $rc.T
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(255, 58, 62, 70))
$hdc = $g.GetHdc()
[DutyCap]::PrintWindow($script:hwnd, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
if (-not $wasVisible) { [DutyCap]::ShowWindow($script:hwnd, 0) | Out-Null }
Write-Host "saved $Out ($w x $h, wasVisible=$wasVisible)"
