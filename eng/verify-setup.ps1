#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateSet('Cancel', 'ActiveCancel', 'Retry', 'Handoff')][string]$ExpectedOutcome = 'Cancel',
    [string]$Output = (Join-Path $PSScriptRoot '../artifacts/setup-verification')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$run = Join-Path ([IO.Path]::GetFullPath($Output)) (Get-Date -Format 'yyyyMMdd-HHmmss')
$isolated = Join-Path $run '单文件安装测试'
New-Item -ItemType Directory -Force -Path $isolated | Out-Null
$exe = Join-Path $isolated 'UnfurlSetup.exe'
Copy-Item -LiteralPath $Executable -Destination $exe
$tempBefore = @(Get-ChildItem ([IO.Path]::GetTempPath()) -Directory -Filter 'UnfurlSetup-*' | ForEach-Object FullName)
$installersBefore = @(Get-Process AppInstaller -ErrorAction SilentlyContinue | ForEach-Object Id)
$process = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru
function Find-Setup([string]$Id) {
    $process.Refresh()
    if ($process.HasExited) { throw "Setup exited with $($process.ExitCode)." }
    $handle = $process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $null }
    $window = [Windows.Automation.AutomationElement]::FromHandle($handle)
    return $window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty, $Id))
}
function Invoke-Setup([string]$Id) {
    $control = Find-Setup $Id
    if (-not $control) { throw "Missing setup control: $Id" }
    ([Windows.Automation.InvokePattern]$control.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)).Invoke()
}
try {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $continue = Find-Setup 'ContinueSetup'
        if ($watch.Elapsed.TotalSeconds -gt 20) { throw 'The WinUI setup page did not become ready.' }
        Start-Sleep -Milliseconds 50
    } while (-not $continue)
    & (Join-Path $PSScriptRoot 'capture-window.ps1') -Executable $exe -ExistingProcessId $process.Id `
        -Output (Join-Path $run 'setup.png') -LeaveOpen | Out-Null
    if ($ExpectedOutcome -eq 'Cancel') {
        Invoke-Setup 'CancelSetup'
        if (-not $process.WaitForExit(5000)) { throw 'Cancel did not close idle setup.' }
        if (@(Get-Process AppInstaller -ErrorAction SilentlyContinue | Where-Object Id -notin $installersBefore).Count) {
            throw 'App Installer was opened before the user continued.'
        }
    } else {
        Invoke-Setup 'ContinueSetup'
        $watch.Restart()
        if ($ExpectedOutcome -eq 'ActiveCancel') {
            do {
                $status = (Find-Setup 'SetupStatus').Current.Name
                if ($watch.Elapsed.TotalSeconds -gt 30) { throw "No dependency download to cancel: $status" }
                if ($status -notlike '正在下载*') { Start-Sleep -Milliseconds 50 }
            } while ($status -notlike '正在下载*')
            Invoke-Setup 'CancelSetup'
            $watch.Restart()
        }
        do {
            $button = Find-Setup 'ContinueSetup'
            $status = (Find-Setup 'SetupStatus').Current.Name
            if ($watch.Elapsed.TotalSeconds -gt 180) { throw "Setup preparation timed out: $status" }
            $terminal = $button.Current.IsEnabled -and ($ExpectedOutcome -eq 'ActiveCancel' ? $status -like '已取消*' : $button.Current.Name -in '重试', '完成')
            if (-not $terminal) { Start-Sleep -Milliseconds 100 }
        } while (-not $terminal)
        & (Join-Path $PSScriptRoot 'capture-window.ps1') -Executable $exe -ExistingProcessId $process.Id `
            -Output (Join-Path $run 'result.png') -LeaveOpen | Out-Null
        if ($ExpectedOutcome -in 'Retry', 'ActiveCancel') {
            if ($ExpectedOutcome -eq 'Retry' -and ($button.Current.Name -ne '重试' -or $status -notmatch '0x')) {
                throw "Expected a retryable error: $status"
            }
            if (@(Get-Process AppInstaller -ErrorAction SilentlyContinue | Where-Object Id -notin $installersBefore).Count) {
                throw 'Failed preparation opened App Installer.'
            }
        } else {
            if ($button.Current.Name -ne '完成' -or $status -notmatch 'Windows 安装窗口') {
                throw "Setup did not hand off to Windows: $status"
            }
            $watch.Restart()
            do {
                $installer = Get-Process AppInstaller -ErrorAction SilentlyContinue |
                    Where-Object { $_.Id -notin $installersBefore -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                if (-not $installer) { Start-Sleep -Milliseconds 100 }
            } while (-not $installer -and $watch.Elapsed.TotalSeconds -lt 20)
            if (-not $installer) { throw 'The Windows App Installer process did not start.' }
            # Wait for the actual confirmation page, not just its loading window.
            $process.CloseMainWindow() | Out-Null
            if (-not $process.WaitForExit(5000)) { throw 'Finished setup did not close.' }
            $watch.Restart()
            do {
                $installer.Refresh()
                $page = [Windows.Automation.AutomationElement]::FromHandle($installer.MainWindowHandle)
                $controls = $page.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
                $names = @($controls | ForEach-Object { $_.Current.Name })
                $action = @($controls | Where-Object {
                        $_.Current.ControlType -eq [Windows.Automation.ControlType]::Button -and
                        $_.Current.Name -match '^(安装|重新安装|启动|打开|更新|Install|Reinstall|Launch|Open|Update)'
                    })
                $ready = $action.Count -gt 0 -and ($names -join ' ') -match 'Unfurl'
                if (-not $ready) { Start-Sleep -Milliseconds 500 }
            } while (-not $ready -and $watch.Elapsed.TotalSeconds -lt 90)
            if (-not $ready) { throw "App Installer did not show its confirmation page: $($names -join ' ')" }
            & (Join-Path $PSScriptRoot 'capture-window.ps1') -ExistingProcessId $installer.Id `
                -Output (Join-Path $run 'windows-confirmation.png') -LeaveOpen | Out-Null
            # Leave the actual Install action to Windows and the user; close only this test window.
            $installer.CloseMainWindow() | Out-Null
        }
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(5000)) { throw 'Setup did not close after preparation.' }
    }
    $newTemp = @(Get-ChildItem ([IO.Path]::GetTempPath()) -Directory -Filter 'UnfurlSetup-*' | Where-Object FullName -notin $tempBefore)
    foreach ($directory in $newTemp) {
        $remaining = @(Get-ChildItem -LiteralPath $directory.FullName -Recurse -File)
        if ($ExpectedOutcome -ne 'Handoff' -or @($remaining | Where-Object Extension -ne '.appinstaller').Count) {
            throw "Setup left temporary payloads: $($directory.FullName)"
        }
    }
    [pscustomobject]@{ Outcome = $ExpectedOutcome; Status = $status; ExecutableSHA256 = (Get-FileHash $exe).Hash; Passed = $true } |
        ConvertTo-Json | Set-Content (Join-Path $run 'result.json') -Encoding utf8
    Write-Information -InformationAction Continue "Standalone setup $ExpectedOutcome verification passed: $run"
} finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    $process.Dispose()
}
