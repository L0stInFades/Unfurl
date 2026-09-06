$ErrorActionPreference = 'Stop'

$version = '2.4.0'
$root = Join-Path $PSScriptRoot '..\.cache\windowsappsdk\2.4.0'
$root = [System.IO.Path]::GetFullPath($root)
$packages = @(
    @{ Id = 'microsoft.windowsappsdk.foundation'; Version = '2.3.9' },
    @{ Id = 'microsoft.windowsappsdk.interactiveexperiences'; Version = '2.1.6' },
    @{ Id = 'microsoft.windowsappsdk.winui'; Version = '2.3.6' },
    @{ Id = 'microsoft.windowsappsdk.runtime'; Version = $version },
    @{ Id = 'microsoft.web.webview2'; Version = '1.0.4078.44' }
)

if (-not (Test-Path (Join-Path $root 'include\MddBootstrap.h')) -or
    -not (Test-Path (Join-Path $root 'metadata\10.0.18362.0\Microsoft.UI.winmd')) -or
    -not (Test-Path (Join-Path $root 'lib\Microsoft.Web.WebView2.Core.winmd'))) {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    foreach ($item in $packages) {
        $package = Join-Path $env:TEMP "$($item.Id).$($item.Version).nupkg"
        $uri = "https://api.nuget.org/v3-flatcontainer/$($item.Id)/$($item.Version)/$($item.Id).$($item.Version).nupkg"
        Invoke-WebRequest -Uri $uri -OutFile $package
        Expand-Archive -LiteralPath $package -DestinationPath $root -Force
        Remove-Item -LiteralPath $package -Force
    }
}

Write-Host "Windows App SDK $version restored to $root"
