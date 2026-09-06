param(
    [string]$Configuration = 'Release',
    [string]$Output = 'artifacts'
)

$ErrorActionPreference = 'Stop'

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $repo "build\windows-release"
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repo $Output))
$stage = Join-Path $artifactRoot 'Unfurl'

cmake --build $build --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "The $Configuration build failed with exit code $LASTEXITCODE."
}
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'Unfurl.exe') -Destination $stage
Get-ChildItem -LiteralPath $build -Filter '*.dll' -File | Copy-Item -Destination $stage
$bootstrap = Join-Path $repo '.cache\windowsappsdk\2.4.0\runtimes\win-x64\native\Microsoft.WindowsAppRuntime.Bootstrap.dll'
if (Test-Path -LiteralPath $bootstrap) {
    Copy-Item -LiteralPath $bootstrap -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination $stage

$zip = Join-Path $artifactRoot "Unfurl-$Configuration.zip"
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Format-List
Write-Host "Created $zip"
