# 把桌宠窗口临时挪到指定屏幕坐标（物理像素）截图，再挪回原位。
# 用于对比同一画面在不同缩放比的屏幕上的效果。
# 用法：capture-at.ps1 -X 1200 -Y 300 -Out hi.png
param([int]$X, [int]$Y, [string]$Out)
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DutyMove {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rc);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
[DutyMove]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$target = (Get-Process -Name dutyon-pet -ErrorAction Stop | Select-Object -First 1).Id
$script:hwnd = [IntPtr]::Zero
$cb = [DutyMove+EnumProc]{ param($h, $lp)
    $p = [uint32]0; [DutyMove]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq $target) {
        $sb = New-Object System.Text.StringBuilder 64
        [DutyMove]::GetClassName($h, $sb, 64) | Out-Null
        $rc = New-Object DutyMove+RECT
        [DutyMove]::GetWindowRect($h, [ref]$rc) | Out-Null
 if ($sb.ToString() -like 'GLFW*' -and ($rc.B - $rc.T) -gt 150) { $script:hwnd = $h }
    }
    return $true }
[DutyMove]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'pet window not found' }
$rc = New-Object DutyMove+RECT
[DutyMove]::GetWindowRect($script:hwnd, [ref]$rc) | Out-Null
$flags = 0x0001 -bor 0x0004 -bor 0x0010   # NOSIZE | NOZORDER | NOACTIVATE
[DutyMove]::SetWindowPos($script:hwnd, [IntPtr]::Zero, $X, $Y, 0, 0, $flags) | Out-Null
Start-Sleep -Milliseconds 3500
& (Join-Path $PSScriptRoot 'capture-desktop.ps1') -Out $Out
[DutyMove]::SetWindowPos($script:hwnd, [IntPtr]::Zero, $rc.L, $rc.T, 0, 0, $flags) | Out-Null
Start-Sleep -Milliseconds 1500
Write-Host "restored to ($($rc.L),$($rc.T))"
