#Requires -Version 7.0
param([string]$Path = (Join-Path $PSScriptRoot '..\artifacts\release'))

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($Path)
$release = Get-Content -LiteralPath (Join-Path $root 'release.json') -Raw | ConvertFrom-Json
[xml]$feed = Get-Content -LiteralPath (Join-Path $root 'Unfurl.appinstaller') -Raw
if ($feed.AppInstaller.Uri -cne "https://github.com/$($release.Repository)/releases/latest/download/Unfurl.appinstaller") {
    throw 'The release feed must use the stable GitHub Releases latest URL.'
}
if ($feed.AppInstaller.Version -ne $release.Version -or $feed.AppInstaller.MainPackage.Version -ne $release.Version) {
    throw 'Release and App Installer versions do not match.'
}
if ($feed.AppInstaller.UpdateSettings.OnLaunch.HoursBetweenUpdateChecks -ne '0' -or
    -not $feed.AppInstaller.UpdateSettings.SelectSingleNode('*[local-name()="AutomaticBackgroundTask"]') -or
    $feed.AppInstaller.UpdateSettings.SelectSingleNode('*[local-name()="ForceUpdateFromAnyVersion"]')) {
    throw 'Expected launch/background update checks with no downgrades.'
}
$checksums = @{}
foreach ($line in (Get-Content -LiteralPath (Join-Path $root 'SHA256SUMS.txt'))) {
    if ($line -notmatch '^([a-f0-9]{64})  ([A-Za-z0-9_.-]+)$') { throw "Invalid checksum entry: $line" }
    if ($checksums.ContainsKey($Matches[2])) { throw 'Duplicate checksum entry.' }
    $checksums[$Matches[2]] = $Matches[1]
}
foreach ($asset in $release.Assets) {
    if ($asset -notmatch '^[A-Za-z0-9_.-]+\.(msix|appx|appinstaller|cer|zip|txt)$') {
        throw "Unexpected release asset: $asset"
    }
    $file = Join-Path $root $asset
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing asset: $asset" }
    if ($asset -ne 'SHA256SUMS.txt' -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $checksums[$asset]) {
        throw "SHA-256 mismatch: $asset"
    }
}
$publicCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $root 'Unfurl.cer'))
if ($publicCertificate.HasPrivateKey -or $publicCertificate.Thumbprint -ne $release.CertificateThumbprint) {
    throw 'The published certificate does not match the release signing identity.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$packages = @($feed.AppInstaller.MainPackage) + @($feed.AppInstaller.Dependencies.Package)
foreach ($package in $packages) {
    $uri = [uri]$package.Uri
    $name = [IO.Path]::GetFileName($uri.AbsolutePath)
    if ($package.Uri -cne "https://github.com/$($release.Repository)/releases/download/$($release.Tag)/$name" -or
        $name -notin $release.Assets) {
        throw "Package URL or asset is not bound to this release: $($package.Uri)"
    }
    $file = Join-Path $root $name
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -cne $package.Publisher) {
        throw "Package signature is not valid/trusted or Publisher differs: $name ($($signature.Status))"
    }
    if ($package.LocalName -eq 'MainPackage' -and
        ($signature.SignerCertificate.Thumbprint -ne $release.CertificateThumbprint -or -not $signature.TimeStamperCertificate)) {
        throw 'The main package must have the expected signer and an RFC 3161 timestamp.'
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($file)
    try {
        $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
        try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        foreach ($attribute in @('Name', 'Publisher', 'Version', 'ProcessorArchitecture')) {
            if ($manifest.Package.Identity.GetAttribute($attribute) -cne $package.GetAttribute($attribute)) {
                throw "$name has a different $attribute than the App Installer feed."
            }
        }
        if ($package.LocalName -eq 'MainPackage') {
            foreach ($dependency in $manifest.Package.Dependencies.PackageDependency) {
                $provided = @($feed.AppInstaller.Dependencies.Package | Where-Object {
                    $_.Name -ceq $dependency.Name -and $_.Publisher -ceq $dependency.Publisher -and
                    [version]$_.Version -ge [version]$dependency.MinVersion
                })
                if ($provided.Count -ne 1) { throw "Missing framework: $($dependency.Name)" }
            }
        }
    } finally { $archive.Dispose() }
}
Write-Host "Verified $($release.Tag): trusted signatures, timestamp, identities, dependencies, update policy, URLs and SHA-256."
