#Requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$executablePath = [System.IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Unfurl.exe was not found at $executablePath."
}

$classes = 'Registry::HKEY_CURRENT_USER\Software\Classes'
$icon = "$executablePath,0"
$command = '"' + $executablePath + '" "%1"'
$compressCommand = '"' + $executablePath + '" --compress "%1"'
$extensions = @('.zip', '.7z', '.rar', '.tar', '.gz', '.bz2', '.xz', '.zst', '.tgz', '.tbz2', '.txz', '.001')

function Set-ShellCommand {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string]$Icon
    )

    if (-not $PSCmdlet.ShouldProcess($Key, 'Register Unfurl Explorer command')) { return }
    New-Item -Path $Key -Force | Out-Null
    Set-ItemProperty -LiteralPath $Key -Name '(default)' -Value $Label
    New-ItemProperty -LiteralPath $Key -Name 'Icon' -PropertyType String -Value $Icon -Force | Out-Null
    $commandKey = Join-Path $Key 'command'
    New-Item -Path $commandKey -Force | Out-Null
    Set-ItemProperty -LiteralPath $commandKey -Name '(default)' -Value $Command
}

foreach ($extension in $extensions) {
    $key = Join-Path $classes "SystemFileAssociations\$extension\shell\Unfurl.Extract"
    Set-ShellCommand -Key $key -Label '使用 Unfurl 解压' -Command $command -Icon $icon
}

$directoryKey = Join-Path $classes 'Directory\shell\Unfurl.Compress'
Set-ShellCommand -Key $directoryKey -Label '使用 Unfurl 压缩' -Command $compressCommand -Icon $icon
$fileKey = Join-Path $classes '*\shell\Unfurl.Compress'
Set-ShellCommand -Key $fileKey -Label '使用 Unfurl 压缩' -Command $compressCommand -Icon $icon
if (-not $WhatIfPreference) { Write-Information -InformationAction Continue 'Registered Unfurl Explorer commands for the current user.' }
