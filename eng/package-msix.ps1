#Requires -Version 7.0
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{40}$')][string]$CertificateThumbprint,
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository = 'L0stInFades/Unfurl',
    [version]$Version,
    [string]$Output = 'artifacts/release',
    [uri]$AppInstallerUri,
    [uri]$PackageBaseUri,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repo $Output))
$certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
[xml]$manifest = Get-Content -LiteralPath (Join-Path $repo 'installer\AppxManifest.xml') -Raw
if (-not $certificate.HasPrivateKey -or $certificate.Subject -cne $manifest.Package.Identity.Publisher -or
    $certificate.NotAfter -le (Get-Date) -or $certificate.NotBefore -gt (Get-Date)) {
    throw 'The signing certificate must be valid, have a private key, and exactly match the manifest Publisher.'
}
$sdk = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' -and (Test-Path (Join-Path $_.FullName 'x64\makeappx.exe')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Install the Windows SDK with MakeAppx and SignTool.' }
$makeappx = Join-Path $sdk.FullName 'x64\makeappx.exe'
$signtool = Join-Path $sdk.FullName 'x64\signtool.exe'

& (Join-Path $PSScriptRoot 'package-release.ps1') -Output $Output -IncludeWindowsAppRuntime
$portable = Join-Path $artifactRoot 'Unfurl'
if (-not $Version) { $Version = [version](Get-Item (Join-Path $portable 'Unfurl.exe')).VersionInfo.FileVersion }
if ($Version.Revision -lt 0 -or @($Version.Major, $Version.Minor, $Version.Build, $Version.Revision |
        Where-Object { $_ -gt 65535 }).Count -gt 0) {
    throw 'MSIX requires four version components between 0 and 65535.'
}
$tag = 'v' + $Version.ToString(3)
if ($Version.Revision -ne 0) { $tag += '.' + $Version.Revision }
if (-not $AppInstallerUri) { $AppInstallerUri = "https://github.com/$Repository/releases/latest/download/Unfurl.appinstaller" }
if (-not $PackageBaseUri) { $PackageBaseUri = "https://github.com/$Repository/releases/download/$tag/" }

function Read-PackageManifest([string]$Path) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
        try { return [xml]$reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
}

function Save-Xml([xml]$Document, [string]$Path) {
    $settings = [Xml.XmlWriterSettings]::new()
    $settings.Indent = $true
    $settings.Encoding = [Text.UTF8Encoding]::new($false)
    $writer = [Xml.XmlWriter]::Create($Path, $settings)
    try { $Document.Save($writer) } finally { $writer.Dispose() }
}

$vcPackage = Join-Path $repo '.cache\msix\Microsoft.VCLibs.x64.14.00.Desktop.appx'
if (-not (Test-Path -LiteralPath $vcPackage)) {
    New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($vcPackage)) | Out-Null
    Invoke-WebRequest -Uri 'https://aka.ms/Microsoft.VCLibs.x64.14.00.Desktop.appx' -OutFile $vcPackage
}
$frameworks = @(
    (Join-Path $portable 'windows-app-runtime\Microsoft.WindowsAppRuntime.2.msix'),
    $vcPackage
)
$dependencies = foreach ($framework in $frameworks) {
    $signature = Get-AuthenticodeSignature -LiteralPath $framework
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "The Microsoft framework signature is invalid: $framework"
    }
    $dependencyManifest = Read-PackageManifest $framework
    if ($dependencyManifest.Package.Properties.Framework -ne 'true' -or
        $dependencyManifest.Package.Identity.ProcessorArchitecture -ne 'x64') {
        throw "Expected an x64 framework package: $framework"
    }
    $identity = $dependencyManifest.Package.Identity
    $assetName = "$($identity.Name)_$($identity.Version)_x64$([IO.Path]::GetExtension($framework))"
    Copy-Item -LiteralPath $framework -Destination (Join-Path $artifactRoot $assetName) -Force
    [pscustomobject]@{ Identity = $identity; AssetName = $assetName }
}

$stage = Join-Path $artifactRoot 'msix-stage'
if (-not $stage.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'MSIX staging must stay inside the repository.'
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Get-ChildItem -LiteralPath $portable -File | Where-Object {
    $_.Name -notmatch '^(msvcp|vcruntime|vccorlib|concrt).*\.dll$'
} | Copy-Item -Destination $stage
Copy-Item -LiteralPath (Join-Path $portable 'licenses') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $portable 'docs') -Destination $stage -Recurse

Add-Type -AssemblyName System.Drawing
$assets = Join-Path $stage 'Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
$icon = [Drawing.Icon]::new((Join-Path $repo 'src\app\Unfurl.ico'), 256, 256)
$sourceImage = $icon.ToBitmap()
try {
    foreach ($asset in @(@{ Name = 'StoreLogo'; Size = 50 }, @{ Name = 'Square44x44Logo'; Size = 44 },
            @{ Name = 'Square150x150Logo'; Size = 150 })) {
        $bitmap = [Drawing.Bitmap]::new($asset.Size, $asset.Size)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.DrawImage($sourceImage, 0, 0, $asset.Size, $asset.Size)
            $bitmap.Save((Join-Path $assets ($asset.Name + '.png')), [Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $sourceImage.Dispose(); $icon.Dispose() }

$manifest.Package.Identity.SetAttribute('Version', $Version.ToString())
foreach ($dependency in $dependencies) {
    $node = $manifest.CreateElement('PackageDependency', $manifest.DocumentElement.NamespaceURI)
    $node.SetAttribute('Name', $dependency.Identity.Name)
    $node.SetAttribute('Publisher', $dependency.Identity.Publisher)
    $node.SetAttribute('MinVersion', $dependency.Identity.Version)
    $manifest.Package.Dependencies.AppendChild($node) | Out-Null
}
Save-Xml $manifest (Join-Path $stage 'AppxManifest.xml')
$msixName = "Unfurl_$($Version)_x64.msix"
$msix = Join-Path $artifactRoot $msixName
& $makeappx pack /d $stage /p $msix /o
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx validation or packaging failed.' }
& $signtool sign /fd SHA256 /sha1 $CertificateThumbprint /tr $TimestampUrl /td SHA256 $msix
if ($LASTEXITCODE -ne 0) { throw 'MSIX signing or timestamping failed.' }
Export-Certificate -Cert $certificate -FilePath (Join-Path $artifactRoot 'Unfurl.cer') -Type CERT -Force | Out-Null

[xml]$appInstaller = '<?xml version="1.0" encoding="utf-8"?><AppInstaller xmlns="http://schemas.microsoft.com/appx/appinstaller/2021" />'
$root = $appInstaller.DocumentElement
$root.SetAttribute('Version', $Version.ToString())
$root.SetAttribute('Uri', $AppInstallerUri.AbsoluteUri)
$main = $appInstaller.CreateElement('MainPackage', $root.NamespaceURI)
foreach ($attribute in @('Name', 'Publisher', 'Version', 'ProcessorArchitecture')) {
    $main.SetAttribute($attribute, $manifest.Package.Identity.GetAttribute($attribute))
}
$main.SetAttribute('Uri', [uri]::new($PackageBaseUri, $msixName).AbsoluteUri)
$root.AppendChild($main) | Out-Null
$dependencyRoot = $appInstaller.CreateElement('Dependencies', $root.NamespaceURI)
foreach ($dependency in $dependencies) {
    $node = $appInstaller.CreateElement('Package', $root.NamespaceURI)
    foreach ($attribute in @('Name', 'Publisher', 'Version', 'ProcessorArchitecture')) {
        $node.SetAttribute($attribute, $dependency.Identity.GetAttribute($attribute))
    }
    $node.SetAttribute('Uri', [uri]::new($PackageBaseUri, $dependency.AssetName).AbsoluteUri)
    $dependencyRoot.AppendChild($node) | Out-Null
}
$root.AppendChild($dependencyRoot) | Out-Null
$updates = $appInstaller.CreateElement('UpdateSettings', $root.NamespaceURI)
$onLaunch = $appInstaller.CreateElement('OnLaunch', $root.NamespaceURI)
$onLaunch.SetAttribute('HoursBetweenUpdateChecks', '0')
$updates.AppendChild($onLaunch) | Out-Null
$updates.AppendChild($appInstaller.CreateElement('AutomaticBackgroundTask', $root.NamespaceURI)) | Out-Null
$root.AppendChild($updates) | Out-Null
Save-Xml $appInstaller (Join-Path $artifactRoot 'Unfurl.appinstaller')

$assetNames = @($msixName, 'Unfurl.appinstaller', 'Unfurl.cer', 'Unfurl-Release.zip') + @($dependencies.AssetName)
$checksums = foreach ($name in ($assetNames | Sort-Object)) {
    $hash = Get-FileHash -LiteralPath (Join-Path $artifactRoot $name) -Algorithm SHA256
    "$($hash.Hash.ToLowerInvariant())  $name"
}
[IO.File]::WriteAllLines((Join-Path $artifactRoot 'SHA256SUMS.txt'), $checksums, [Text.UTF8Encoding]::new($false))
[pscustomobject]@{
    Repository = $Repository
    Tag = $tag
    Version = $Version.ToString()
    CertificateThumbprint = $CertificateThumbprint.ToUpperInvariant()
    AppInstallerUri = $AppInstallerUri.AbsoluteUri
    Assets = $assetNames + 'SHA256SUMS.txt'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $artifactRoot 'release.json') -Encoding utf8NoBOM
Write-Host "Created signed MSIX and App Installer feed in $artifactRoot"
