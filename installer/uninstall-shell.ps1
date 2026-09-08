#Requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = 'Stop'
$classes = 'Registry::HKEY_CURRENT_USER\Software\Classes'
$extensions = @('.zip', '.7z', '.rar', '.tar', '.gz', '.bz2', '.xz', '.zst', '.tgz', '.tbz2', '.txz', '.001')

foreach ($extension in $extensions) {
    $key = Join-Path $classes "SystemFileAssociations\$extension\shell\Unfurl.Extract"
    if ((Test-Path -LiteralPath $key) -and $PSCmdlet.ShouldProcess($key, 'Remove Unfurl Explorer command')) {
        Remove-Item -LiteralPath $key -Recurse -Force
    }
}

# Remove the entry created by the older wildcard .reg template as well. The asterisk is a literal key name here.
foreach ($relative in 'Directory\shell\Unfurl.Compress', '*\shell\Unfurl.Compress', '*\shell\Unfurl.Extract') {
    $key = Join-Path $classes $relative
    if ((Test-Path -LiteralPath $key) -and $PSCmdlet.ShouldProcess($key, 'Remove Unfurl Explorer command')) {
        Remove-Item -LiteralPath $key -Recurse -Force
    }
}
if (-not $WhatIfPreference) { Write-Information -InformationAction Continue 'Removed Unfurl Explorer commands for the current user.' }
