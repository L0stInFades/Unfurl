#Requires -Version 7.0
<#
.SYNOPSIS
Checks repository scripts with PSScriptAnalyzer and the PowerShell 7 parser.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
if (-not (Get-Module -Name PSScriptAnalyzer | Where-Object Version -GE ([version]'1.24.0'))) {
    Import-Module PSScriptAnalyzer -MinimumVersion 1.24.0 -ErrorAction Stop
}
$repo = Split-Path -Parent $PSScriptRoot
$files = Get-ChildItem -LiteralPath (Join-Path $repo 'eng'), (Join-Path $repo 'installer') -Filter '*.ps1' -File
$issues = foreach ($file in $files) {
    Invoke-ScriptAnalyzer -Path $file.FullName -Settings (Join-Path $PSScriptRoot 'PSScriptAnalyzerSettings.psd1')
}
if ($issues) {
    $issues | Format-Table ScriptName, Line, RuleName, Message -Wrap | Out-Host
    throw "PowerShell analysis reported $(@($issues).Count) issue(s)."
}
Write-Information "Verified $($files.Count) PowerShell 7 scripts." -InformationAction Continue
