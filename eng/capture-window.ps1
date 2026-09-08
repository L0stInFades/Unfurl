#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows-release\Unfurl.exe'),
    [string]$Output = (Join-Path $PSScriptRoot '..\artifacts\screenshots\unfurl.png'),
    [string[]]$LaunchArguments = @(),
    [int]$ExistingProcessId = 0,
    [int]$Width = 0,
    [int]$Height = 0,
    [switch]$PreserveActivation,
    [switch]$LeaveOpen
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class UnfurlWindowCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height, bool repaint);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr first, IntPtr second);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, uint attribute, out Rect rect, uint size);

}
'@

$executablePath = [System.IO.Path]::GetFullPath($Executable)
$outputPath = [System.IO.Path]::GetFullPath($Output)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($outputPath)) | Out-Null
if ($ExistingProcessId) {
    $process = Get-Process -Id $ExistingProcessId
} else {
    $start = [System.Diagnostics.ProcessStartInfo]::new($executablePath)
    $start.WorkingDirectory = [System.IO.Path]::GetDirectoryName($executablePath)
    $start.UseShellExecute = $false
    foreach ($argument in $LaunchArguments) { $start.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($start)
}
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        if ($process.HasExited) { throw "Unfurl exited before showing its window (code $($process.ExitCode))." }
    } while (-not $process.MainWindowHandle -and [DateTime]::UtcNow -lt $deadline)
    if (-not $process.MainWindowHandle) { throw 'Unfurl did not create a window within 10 seconds.' }
    Start-Sleep -Milliseconds 1500
    $process.Refresh()
    if ($process.HasExited) { throw "Unfurl exited during startup (code $($process.ExitCode))." }
    $hwnd = $process.MainWindowHandle
    $previousDpi = [UnfurlWindowCapture]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
    try {
        if (-not $PreserveActivation -and [UnfurlWindowCapture]::IsIconic($hwnd)) {
            [UnfurlWindowCapture]::ShowWindow($hwnd, 9) | Out-Null
        }
        $dpi = [UnfurlWindowCapture]::GetDpiForWindow($hwnd)
        if ($Width -gt 0 -and $Height -gt 0) {
            [UnfurlWindowCapture]::MoveWindow($hwnd, 48, 48, [int]($Width * $dpi / 96), [int]($Height * $dpi / 96), $true) | Out-Null
        }
        if (-not $PreserveActivation) {
            [UnfurlWindowCapture]::SetForegroundWindow($hwnd) | Out-Null
        }
        $hoverRect = [UnfurlWindowCapture+Rect]::new()
        [UnfurlWindowCapture]::GetWindowRect($hwnd, [ref]$hoverRect) | Out-Null
        [UnfurlWindowCapture]::SetCursorPos([int](($hoverRect.Left + $hoverRect.Right) / 2), $hoverRect.Top + 16) | Out-Null
        Start-Sleep -Milliseconds 1200
        $process.Refresh()
        if ($process.HasExited) { throw "Unfurl exited while rendering (code $($process.ExitCode))." }
        $rect = [UnfurlWindowCapture+Rect]::new()
        if (-not [UnfurlWindowCapture]::GetWindowRect($hwnd, [ref]$rect)) { throw 'Cannot read the window bounds.' }
        $visibleFrame = [UnfurlWindowCapture+Rect]::new()
        if ([UnfurlWindowCapture]::DwmGetWindowAttribute($hwnd, 9, [ref]$visibleFrame, 16) -eq 0) {
            $rect = $visibleFrame
        }
        $bitmap = [System.Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size, [System.Drawing.CopyPixelOperation]::SourceCopy)
            $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
        [pscustomobject]@{
            Screenshot = $outputPath
            ProcessId = $process.Id
            Dpi = $dpi
            PerMonitorV2 = [UnfurlWindowCapture]::AreDpiAwarenessContextsEqual(
                [UnfurlWindowCapture]::GetWindowDpiAwarenessContext($hwnd), [IntPtr]::new(-4))
        }
    } finally {
        [UnfurlWindowCapture]::SetThreadDpiAwarenessContext($previousDpi) | Out-Null
    }
} finally {
    if (-not $ExistingProcessId -and -not $LeaveOpen -and -not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() }
    }
}
