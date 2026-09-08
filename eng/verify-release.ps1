#Requires -Version 7.0
[CmdletBinding()]
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
    if ($asset -notmatch '^[A-Za-z0-9_.-]+\.(msix|appx|appinstaller|cer|zip|exe|txt)$') {
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
if ('UnfurlSetup.exe' -notin $release.Assets) { throw 'The release must include the standalone WinUI 3 installer.' }
$setup = Join-Path $root 'UnfurlSetup.exe'
$setupSignature = Get-AuthenticodeSignature -LiteralPath $setup
if ($setupSignature.Status -ne 'Valid' -or $setupSignature.SignerCertificate.Thumbprint -ne $release.CertificateThumbprint -or
    -not $setupSignature.TimeStamperCertificate -or (Get-Item $setup).VersionInfo.FileVersion -ne $release.Version) {
    throw 'Setup must have the release version, trusted release signer and timestamp.'
}
if ((Get-Item $setup).Length -ge 1MB) { throw 'The thin installer exceeded its 1 MiB budget.' }
Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class UnfurlSetupResources {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryExW(string name, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    public static byte[] Read(string path, int id) {
        var module = LoadLibraryExW(path, IntPtr.Zero, 0x22); // Data only; never execute setup while verifying.
        if (module == IntPtr.Zero) throw new Win32Exception();
        try {
            var info = FindResourceW(module, new IntPtr(id), new IntPtr(10));
            if (info == IntPtr.Zero) throw new Win32Exception();
            var pointer = LockResource(LoadResource(module, info));
            if (pointer == IntPtr.Zero) throw new Win32Exception();
            var bytes = new byte[SizeofResource(module, info)];
            Marshal.Copy(pointer, bytes, 0, bytes.Length);
            return bytes;
        } finally { FreeLibrary(module); }
    }
}
'@
foreach ($resource in @(@{Id = 10; File = 'Unfurl.cer' }, @{Id = 12; File = 'Unfurl.appinstaller' })) {
    $bytes = [UnfurlSetupResources]::Read($setup, $resource.Id)
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
    if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $root $resource.File) -Algorithm SHA256).Hash) {
        throw "Setup has a different embedded $($resource.File)."
    }
}
foreach ($asset in $release.Assets | Where-Object { $_ -match '^Microsoft.*\.(msix|appx)$' }) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $root $asset)
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Setup dependency has an invalid Microsoft signature: $asset"
    }
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
Write-Information -InformationAction Continue "Verified $($release.Tag): setup resources and size, trusted signatures, timestamps, identities, dependencies, update policy, URLs and SHA-256."
