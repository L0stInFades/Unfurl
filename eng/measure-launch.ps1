#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows-release\Unfurl.exe'),
    [ValidateRange(1, 50)][int]$Samples = 5,
    [ValidateRange(1, 60)][int]$SettleSeconds = 10,
    [ValidateRange(1, 60)][int]$CpuSeconds = 30,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$path = [System.IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $path)) { throw "Build $path before measuring launch." }

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class UnfurlMemory {
    [StructLayout(LayoutKind.Sequential)] public struct Counters {
        public uint cb, faults;
        public UIntPtr peakWorkingSet, workingSet, peakPaged, paged, peakNonPaged, nonPaged;
        public UIntPtr pagefile, peakPagefile, privateBytes, privateWorkingSet, sharedCommit;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool K32GetProcessMemoryInfo(IntPtr process, ref Counters counters, uint size);
    public static Counters Read(IntPtr process) {
        var value = new Counters();
        value.cb = (uint)Marshal.SizeOf<Counters>();
        if (!K32GetProcessMemoryInfo(process, ref value, value.cb)) throw new Win32Exception();
        return value;
    }
}
'@
$readyCondition = [Windows.Automation.PropertyCondition]::new(
    [Windows.Automation.AutomationElement]::AutomationIdProperty, 'AddFilesCommand')
$measurements = for ($index = 0; $index -lt $Samples; $index++) {
    $start = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $path -PassThru -WindowStyle Hidden
    try {
        $firstWindow = $null
        $ready = $false
        do {
            $process.Refresh()
            if ($process.HasExited) { throw "Unfurl exited before readiness: $($process.ExitCode)" }
            $windowHandle = $process.MainWindowHandle
            if ($windowHandle -ne [IntPtr]::Zero) {
                if ($null -eq $firstWindow) { $firstWindow = $start.Elapsed.TotalMilliseconds }
                $element = [Windows.Automation.AutomationElement]::FromHandle($windowHandle)
                $command = $element.FindFirst([Windows.Automation.TreeScope]::Descendants, $readyCondition)
                $ready = $command -and $command.Current.IsEnabled -and -not $command.Current.IsOffscreen
            }
            if ($start.Elapsed.TotalSeconds -gt 20) { throw 'Timed out waiting for the interactive WinUI page.' }
            if (-not $ready) { Start-Sleep -Milliseconds 10 }
        } while (-not $ready)
        $interactiveMs = $start.Elapsed.TotalMilliseconds
        Start-Sleep -Seconds $SettleSeconds
        $process.Refresh()
        $memory = [UnfurlMemory]::Read($process.Handle)
        $cpuBefore = $process.TotalProcessorTime.TotalMilliseconds
        $cpuWatch = [Diagnostics.Stopwatch]::StartNew()
        Start-Sleep -Seconds $CpuSeconds
        $process.Refresh()
        $cpuMs = $process.TotalProcessorTime.TotalMilliseconds - $cpuBefore
        [pscustomobject]@{
            Sample = $index + 1
            FirstWindowMs = [math]::Round($firstWindow, 2)
            InteractiveMs = [math]::Round($interactiveMs, 2)
            PrivateWorkingSetMiB = [math]::Round($memory.privateWorkingSet.ToUInt64() / 1MB, 2)
            PrivateCommitMiB = [math]::Round($memory.privateBytes.ToUInt64() / 1MB, 2)
            WorkingSetMiB = [math]::Round($memory.workingSet.ToUInt64() / 1MB, 2)
            IdleCpuMs = $cpuMs
            IdleOneCorePercent = [math]::Round(100 * $cpuMs / $cpuWatch.Elapsed.TotalMilliseconds, 3)
        }
    } finally {
        if (-not $process.HasExited) {
            $process.CloseMainWindow() | Out-Null
            if (-not $process.WaitForExit(3000)) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                $process.WaitForExit(1000) | Out-Null
            }
        }
        $process.Dispose()
    }
}
$measurements | Format-Table
if ($Output) {
    [pscustomobject]@{
        Executable = $path
        SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        MeasuredAt = (Get-Date).ToString('o')
        SettleSeconds = $SettleSeconds
        CpuSeconds = $CpuSeconds
        Method = 'UI Automation enabled/visible command; cached launches, not reboot-cold startup or first-pixel timing.'
        Samples = @($measurements)
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Output -Encoding utf8
}
