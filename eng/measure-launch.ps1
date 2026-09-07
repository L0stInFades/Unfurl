param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows-release\Unfurl.exe'),
    [int]$Samples = 5
)

$ErrorActionPreference = 'Stop'
$path = [System.IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $path)) { throw "Build $path before measuring launch." }

for ($index = 0; $index -lt $Samples; $index++) {
    $start = [System.Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $path -PassThru
    do {
        Start-Sleep -Milliseconds 10
        $process.Refresh()
    } while (-not $process.MainWindowHandle -and -not $process.HasExited)
    $start.Stop()
    $workingSet = if ($process.HasExited) { 0 } else { $process.WorkingSet64 }
    [pscustomobject]@{ Sample = $index + 1; FirstWindowMs = $start.Elapsed.TotalMilliseconds; WorkingSetMiB = [math]::Round($workingSet / 1MB, 1) }
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(2000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(1000) | Out-Null
        }
    }
}
