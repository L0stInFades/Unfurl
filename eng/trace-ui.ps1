#Requires -Version 7.0
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('Light', 'Verbose')][string]$Detail = 'Light'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UnfurlTraceWindow {
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    public static void Resize(IntPtr window, int width, int height) {
        var previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try {
            var dpi = GetDpiForWindow(window);
            SetWindowPos(window, IntPtr.Zero, 0, 0, (int)(width * dpi / 96), (int)(height * dpi / 96), 6);
        } finally { SetThreadDpiAwarenessContext(previous); }
    }
}
'@
$path = [IO.Path]::GetFullPath($Executable)
$trace = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($trace)) | Out-Null
if (Test-Path -LiteralPath $trace) { throw 'Choose a new trace path to preserve previous measurements.' }
$process = $null
$recording = $false
$phases = [Collections.Generic.List[object]]::new()
function Mark([string]$Name) {
    wpr -marker $Name | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "WPR could not record the $Name phase marker." }
    $phases.Add([pscustomobject]@{Name = $Name; Utc = (Get-Date).ToUniversalTime().ToString('o') })
}
function Find-Control([string]$Id) {
    $window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty, $Id))
}
try {
    # WPR refuses an existing recording; never cancel someone else's trace.
    wpr -start "CPU.$Detail" -start "XAMLActivity.$Detail" -start "GPU.$Detail" -filemode
    if ($LASTEXITCODE -ne 0) { throw 'WPR could not start the CPU/XAML/GPU recording.' }
    $recording = $true
    Mark 'Unfurl.Startup'
    $process = Start-Process -FilePath $path -WindowStyle Hidden -PassThru
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process.Refresh()
        if ($process.HasExited) { throw 'Unfurl exited before readiness.' }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            $window = [Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
            $ready = Find-Control 'AddFilesCommand'
        }
        if ($watch.Elapsed.TotalSeconds -gt 20) { throw 'Timed out waiting for the page.' }
        Start-Sleep -Milliseconds 25
    } while (-not $ready -or -not $ready.Current.IsEnabled)
    Mark 'Unfurl.Ready'
    Start-Sleep -Seconds 2
    Mark 'Unfurl.Resize'
    foreach ($round in 1..3) {
        foreach ($width in 1000, 980, 960, 940, 920, 900, 880, 860, 840, 820, 800, 780, 760, 740, 720, 700, 680, 660, 640, 620, 600, 580, 560,
            580, 600, 620, 640, 660, 680, 700, 720, 740, 760, 780, 800, 820, 840, 860, 880, 900, 920, 940, 960, 980, 1000) {
            [UnfurlTraceWindow]::Resize($process.MainWindowHandle, $width, 640)
            Start-Sleep -Milliseconds 20
        }
    }
    [UnfurlTraceWindow]::Resize($process.MainWindowHandle, 900, 640)
    Start-Sleep -Milliseconds 500
    Mark 'Unfurl.OptionsMotion'
    $options = [Windows.Automation.ExpandCollapsePattern](Find-Control 'ArchiveOptions').GetCurrentPattern(
        [Windows.Automation.ExpandCollapsePattern]::Pattern)
    foreach ($round in 1..5) {
        $options.Expand()
        Start-Sleep -Milliseconds 700
        $options.Collapse()
        Start-Sleep -Milliseconds 700
    }
    Mark 'Unfurl.Navigation'
    foreach ($round in 1..3) {
        foreach ($id in 'SettingsNavigation', 'ExtractNavigation', 'CompressNavigation') {
            ([Windows.Automation.SelectionItemPattern](Find-Control $id).GetCurrentPattern(
                [Windows.Automation.SelectionItemPattern]::Pattern)).Select()
            Start-Sleep -Milliseconds 500
        }
    }
    Start-Sleep -Seconds 2
    Mark 'Unfurl.Idle'
    Start-Sleep -Seconds 10
    Mark 'Unfurl.Close'
    $process.CloseMainWindow() | Out-Null
    if (-not $process.WaitForExit(5000)) { throw 'The idle application did not close.' }
    wpr -stop $trace 'Unfurl cached launch, three resize sweeps, five options cycles, navigation and ten-second idle'
    if ($LASTEXITCODE -ne 0) { throw 'WPR could not save the trace.' }
    $recording = $false
    [pscustomobject]@{
        Executable = $path; SHA256 = (Get-FileHash $path).Hash; ProcessId = $process.Id; Phases = $phases; Detail = $Detail
        Method = 'WPR CPU + XAMLActivity + GPU. UI Automation drives fixed interactions. Cached launch; no reboot or cache purge.'
    } | ConvertTo-Json -Depth 5 | Set-Content ([IO.Path]::ChangeExtension($trace, '.json')) -Encoding utf8
} finally {
    if ($process) {
        if (-not $process.HasExited) {
            $process.CloseMainWindow() | Out-Null
            if (-not $process.WaitForExit(3000)) { Stop-Process -Id $process.Id }
        }
        $process.Dispose()
    }
    if ($recording) { wpr -cancel | Out-Null }
}
