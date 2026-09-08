#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$ReleaseRoot,
    [Parameter(Mandatory)][uri]$PackageBaseUri,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{40}$')][string]$CertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Setup packaging requires Windows and MSVC.' }
if (-not $PackageBaseUri.IsAbsoluteUri -or $PackageBaseUri.Scheme -ne 'https' -or -not $PackageBaseUri.AbsoluteUri.EndsWith('/')) {
    throw 'PackageBaseUri must be an absolute HTTPS directory URL ending in a slash.'
}
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$root = [IO.Path]::GetFullPath($ReleaseRoot)
$build = Join-Path $repo 'build/setup-release'
$payload = Join-Path $build 'payload'
New-Item -ItemType Directory -Force -Path $payload | Out-Null
$sdkRoot = Join-Path $repo '.cache/windowsappsdk/2.4.0'
[xml]$feed = Get-Content -LiteralPath (Join-Path $root 'Unfurl.appinstaller') -Raw
$bootstrap = Join-Path $sdkRoot 'runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $bootstrap
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw 'The embedded Windows App SDK bootstrap DLL must have a valid Microsoft signature.'
}
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $root 'Unfurl.cer'))
if ($certificate.HasPrivateKey -or $certificate.Thumbprint -ne $CertificateThumbprint) {
    throw 'Setup must embed only the public release certificate.'
}
function Read-Identity([string]$Path) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $reader = [IO.StreamReader]::new($zip.GetEntry('AppxManifest.xml').Open())
        try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        return $manifest.Package.Identity
    } finally { $zip.Dispose() }
}
function ConvertTo-CppString([string]$Value) {
    if ($Value -match '[\r\n\x00]') { throw 'Unexpected control character in setup metadata.' }
    return 'L"' + $Value.Replace('\', '\\').Replace('"', '\"') + '"'
}
$files = @($feed.AppInstaller.Dependencies.Package | ForEach-Object { Join-Path $root ([IO.Path]::GetFileName(([uri]$_.Uri).AbsolutePath)) })
foreach ($name in @('Main', 'Singleton', 'DDLM')) {
    $source = Join-Path $sdkRoot "tools/MSIX/win10-x64/Microsoft.WindowsAppRuntime.$name.2.msix"
    $identity = Read-Identity $source
    $destination = Join-Path $root "$($identity.Name)_$($identity.Version)_x64.msix"
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $files += $destination
}
$rows = foreach ($file in $files) {
    $identity = Read-Identity $file
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -cne $identity.Publisher -or
        $identity.Publisher -notmatch 'O=Microsoft Corporation' -or $identity.ProcessorArchitecture -ne 'x64') {
        throw "Setup dependency is not a valid Microsoft x64 package: $file"
    }
    $version = [version]$identity.Version
    $encodedVersion = ([uint64]$version.Major -shl 48) -bor ([uint64]$version.Minor -shl 32) -bor
    ([uint64]$version.Build -shl 16) -bor [uint64]$version.Revision
    $name = [IO.Path]::GetFileName($file)
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $label = if ($identity.Name -like 'Microsoft.VCLibs*') { 'Microsoft Visual C++' } else { 'Windows App Runtime' }
    '    {' + ((@($identity.Name, $name, ([uri]::new($PackageBaseUri, $name)).AbsoluteUri, $hash, $label) | ForEach-Object { ConvertTo-CppString $_ }) -join ', ') + ", ${encodedVersion}ULL},"
}
$header = @'
#pragma once
#include <cstdint>
namespace unfurl::setup::payload {
struct Dependency {
    const wchar_t *name, *file, *url, *sha256, *label;
    std::uint64_t version;
};
inline constexpr wchar_t microsoft_publisher[] = L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";
inline constexpr Dependency dependencies[] = {
'@ + "`n" + ($rows -join "`n") + "`n};`n}`n"
[IO.File]::WriteAllText((Join-Path $payload 'SetupPayload.h'), $header, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $root 'Unfurl.cer'), (Join-Path $root 'Unfurl.appinstaller') -Destination $payload -Force
$resources = @(
    '#include <windows.h>',
    ('#include "' + (Join-Path $repo 'installer/UnfurlSetup.rc').Replace('\', '/') + '"'),
    ('1 ICON "' + (Join-Path $repo 'src/app/Unfurl.ico').Replace('\', '/') + '"'),
    '10 RCDATA "Unfurl.cer"',
    ('11 RCDATA "' + $bootstrap.Replace('\', '/') + '"'),
    '12 RCDATA "Unfurl.appinstaller"',
    ('13 RCDATA "' + (Join-Path $repo 'installer/SetupWindow.xaml').Replace('\', '/') + '"')
)
[IO.File]::WriteAllLines((Join-Path $payload 'SetupPayload.rc'), $resources, [Text.UTF8Encoding]::new($false))
cmake -S $repo -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNFURL_BUILD_APP=OFF -DUNFURL_BUILD_TESTS=OFF -DUNFURL_BUILD_INSTALLER=ON "-DUNFURL_SETUP_PAYLOAD_DIR=$payload"
if ($LASTEXITCODE -ne 0) { throw 'Setup configuration failed.' }
cmake --build $build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Setup build failed.' }
$exe = Join-Path $root 'UnfurlSetup.exe'
Copy-Item -LiteralPath (Join-Path $build 'UnfurlSetup.exe') -Destination $exe -Force
$signtool = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin') -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' -and (Test-Path (Join-Path $_.FullName 'x64/signtool.exe')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 |
    ForEach-Object { Join-Path $_.FullName 'x64/signtool.exe' }
& $signtool sign /fd SHA256 /sha1 $CertificateThumbprint /tr $TimestampUrl /td SHA256 $exe
if ($LASTEXITCODE -ne 0) { throw 'Setup signing failed.' }
@($files | ForEach-Object { [IO.Path]::GetFileName($_) }) | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $root 'setup-dependencies.json') -Encoding utf8NoBOM
Write-Information -InformationAction Continue "Created standalone WinUI 3 installer: $exe ($((Get-Item $exe).Length) bytes)"
