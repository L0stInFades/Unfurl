#Requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$RuntimeDirectory = (Join-Path $PSScriptRoot '..\windows-app-runtime')
)

$ErrorActionPreference = 'Stop'
$runtimePath = [System.IO.Path]::GetFullPath($RuntimeDirectory)
if (-not (Test-Path -LiteralPath $runtimePath -PathType Container)) {
    throw "Windows App Runtime payload was not found at $runtimePath. Rebuild with -IncludeWindowsAppRuntime."
}

$framework = Join-Path $runtimePath 'Microsoft.WindowsAppRuntime.2.msix'
$main = Join-Path $runtimePath 'Microsoft.WindowsAppRuntime.Main.2.msix'
$singleton = Join-Path $runtimePath 'Microsoft.WindowsAppRuntime.Singleton.2.msix'
$ddlm = Join-Path $runtimePath 'Microsoft.WindowsAppRuntime.DDLM.2.msix'
foreach ($package in @($framework, $main, $singleton, $ddlm)) {
    if (-not (Test-Path -LiteralPath $package -PathType Leaf)) {
        throw "Required Windows App Runtime package is missing: $package"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $package
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Expected a valid Microsoft-signed runtime package: $package"
    }
}

if (-not $PSCmdlet.ShouldProcess('Current Windows user', 'Install Windows App Runtime 2.4.0 x64')) { return }
# Appx is a Windows module that requires Windows PowerShell compatibility on PowerShell 7.
Import-Module Appx -UseWindowsPowerShell
Add-AppxPackage -Path $framework
$dependencies = @($framework)
foreach ($package in @($main, $singleton, $ddlm)) {
    Add-AppxPackage -Path $package -DependencyPath $dependencies
}
Write-Information 'Windows App Runtime 2.4.0 x64 is installed for the current user.' -InformationAction Continue
