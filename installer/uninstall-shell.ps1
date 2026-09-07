$ErrorActionPreference = 'Stop'
$classes = 'Registry::HKEY_CURRENT_USER\Software\Classes'
$extensions = @('.zip', '.7z', '.rar', '.tar', '.gz', '.bz2', '.xz', '.zst', '.tgz', '.tbz2', '.txz', '.001')

foreach ($extension in $extensions) {
    $key = Join-Path $classes "SystemFileAssociations\$extension\shell\Unfurl.Extract"
    Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue
}

$directoryKey = Join-Path $classes 'Directory\shell\Unfurl.Compress'
Remove-Item -LiteralPath $directoryKey -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $classes '*\shell\Unfurl.Compress') -Recurse -Force -ErrorAction SilentlyContinue

# Remove the entry created by the older wildcard .reg template as well. The asterisk is a literal key name here.
Remove-Item -LiteralPath (Join-Path $classes '*\shell\Unfurl.Extract') -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Removed Unfurl Explorer commands for the current user."
