#Requires -Version 5.1
param(
    [string]$Name = 'L0stInFades.Unfurl',
    [ValidateSet('', 'NoUpdates', 'Available', 'Required', 'Unknown', 'Error')][string]$ExpectedAvailability = ''
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Core') {
    throw 'Run this WinRT diagnostic with Windows PowerShell: powershell.exe -NoProfile -File eng/inspect-update.ps1'
}
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$manager = [Windows.Management.Deployment.PackageManager, Windows.Management.Deployment, ContentType = WindowsRuntime]::new()
$installed = Get-AppxPackage -Name $Name
if (-not $installed) { throw "$Name is not installed for this user." }
$package = $manager.FindPackageForUser('', $installed.PackageFullName)
$installer = $package.GetAppInstallerInfo()
if (-not $installer) { throw 'The package is not enrolled through App Installer.' }
$resultType = [Windows.ApplicationModel.PackageUpdateAvailabilityResult, Windows.ApplicationModel, ContentType = WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetGenericArguments().Count -eq 1 -and
    $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
$operation = $package.CheckUpdateAvailabilityAsync()
$task = $asTask.MakeGenericMethod($resultType).Invoke($null, @($operation))
if (-not $task.Wait(30000)) { throw 'The Windows update check did not finish within 30 seconds.' }
$result = $task.Result
if ($ExpectedAvailability -and $result.Availability.ToString() -ne $ExpectedAvailability) {
    throw "Expected $ExpectedAvailability, received $($result.Availability): $($result.ExtendedError)"
}
[pscustomobject]@{
    Package = $installed.PackageFullName
    Version = $installed.Version
    AppInstallerUri = $installer.Uri.AbsoluteUri
    Availability = $result.Availability.ToString()
    ExtendedError = $result.ExtendedError
} | ConvertTo-Json
