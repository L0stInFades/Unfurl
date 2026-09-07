param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows-release\Unfurl.exe'),
    [string]$Output = (Join-Path $PSScriptRoot '..\artifacts\ui-verification')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UnfurlUiInput {
    [DllImport("user32.dll")] private static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] private static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)]
    public static extern IntPtr SetText(IntPtr hwnd, uint message, IntPtr parameter, string text);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
    public static void Press(byte key) {
        keybd_event(key, 0, 0, UIntPtr.Zero);
        keybd_event(key, 0, 2, UIntPtr.Zero);
    }
    public static void Click(int x, int y) {
        var previousDpi = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try {
            SetCursorPos(x, y);
            mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
            mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
        } finally {
            SetThreadDpiAwarenessContext(previousDpi);
        }
    }
    public static void Drag(int x, int y, int dx, int dy) {
        var previousDpi = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try {
            SetCursorPos(x, y);
            mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
            System.Threading.Thread.Sleep(100);
            SetCursorPos(x + dx, y + dy);
            System.Threading.Thread.Sleep(150);
            mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
        } finally {
            SetThreadDpiAwarenessContext(previousDpi);
        }
    }
}
'@

$run = Join-Path ([System.IO.Path]::GetFullPath($Output)) (Get-Date -Format 'yyyyMMdd-HHmmss')
$inputs = Join-Path $run 'Project files'
[System.IO.Directory]::CreateDirectory((Join-Path $inputs 'Assets')) | Out-Null
$unicodeName = -join ([char[]]@(0x9879, 0x76ee, 0x8bf4, 0x660e))
$fixtures = @{
    'Design review.txt' = "Design review`nApproved for release.`n"
    'Budget.csv' = "Item,Amount`nDevelopment,4200`nTesting,800`n"
    "$unicodeName.txt" = "Unicode names survive compression and extraction.`n"
    'Assets/Readme.md' = "# Assets`nRelease package contents.`n"
}
foreach ($entry in $fixtures.GetEnumerator()) {
    [System.IO.File]::WriteAllText((Join-Path $inputs $entry.Key), $entry.Value)
}

function Find-Control([string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    $element = $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if (-not $element) { throw "The UI is missing $Id." }
    return $element
}

function Invoke-Control([string]$Id) {
    $element = Find-Control $Id
    if (-not $element.Current.IsEnabled) { throw "$Id is disabled." }
    ([System.Windows.Automation.InvokePattern]$element.GetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern)).Invoke()
}

function Set-Text([string]$Id, [string]$Value) {
    $element = Find-Control $Id
    ([System.Windows.Automation.ValuePattern]$element.GetCurrentPattern(
        [System.Windows.Automation.ValuePattern]::Pattern)).SetValue($Value)
}

function Select-Choice([string]$Id, [int]$Index) {
    if ($Id -eq 'ThemeChoice') {
        $returnNav = if ((Find-Control 'WorkspaceHeading').Current.Name -eq '解压压缩包') { 'ExtractNavigation' } else { 'CompressNavigation' }
        $selectionBefore = (Find-Control 'SelectionTitle').Current.Name
        Select-Navigation 'SettingsNavigation'
        Start-Sleep -Milliseconds 250
    }
    (Find-Control $Id).SetFocus()
    [System.Windows.Forms.SendKeys]::SendWait('%{DOWN}')
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.SendKeys]::SendWait('{HOME}' + ('{DOWN}' * $Index) + '{ENTER}')
    Start-Sleep -Milliseconds 250
    [UnfurlUiInput]::Press(0x1B)
    if ($Id -eq 'ThemeChoice') {
        Select-Navigation $returnNav
        Start-Sleep -Milliseconds 2500
        if ((Find-Control 'SelectionTitle').Current.Name -ne $selectionBefore) { throw 'Visiting settings lost the current selection.' }
    }
}

function Toggle-AllItems {
    ([System.Windows.Automation.TogglePattern](Find-Control 'SelectAllItems').GetCurrentPattern(
        [System.Windows.Automation.TogglePattern]::Pattern)).Toggle()
    Start-Sleep -Milliseconds 200
}

function Select-ArchiveItem([string]$Name, [bool]$Selected = $true) {
    $list = Find-Control 'FileList'
    $container = [System.Windows.Automation.ItemContainerPattern]$list.GetCurrentPattern(
        [System.Windows.Automation.ItemContainerPattern]::Pattern)
    $item = $container.FindItemByProperty($null, [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    if (-not $item) { throw "The archive list is missing $Name." }
    $virtualized = $null
    if ($item.TryGetCurrentPattern([System.Windows.Automation.VirtualizedItemPattern]::Pattern, [ref]$virtualized)) {
        ([System.Windows.Automation.VirtualizedItemPattern]$virtualized).Realize()
    }
    ([System.Windows.Automation.ScrollItemPattern]$item.GetCurrentPattern(
        [System.Windows.Automation.ScrollItemPattern]::Pattern)).ScrollIntoView()
    $pattern = [System.Windows.Automation.SelectionItemPattern]$item.GetCurrentPattern(
        [System.Windows.Automation.SelectionItemPattern]::Pattern)
    if ($Selected) { $pattern.AddToSelection() } else { $pattern.RemoveFromSelection() }
    Start-Sleep -Milliseconds 200
}

function Set-Options([bool]$Expanded) {
    $pattern = [System.Windows.Automation.ExpandCollapsePattern](Find-Control 'ArchiveOptions').GetCurrentPattern(
        [System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    if ($Expanded) { $pattern.Expand() } else { $pattern.Collapse() }
    Start-Sleep -Milliseconds 650
}

function Verify-OptionsMotion {
    $pattern = [System.Windows.Automation.ExpandCollapsePattern](Find-Control 'ArchiveOptions').GetCurrentPattern(
        [System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $scroll = [System.Windows.Automation.ScrollPattern](Find-Control 'WorkspaceScroll').GetCurrentPattern(
        [System.Windows.Automation.ScrollPattern]::Pattern)
    foreach ($expanded in @($true, $false)) {
        $samples = [System.Collections.Generic.List[object]]::new()
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        if ($expanded) { $pattern.Expand() } else { $pattern.Collapse() }
        do {
            $state = $scroll.Current
            $viewportHeight = (Find-Control 'WorkspaceScroll').Current.BoundingRectangle.Height
            $offset = if ($state.VerticallyScrollable) {
                ($viewportHeight * 100 / $state.VerticalViewSize - $viewportHeight) * $state.VerticalScrollPercent / 100
            } else { 0 }
            $samples.Add([pscustomobject]@{ Milliseconds = $watch.ElapsedMilliseconds; Offset = $offset })
            Start-Sleep -Milliseconds 20
        } while ($watch.ElapsedMilliseconds -lt 900)
        $stateName = if ($expanded) { 'expanding' } else { 'collapsing' }
        $samples | Export-Csv -LiteralPath (Join-Path $run "options-$stateName.csv") -NoTypeInformation
        $distinctOffsets = @($samples | ForEach-Object { [Math]::Round($_.Offset, 1) } | Select-Object -Unique)
        if ($distinctOffsets.Count -lt 3) { throw "The options $stateName scroll did not animate through intermediate positions." }
        if ($expanded) {
            $viewport = (Find-Control 'WorkspaceScroll').Current.BoundingRectangle
            foreach ($controlId in @('ArchivePasswordBox', 'SplitSizeBox')) {
                $bounds = (Find-Control $controlId).Current.BoundingRectangle
                if ($bounds.Top -lt $viewport.Top -or $bounds.Bottom -gt $viewport.Bottom) {
                    throw "Expanding options did not bring $controlId fully into view."
                }
            }
            Snapshot 'options-visible-dark.png'
        } elseif ($scroll.Current.VerticallyScrollable) {
            throw 'Collapsing options left unused scroll space in the default window.'
        }
    }
}

function Select-Navigation([string]$Id) {
    ([System.Windows.Automation.SelectionItemPattern](Find-Control $Id).GetCurrentPattern(
        [System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
    Start-Sleep -Milliseconds 250
}

function Wait-Status([string]$Pattern) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $status = (Find-Control 'StatusText').Current.Name
        if ($status -like $Pattern) { return $status }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Expected status '$Pattern'; received '$status'."
}

function Snapshot([string]$Name, [int]$Width = 0, [int]$Height = 0) {
    $heading = (Find-Control 'WorkspaceHeading').Current.BoundingRectangle
    [UnfurlUiInput]::Click([int]($heading.Left + $heading.Width / 2), [int]($heading.Top + $heading.Height / 2))
    & (Join-Path $PSScriptRoot 'capture-window.ps1') -ExistingProcessId $script:process.Id -Output (Join-Path $run $Name) -Width $Width -Height $Height | Out-Null
}

function Verify-WindowChrome {
    $pattern = [System.Windows.Automation.WindowPattern]$script:window.GetCurrentPattern(
        [System.Windows.Automation.WindowPattern]::Pattern)
    Invoke-Control 'Maximize'
    Start-Sleep -Milliseconds 400
    if ($pattern.Current.WindowVisualState -ne [System.Windows.Automation.WindowVisualState]::Maximized) {
        throw 'The native maximize button did not maximize the window.'
    }
    Snapshot 'maximized-light.png'
    $title = (Find-Control 'WindowTitleBar').Current.BoundingRectangle
    $titleX = [int]($title.Left + $title.Width / 2)
    $titleY = [int]($title.Top + $title.Height / 2)
    [UnfurlUiInput]::Click($titleX, $titleY)
    [UnfurlUiInput]::Click($titleX, $titleY)
    Start-Sleep -Milliseconds 400
    if ($pattern.Current.WindowVisualState -ne [System.Windows.Automation.WindowVisualState]::Normal) {
        throw 'Double-clicking the title bar did not restore the window.'
    }
    $before = $script:window.Current.BoundingRectangle
    $title = (Find-Control 'WindowTitleBar').Current.BoundingRectangle
    [UnfurlUiInput]::Drag([int]($title.Left + $title.Width / 2), [int]($title.Top + $title.Height / 2), 32, 24)
    Start-Sleep -Milliseconds 300
    $after = $script:window.Current.BoundingRectangle
    if ([Math]::Abs($after.Left - $before.Left) -lt 16 -or [Math]::Abs($after.Top - $before.Top) -lt 12) {
        throw 'Dragging the title bar did not move the window.'
    }
    Invoke-Control 'Minimize'
    Start-Sleep -Milliseconds 300
    if ($pattern.Current.WindowVisualState -ne [System.Windows.Automation.WindowVisualState]::Minimized) {
        throw 'The native minimize button did not minimize the window.'
    }
    $pattern.SetWindowVisualState([System.Windows.Automation.WindowVisualState]::Normal)
    & (Join-Path $PSScriptRoot 'capture-window.ps1') -ExistingProcessId $script:process.Id -Output (Join-Path $run 'restored-light.png') | Out-Null
    $script:process.Refresh()
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:process.MainWindowHandle)
    $expandedWidth = (Find-Control 'CompressNavigation').Current.BoundingRectangle.Width
    Invoke-Control 'PART_PaneToggleButton'
    Start-Sleep -Milliseconds 400
    if ((Find-Control 'CompressNavigation').Current.BoundingRectangle.Width -ge $expandedWidth) {
        throw 'The title bar navigation button did not collapse the pane.'
    }
    Invoke-Control 'PART_PaneToggleButton'
    Start-Sleep -Milliseconds 400
}

function Stop-App {
    if ($script:process -and -not $script:process.HasExited) {
        $script:process.CloseMainWindow() | Out-Null
        if (-not $script:process.WaitForExit(5000)) { $script:process.Kill(); $script:process.WaitForExit() }
    }
}

function Choose-Path([string]$Command, [string]$Title, [string]$Path) {
    (Find-Control $Command).SetFocus()
    [UnfurlUiInput]::Press(0x20)
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $script:process.Id),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Title),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window))
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $dialog = $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
        if ($dialog) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $dialog) { throw "The native '$Title' dialog did not open." }
    do {
        $filename = $dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.AndCondition]::new(
                [System.Windows.Automation.OrCondition]::new(
                    [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, '1148'),
                    [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, '1152')),
                [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Edit')))
        if ($filename) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $filename) { throw "The native '$Title' picker has no path field." }
    # Common Item Dialog controls do not expose ValuePattern on every Windows build.
    [UnfurlUiInput]::SetText([IntPtr]$filename.Current.NativeWindowHandle, 0x000C, [IntPtr]::Zero, $Path) | Out-Null
    $accept = $dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.AndCondition]::new(
            [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, '1'),
            [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Button')))
    if (-not $accept) { throw 'The native file picker has no accept button.' }
    [UnfurlUiInput]::PostMessage([IntPtr]$accept.Current.NativeWindowHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 600
}

try {
    $sources = @('--compress') + @(Get-ChildItem -LiteralPath $inputs | ForEach-Object FullName)
    $capture = & (Join-Path $PSScriptRoot 'capture-window.ps1') -Executable $Executable -LaunchArguments $sources -Output (Join-Path $run 'compress-ready.png') -LeaveOpen
    if (-not $capture.PerMonitorV2) { throw 'The application is not PerMonitorV2 DPI aware.' }
    $script:process = Get-Process -Id $capture.ProcessId
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:process.MainWindowHandle)
    foreach ($translation in @{ CompressNavigation = '压缩'; ExtractNavigation = '解压'; WorkspaceHeading = '创建压缩包'; DestinationCommand = '选择保存位置' }.GetEnumerator()) {
        if ((Find-Control $translation.Key).Current.Name -ne $translation.Value) { throw "Unexpected Chinese label for $($translation.Key)." }
    }
    Verify-OptionsMotion
    Set-Text 'ArchiveNameBox' '../invalid'
    Invoke-Control 'CompressCommand'
    Wait-Status '压缩包名称必须*不能包含路径*' | Write-Host
    Set-Text 'ArchiveNameBox' 'Project delivery'
    Set-Options $true
    Set-Text 'SplitSizeBox' '-1'
    Invoke-Control 'CompressCommand'
    Wait-Status '分卷大小必须*' | Write-Host
    Set-Text 'SplitSizeBox' ''
    Select-Choice 'FormatComboBox' 1
    if ((Find-Control 'ArchivePasswordBox').Current.IsEnabled) { throw 'Non-ZIP password control must be disabled.' }
    Select-Choice 'FormatComboBox' 0
    Invoke-Control 'CompressCommand'
    Wait-Status '已创建：*' | Write-Host
    $archive = Join-Path $inputs 'Project delivery.zip'
    if (-not (Test-Path -LiteralPath $archive)) { throw 'Compress did not create its output.' }
    $zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($fixture in $fixtures.GetEnumerator()) {
            $entry = $zip.GetEntry($fixture.Key)
            if (-not $entry) { throw "The archive is missing $($fixture.Key)." }
            $difference = $entry.LastWriteTime.UtcDateTime - [System.IO.File]::GetLastWriteTimeUtc((Join-Path $inputs $fixture.Key))
            if ([Math]::Abs($difference.TotalSeconds) -gt 2) { throw "Modification time differs for $($fixture.Key)." }
            $reader = [System.IO.StreamReader]::new($entry.Open())
            try {
                if ($reader.ReadToEnd() -cne $fixture.Value) { throw "Archive content differs for $($fixture.Key)." }
            } finally { $reader.Dispose() }
        }
    } finally { $zip.Dispose() }
    Snapshot 'options-expanded.png'
    Set-Options $false
    $viewport = (Find-Control 'WorkspaceScroll').Current.BoundingRectangle
    foreach ($controlId in @('AddFilesCommand', 'ArchiveNameBox', 'FormatComboBox', 'DestinationCommand', 'ArchiveOptions')) {
        $bounds = (Find-Control $controlId).Current.BoundingRectangle
        if ($bounds.Top -lt $viewport.Top -or $bounds.Bottom -gt $viewport.Bottom) {
            throw "$controlId is clipped in the default window."
        }
    }
    Select-Choice 'ThemeChoice' 2
    Snapshot 'compress-complete-dark.png'
    Select-Choice 'ThemeChoice' 1
    Snapshot 'compress-complete-light.png'
    Select-Navigation 'SettingsNavigation'
    & (Join-Path $PSScriptRoot 'capture-window.ps1') -ExistingProcessId $script:process.Id -Output (Join-Path $run 'settings-light.png') | Out-Null
    & (Join-Path $PSScriptRoot 'capture-window.ps1') -ExistingProcessId $script:process.Id -Output (Join-Path $run 'settings-minimum-light.png') -Width 560 -Height 480 | Out-Null
    $themeBounds = (Find-Control 'ThemeChoice').Current.BoundingRectangle
    $settingsBounds = $script:window.Current.BoundingRectangle
    if ($themeBounds.Right -gt $settingsBounds.Right) { throw 'The theme selector is clipped in the minimum window.' }
    Select-Navigation 'CompressNavigation'
    Snapshot 'settings-return-light.png' 960 640
    Verify-WindowChrome
    $activationWindow = [System.Windows.Forms.Form]::new()
    try {
        $activationWindow.Text = 'Material verification'
        $activationWindow.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
        $activationWindow.Location = [System.Drawing.Point]::new(0, 0)
        $activationWindow.Size = [System.Drawing.Size]::new(180, 80)
        $activationWindow.ShowInTaskbar = $false
        $activationWindow.Show()
        $activationWindow.Activate()
        [System.Windows.Forms.Application]::DoEvents()
        & (Join-Path $PSScriptRoot 'capture-window.ps1') -ExistingProcessId $script:process.Id -Output (Join-Path $run 'inactive-light.png') -PreserveActivation | Out-Null
    } finally {
        $activationWindow.Dispose()
    }
    Snapshot 'wide-light.png' 1400 800
    if ((Find-Control 'WorkspaceScroll').Current.BoundingRectangle.Width -gt (797 * $capture.Dpi / 96)) {
        throw 'The content stretches past its maximum reading width.'
    }
    Snapshot 'compact-light.png' 640 640
    Snapshot 'minimum-light.png' 560 480
    $scroll = [System.Windows.Automation.ScrollPattern](Find-Control 'WorkspaceScroll').GetCurrentPattern(
        [System.Windows.Automation.ScrollPattern]::Pattern)
    if ($scroll.Current.VerticallyScrollable) {
        $scroll.SetScrollPercent([System.Windows.Automation.ScrollPattern]::NoScroll, 100)
    }
    Snapshot 'minimum-settings-light.png'
    $viewport = (Find-Control 'WorkspaceScroll').Current.BoundingRectangle
    $destinationBounds = (Find-Control 'DestinationCommand').Current.BoundingRectangle
    if ($destinationBounds.Bottom -gt $viewport.Bottom -or $destinationBounds.Top -lt $viewport.Top) {
        throw 'The destination control is outside the scrolled minimum viewport.'
    }
    Stop-App

    $capture = & (Join-Path $PSScriptRoot 'capture-window.ps1') -Executable $Executable -LaunchArguments @($archive) -Output (Join-Path $run 'archive-preview.png') -LeaveOpen
    $script:process = Get-Process -Id $capture.ProcessId
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:process.MainWindowHandle)
    Wait-Status '*可以开始解压' | Write-Host
    Toggle-AllItems
    if ((Find-Control 'ExtractCommand').Current.IsEnabled) { throw 'An empty entry selection left Extract enabled.' }
    Select-ArchiveItem 'Assets'
    Select-ArchiveItem 'Assets/Readme.md' $false
    if ((Find-Control 'ExtractCommand').Current.IsEnabled) { throw 'Deselecting the last file left its parent selected.' }
    Select-ArchiveItem "$unicodeName.txt"
    $toggle = [System.Windows.Automation.TogglePattern](Find-Control 'SelectAllItems').GetCurrentPattern(
        [System.Windows.Automation.TogglePattern]::Pattern)
    if ($toggle.Current.ToggleState -ne [System.Windows.Automation.ToggleState]::Indeterminate) {
        throw 'Partial selection is not reflected in the select-all checkbox.'
    }
    Select-Choice 'ThemeChoice' 1
    $partialOutput = Join-Path $run 'Partial output'
    [System.IO.Directory]::CreateDirectory($partialOutput) | Out-Null
    Choose-Path 'DestinationCommand' '选择保存位置' $partialOutput
    Snapshot 'partial-selection-light.png'
    Invoke-Control 'ExtractCommand'
    Wait-Status '已解压：*' | Write-Host
    $partialFiles = @(Get-ChildItem -LiteralPath $partialOutput -Recurse -File)
    if ($partialFiles.Count -ne 1 -or $partialFiles[0].Name -cne "$unicodeName.txt" -or
        [System.IO.File]::ReadAllText($partialFiles[0].FullName) -cne $fixtures["$unicodeName.txt"]) {
        throw 'Partial extraction wrote omitted files or changed the selected Unicode file.'
    }
    Toggle-AllItems
    if ($toggle.Current.ToggleState -ne [System.Windows.Automation.ToggleState]::On) { throw 'Select all did not select the complete archive.' }
    Choose-Path 'DestinationCommand' '选择保存位置' $inputs
    Invoke-Control 'ExtractCommand'
    Wait-Status '已解压：*' | Write-Host
    $extracted = Join-Path $inputs 'Project delivery'
    foreach ($fixture in $fixtures.GetEnumerator()) {
        $path = Join-Path $extracted $fixture.Key
        if (-not (Test-Path -LiteralPath $path) -or [System.IO.File]::ReadAllText($path) -cne $fixture.Value) {
            throw "Extracted content differs for $($fixture.Key)."
        }
    }
    Snapshot 'extract-complete-dark.png'
    Invoke-Control 'ClearCommand'
    if ((Find-Control 'ExtractCommand').Current.IsEnabled) { throw 'Clearing the selection left Extract enabled.' }
    Select-Choice 'ThemeChoice' 1
    Snapshot 'empty-extract-light.png'
    Select-Navigation 'CompressNavigation'
    if ((Find-Control 'CompressCommand').Current.IsEnabled) { throw 'An empty compression selection left Compress enabled.' }
    Snapshot 'empty-light.png'

    Choose-Path 'AddFilesCommand' '添加文件' (Join-Path $inputs 'Design review.txt')
    Wait-Status '*可以开始压缩' | Write-Host
    Set-Text 'ArchiveNameBox' 'From picker'
    Choose-Path 'AddFolderCommand' '添加文件夹' (Join-Path $inputs 'Assets')
    $chosenOutput = Join-Path $run 'Chosen output'
    [System.IO.Directory]::CreateDirectory($chosenOutput) | Out-Null
    Choose-Path 'DestinationCommand' '选择保存位置' $chosenOutput
    Invoke-Control 'CompressCommand'
    Wait-Status '已创建：From picker.zip*' | Write-Host
    if (-not (Test-Path -LiteralPath (Join-Path $chosenOutput 'From picker.zip'))) { throw 'The chosen destination was ignored.' }
    Snapshot 'picker-compression-light.png'
    Stop-App

    $manyArchive = Join-Path $run 'Many entries.zip'
    $otherArchive = Join-Path $run 'Other entries.zip'
    foreach ($spec in @(@{ Path = $manyArchive; Count = 260; Prefix = 'Many' }, @{ Path = $otherArchive; Count = 2; Prefix = 'Other' })) {
        $zip = [System.IO.Compression.ZipFile]::Open($spec.Path, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            for ($i = 0; $i -lt $spec.Count; $i++) {
                $writer = [System.IO.StreamWriter]::new($zip.CreateEntry(('item-{0:D3}.txt' -f $i)).Open())
                try { $writer.Write("$($spec.Prefix) entry $i") } finally { $writer.Dispose() }
            }
        } finally { $zip.Dispose() }
    }
    $capture = & (Join-Path $PSScriptRoot 'capture-window.ps1') -Executable $Executable -LaunchArguments @($manyArchive, $otherArchive) -Output (Join-Path $run 'multiple-archives.png') -LeaveOpen
    $script:process = Get-Process -Id $capture.ProcessId
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:process.MainWindowHandle)
    Wait-Status '*可以开始解压' | Write-Host
    if ((Find-Control 'SelectionTitle').Current.Name -ne '已选 262 / 262 项') { throw 'The archive list still truncates entries or ignores additional archives.' }
    Toggle-AllItems
    Select-ArchiveItem 'Many entries.zip / item-259.txt'
    Select-ArchiveItem 'Other entries.zip / item-000.txt'
    $multiOutput = Join-Path $run 'Multiple partial output'
    [System.IO.Directory]::CreateDirectory($multiOutput) | Out-Null
    Choose-Path 'DestinationCommand' '选择保存位置' $multiOutput
    Snapshot 'multiple-partial-selection.png'
    Invoke-Control 'ExtractCommand'
    Wait-Status '已解压：2 个压缩包*' | Write-Host
    if (@(Get-ChildItem -LiteralPath $multiOutput -File -Recurse).Count -ne 2 -or
        [System.IO.File]::ReadAllText((Join-Path $multiOutput 'Many entries/item-259.txt')) -cne 'Many entry 259' -or
        [System.IO.File]::ReadAllText((Join-Path $multiOutput 'Other entries/item-000.txt')) -cne 'Other entry 0') {
        throw 'Partial extraction mixed archive selections or omitted an entry after the former preview limit.'
    }
    $allOutput = Join-Path $run 'All entries output'
    [System.IO.Directory]::CreateDirectory($allOutput) | Out-Null
    Choose-Path 'DestinationCommand' '选择保存位置' $allOutput
    Toggle-AllItems
    Invoke-Control 'ExtractCommand'
    Wait-Status '已解压：2 个压缩包*' | Write-Host
    if (@(Get-ChildItem -LiteralPath $allOutput -File -Recurse).Count -ne 262) { throw 'Select all omitted archive entries.' }
    Stop-App

    $cancelSource = Join-Path $run 'Cancellation source.bin'
    $stream = [System.IO.File]::Create($cancelSource)
    try {
        $block = [byte[]]::new(4MB)
        [System.Random]::new(42).NextBytes($block)
        for ($i = 0; $i -lt 32; $i++) { $stream.Write($block, 0, $block.Length) }
    } finally { $stream.Dispose() }
    $capture = & (Join-Path $PSScriptRoot 'capture-window.ps1') -Executable $Executable -LaunchArguments @('--compress', $cancelSource) -Output (Join-Path $run 'cancellation-ready.png') -LeaveOpen
    $script:process = Get-Process -Id $capture.ProcessId
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:process.MainWindowHandle)
    Set-Text 'ArchiveNameBox' 'Cancelled'
    Select-Choice 'FormatComboBox' 1
    Invoke-Control 'CompressCommand'
    Wait-Status '正在压缩：*' | Write-Host
    Invoke-Control 'CancelCommand'
    Wait-Status '已取消' | Write-Host
    if (Test-Path -LiteralPath (Join-Path $run 'Cancelled.7z')) { throw 'Cancellation published an archive.' }
    if (Get-ChildItem -LiteralPath $run -Directory -Filter '.unfurl-stage-*') { throw 'Cancellation left a staging directory.' }
    Snapshot 'cancelled-dark.png'
    Set-Text 'ArchiveNameBox' 'Close cancellation'
    Invoke-Control 'CompressCommand'
    Wait-Status '正在压缩：*' | Write-Host
    $script:process.CloseMainWindow() | Out-Null
    if (-not $script:process.WaitForExit(20000)) { throw 'Closing the app did not finish cancellation.' }
    if (Test-Path -LiteralPath (Join-Path $run 'Close cancellation.7z')) { throw 'Closing published a cancelled archive.' }
    if (Get-ChildItem -LiteralPath $run -Directory -Filter '.unfurl-stage-*') { throw 'Closing left a staging directory.' }
    Write-Host "UI verification passed at $($capture.Dpi) DPI. Screenshots: $run"
} finally {
    Stop-App
    if ($cancelSource -and (Test-Path -LiteralPath $cancelSource)) { [System.IO.File]::Delete($cancelSource) }
}
