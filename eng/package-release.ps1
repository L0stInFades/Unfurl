param(
    [string]$Configuration = 'Release',
    [string]$Output = 'artifacts',
    [switch]$IncludeWindowsAppRuntime
)

$ErrorActionPreference = 'Stop'

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $repo "build\windows-release"
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repo $Output))
$stage = Join-Path $artifactRoot 'Unfurl'
if ($stage.Equals($repo, [System.StringComparison]::OrdinalIgnoreCase) -or
    $repo.StartsWith($stage + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The staging directory must not contain the repository.'
}

cmake --build $build --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "The $Configuration build failed with exit code $LASTEXITCODE."
}
$executable = Join-Path $build 'Unfurl.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "The build did not produce $executable."
}
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $executable -Destination $stage
$runtime_dlls = @(Get-ChildItem -LiteralPath $build -Filter '*.dll' -File)
if ($runtime_dlls.Count -eq 0 -or -not ($runtime_dlls.Name -contains 'archive.dll')) {
    throw "The build output is missing libarchive runtime dependencies."
}
$runtime_dlls | Copy-Item -Destination $stage
if (-not $env:VCToolsRedistDir) {
    throw 'Run packaging from a Visual Studio Developer PowerShell with VCToolsRedistDir set.'
}
$crt = Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $crt) {
    throw 'The Visual C++ x64 redistributable DLLs were not found.'
}
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' -File | Copy-Item -Destination $stage
$bootstrap = Join-Path $repo '.cache\windowsappsdk\2.4.0\runtimes\win-x64\native\Microsoft.WindowsAppRuntime.Bootstrap.dll'
if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
    throw "Windows App SDK Bootstrap DLL was not restored at $bootstrap."
}
Copy-Item -LiteralPath $bootstrap -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'README.zh-CN.md') -Destination $stage
$docsStage = Join-Path $stage 'docs'
New-Item -ItemType Directory -Force -Path $docsStage | Out-Null
foreach ($file in @('verification.md', 'updates.md')) {
    Copy-Item -LiteralPath (Join-Path $repo "docs\$file") -Destination $docsStage
}

$installerStage = Join-Path $stage 'installer'
New-Item -ItemType Directory -Force -Path $installerStage | Out-Null
foreach ($file in @('install-shell.ps1', 'uninstall-shell.ps1', 'install-runtime.ps1', 'UnfurlShell.reg', 'UninstallUnfurlShell.reg')) {
    Copy-Item -LiteralPath (Join-Path $repo "installer\$file") -Destination $installerStage
}

$licenseStage = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force -Path $licenseStage | Out-Null
if ($env:VSINSTALLDIR) {
    $redistList = Get-ChildItem -LiteralPath (Join-Path $env:VSINSTALLDIR 'Licenses') -Filter 'Redist.txt' -File -Recurse |
        Select-Object -First 1
    if ($redistList) {
        Copy-Item -LiteralPath $redistList.FullName -Destination (Join-Path $licenseStage 'VisualStudio-Redist.txt')
    }
}
$vcpkgShare = Join-Path $repo 'build\vcpkg_installed\windows-release\x64-windows\share'
if (Test-Path -LiteralPath $vcpkgShare -PathType Container) {
    foreach ($package in Get-ChildItem -LiteralPath $vcpkgShare -Directory | Sort-Object Name) {
        $copyright = Join-Path $package.FullName 'copyright'
        if (Test-Path -LiteralPath $copyright -PathType Leaf) {
            Copy-Item -LiteralPath $copyright -Destination (Join-Path $licenseStage "$($package.Name).copyright")
        }
    }
}

if ($IncludeWindowsAppRuntime) {
    $runtimeSource = Join-Path $repo '.cache\windowsappsdk\2.4.0\tools\MSIX\win10-x64'
    if (-not (Test-Path -LiteralPath $runtimeSource -PathType Container)) {
        throw "Windows App SDK x64 runtime packages were not restored at $runtimeSource."
    }
    $runtimeStage = Join-Path $stage 'windows-app-runtime'
    New-Item -ItemType Directory -Force -Path $runtimeStage | Out-Null
    Get-ChildItem -LiteralPath $runtimeSource -Filter '*.msix' -File | Sort-Object Name |
        Copy-Item -Destination $runtimeStage
}

$zip = Join-Path $artifactRoot "Unfurl-$Configuration.zip"
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipStream = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None)
$zipArchive = [System.IO.Compression.ZipArchive]::new(
    $zipStream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
$fixedTimestamp = [DateTimeOffset]::Parse('1980-01-01T00:00:00Z')
try {
    $files = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName
    foreach ($file in $files) {
        $relative = [System.IO.Path]::GetRelativePath($stage, $file.FullName).Replace('\', '/')
        $entry = $zipArchive.CreateEntry($relative, [System.IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $fixedTimestamp
        $input = [System.IO.File]::OpenRead($file.FullName)
        $outputStream = $entry.Open()
        try {
            $input.CopyTo($outputStream)
        } finally {
            $outputStream.Dispose()
            $input.Dispose()
        }
    }
} finally {
    $zipArchive.Dispose()
    $zipStream.Dispose()
}
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Format-List
Write-Host "Created $zip"
