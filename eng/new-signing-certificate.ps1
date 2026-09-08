#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Output = (Join-Path $PSScriptRoot '..\artifacts\certificates\Unfurl.cer')
)

$ErrorActionPreference = 'Stop'
[xml]$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\installer\AppxManifest.xml') -Raw
$subject = $manifest.Package.Identity.Publisher
$certificates = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object {
        $_.Subject -ceq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date)
    })
if ($certificates.Count -gt 1) {
    throw "More than one signing certificate exists for $subject. Select one explicitly when packaging."
}
if ($certificates.Count -eq 1) {
    $certificate = $certificates[0]
} else {
    $certificate = New-SelfSignedCertificate -Type Custom -Subject $subject -FriendlyName 'Unfurl MSIX signing' `
        -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -KeyUsage DigitalSignature -KeyExportPolicy NonExportable -NotBefore (Get-Date).AddDays(-1) `
        -NotAfter (Get-Date).AddYears(5) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
$outputPath = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
Export-Certificate -Cert $certificate -FilePath $outputPath -Type CERT -Force | Out-Null
$certificate | Select-Object Subject, Thumbprint, NotAfter
Write-Information -InformationAction Continue "Public certificate: $outputPath"
Write-Information -InformationAction Continue 'The non-exportable private key remains in Cert:\CurrentUser\My on this machine.'
