#Requires -Version 7.0
<#
.SYNOPSIS
Reports Windows App Installer enrollment and update availability.
.DESCRIPTION
Runs in PowerShell 7. The Windows WinRT projection requires .NET Framework,
so only that query runs in an explicit Windows PowerShell compatibility session.
The query is read-only and does not install, update, or close the application.
#>
[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$Name = 'L0stInFades.Unfurl',
    [ValidateSet('', 'NoUpdates', 'Available', 'Required', 'Unknown', 'Error')][string]$ExpectedAvailability = ''
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'The update diagnostic requires Windows.' }
$session = New-PSSession -UseWindowsPowerShell
$packageToInspect = $Name
try {
    $result = Invoke-Command -Session $session -ScriptBlock {
        $PackageName = $using:packageToInspect
        $ErrorActionPreference = 'Stop'
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $manager = [Windows.Management.Deployment.PackageManager, Windows.Management.Deployment, ContentType = WindowsRuntime]::new()
        $installed = Get-AppxPackage -Name $PackageName
        if (-not $installed) { throw "$PackageName is not installed for this user." }
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
        $availability = $task.Result
        [pscustomobject]@{
            Package = $installed.PackageFullName
            Version = $installed.Version
            AppInstallerUri = $installer.Uri.AbsoluteUri
            Availability = $availability.Availability.ToString()
            ExtendedError = [string]$availability.ExtendedError
        }
    }
    if ($ExpectedAvailability -and $result.Availability -ne $ExpectedAvailability) {
        throw "Expected $ExpectedAvailability, received $($result.Availability): $($result.ExtendedError)"
    }
    $result | Select-Object Package, Version, AppInstallerUri, Availability, ExtendedError | ConvertTo-Json
} finally {
    Remove-PSSession -Session $session
}
