# Verification

## Environment

The local verification environment is Windows 11 x64, a 2560 x 1600 display at 150% scaling (144 DPI), Windows App SDK 2.4.0, and PowerShell 7. The application reports PerMonitorV2 DPI awareness through `GetWindowDpiAwarenessContext`.

## Core checks

Both MSVC Release and MinGW GCC 16.2 Debug builds pass the archive core suite. Release assertions remain enabled in the test executable. The suite checks ZIP, 7Z, TAR.GZ, TAR.BZ2, TAR.XZ, TAR.ZST, encrypted ZIP, Unicode filenames and output paths, modification times, duplicate output names, bounded preview, unsafe paths, and missing split volumes.

Regression checks also cancel compression and extraction after streaming has begun, assert that no staging directories remain, and round-trip a ZIP containing more than 999 volumes. The writer and split input stream are closed before staging cleanup on Windows.

The `0.1.1.1` checks add a 24-level directory round trip, a source file truncated during streaming, and TAR hard links both within the staging tree and targeting a file in the caller's working directory. Incomplete input and hard links are rejected without published or staged output. The latest MinGW Debug run passed in 4.47 seconds. Clang-tidy 23 identified unnecessary copies/temporary strings and an exception escaping the cleanup path; those findings were fixed. Remaining diagnostics concern existing readability conventions, adjacent parameter types, and the standard library's signed I/O flag enums; the analysis is not represented as warning-free.

```powershell
ctest --test-dir build/windows-release --output-on-failure --timeout 30
ctest --test-dir build/core-tests --output-on-failure --timeout 30
```

## UI checks

`eng/verify-ui.ps1` uses UI Automation and native Common Item Dialog input on an interactive desktop. It creates its own fixtures, operates the application, and stores screenshots in a dated directory under `artifacts/ui-verification`.

- Compress multiple files and a folder, including Chinese filenames.
- Reject an invalid split size and disable ZIP-only options for other formats.
- Read the ZIP with .NET and compare entry names, contents, and modification times.
- Extract the result through the UI and compare the extracted contents.
- Capture all, none, and partial archive selections in Light and Dark appearance. Row checkboxes inherit `DefaultListViewItemStyle`, including the accent fill and native interaction states (issue #2).
- Check System, Light, and Dark appearance and capture normal, wide, compact, and minimum window sizes.
- Verify the normal window exposes the file commands and output settings without clipping, and wide windows keep the content column bounded.
- Switch between compression and extraction through the navigation pane and open or close the advanced options.
- Use native maximize/minimize buttons, double-click the WinUI title bar to restore, drag the window, and toggle navigation from the title bar.
- Capture the continuous Mica title bar in both themes, the Acrylic appearance flyout, and the inactive material fallback.
- Check Simplified Chinese labels and validation errors without translating filenames, archive format identifiers, or MiB units.
- Sample options expansion/collapse offsets to verify intermediate scroll positions, automatic visibility of expanded fields, and removal of temporary scroll space after collapse.
- Scroll the 560 x 480 DIP window and verify that the destination control remains accessible.
- Add files and folders through native pickers and create an archive in a chosen destination.
- Cancel 7Z compression of a 128 MiB fixture, then close during another compression and verify cleanup.

The screenshot utility captures screen pixels without resizing the image. Visual review checks text sharpness, Chinese glyphs, field labels, action placement, and clipping. Window dimensions passed to the script are DIPs, including the native frame.

The September 8, 2026 run passed at 144 DPI after the issue #2 fix. Screenshots under `artifacts/issue-2/ui/20260908-014601` confirm that selected row checkboxes match the header's accent fill in both Light and Dark appearance. The same run verified folder/child selection, partial extraction, selection across two archives with 262 entries, full extraction, and cancellation cleanup. High Contrast rendering still needs an interactive check; the fix inherits WinUI's theme resources without replacing its colors or control template.

The September 9 build passed the complete UI run at 144 DPI in `artifacts/optimization/final-ui/20260909-034116`. Light and Dark screenshots, normal/compact/minimum layouts, and the options motion samples were retained. In-page containers now reference WinUI's `ControlCornerRadius` (4 DIP), including the former 6-DIP drop area; buttons, inputs, Expander, and list selection retain their native templates. Windows and flyouts retain the platform's 8-DIP treatment. This follows [Microsoft's geometry guidance](https://learn.microsoft.com/en-us/windows/apps/design/style/rounded-corner).

## Standalone setup and PowerShell

`eng/verify-setup.ps1` copies only `UnfurlSetup.exe` to a directory with spaces and Chinese characters, then uses UI Automation to verify idle cancellation, retryable preparation errors, cancellation during an active dependency download, or a completed Windows App Installer page. It waits for the real Install/Reinstall/Launch action after the loading screen and does not click it. Screenshots and payload cleanup checks accompany each result.

The September 9 tests verified normal handoff with the current release feed and installed Microsoft frameworks. An isolated, short-lived certificate fixture verified that the fixed elevated helper imports its embedded public certificate into Local Machine / Trusted People, never Trusted Root. That test certificate and its private key were removed afterward. A fixture forced the normal pinned HTTPS framework download; signature/hash verification succeeded, and Windows returned the expected `0x80073D02` when redeployment met an in-use framework. Setup exposed a retryable error. Cancelling another download cleaned its temporary payload. Restoring the real dependency metadata reused the installed framework and reached the Windows confirmation page. Successful first-time deployment of every prerequisite on a pristine VM and standard-user UAC consent remain untested.

After publication, the anonymously downloaded `v0.1.1.1` setup restored a genuinely missing prerequisite. The test removed only the current user's `Microsoft.WinAppRuntime.DDLM.2.4.0.0-x6` package that had been added during this session, confirmed its absence, and launched the public setup executable alone from a directory containing spaces and Chinese characters. Setup downloaded and installed the pinned package; Windows AppX deployment event 400 confirmed a successful Add operation. No manual restoration was needed. It then reached the Windows App Installer confirmation page displaying version `0.1.1.1`, while Unfurl remained installed at `0.1.1.0` until the separate automatic-update test. Screenshots, package states, and events are retained in `artifacts/optimization/public-setup/`. This covers one missing runtime component, not all prerequisites on a pristine machine.

Setup uses WinUI typography, Mica, theme brushes, the native title bar and button styles, and a polite UI Automation live region for status changes. Its normal entry point uses the invoking user's context; elevation performs only the fixed public-certificate import. Runtime preparation uses native Windows APIs and does not start PowerShell.

All 17 repository PowerShell entry points require PowerShell 7 and pass PSScriptAnalyzer 1.24.0 with the checked-in settings. UTF-8 without a BOM is intentional for PowerShell 7. Analysis checks formatting, approved function verbs, automatic-variable use, and error-prone syntax. The shell-registration and portable-runtime scripts support `-WhatIf`; those dry runs were checked without changing Explorer registrations. `eng/inspect-update.ps1` successfully reported the public feed and `NoUpdates` from PowerShell 7. Only its WinRT query and the Windows Appx module use the documented [Windows PowerShell compatibility boundary](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_windows_powershell_compatibility?view=powershell-7.5).

## Coverage limits

Only one physical display and 150% scaling were available. Moving between displays with different DPI, other scaling settings, and a clean-machine runtime installation still need separate hardware or VM checks. Explorer integration scripts are syntax-checked; local UI verification does not install registry entries. The portable application is unpackaged and unsigned.

## MSIX and automatic updates

The release pipeline validates the MSIX manifest with the Windows SDK MakeAppx tool, signs with SHA-256, adds an RFC 3161 timestamp, and checks the certificate, Microsoft framework signatures, package/feed identities, dependency versions, stable and versioned URLs, and every asset's SHA-256 with `eng/verify-release.ps1`.

On Windows 11 x64, a signed `0.0.9.0` fixture was installed through an App Installer feed served over HTTP with byte-range support. `eng/inspect-update.ps1` confirmed enrollment and `NoUpdates`. Replacing the feed with `0.1.0.0` changed the Windows `Package.CheckUpdateAvailabilityAsync` result to `Available`. Starting the app through its Start menu identity triggered Windows to stage the update; deployment event 638 confirmed the running application was not terminated. Closing and relaunching upgraded the installed package to `0.1.0.0`, after which the API reported `NoUpdates` again. No manual `Add-AppxPackage` update or force-close option was used for that upgrade.

The same machine then uninstalled that localhost-enrolled package and installed `0.1.0.0` through the public `https://github.com/L0stInFades/Unfurl/releases/latest/download/Unfurl.appinstaller` feed. `eng/inspect-update.ps1` reported that URI and `NoUpdates`. After `v0.1.1` was published as latest, an unauthenticated fetch of the latest feed and every referenced package matched `SHA256SUMS.txt`, and the API changed to `Available` while the installed package was still `0.1.0.0`. Starting Unfurl from its Start menu identity downloaded `0.1.1.0`; deployment event 638 and AppX error `0x80073D02` showed Windows deferred the swap because the app was running. Closing and launching again from the Start menu identity started `C:\Program Files\WindowsApps\L0stInFades.Unfurl_0.1.1.0_x64__4373pcb113s44\Unfurl.exe`. The installed package was then `0.1.1.0` and the API reported `NoUpdates`. The publisher, package name, architecture, and signing thumbprint `7D6647D7B3568C1517146B098A55488ADF5A4BA1` were unchanged.

The installed app loaded WinUI from the declared Windows App Runtime framework and its C++ DLLs from the VCLibs UWPDesktop framework. The local machine already had those frameworks; dependency installation on a clean machine and the full eight-hour background schedule remain separate environment checks.

For `v0.1.1.1`, all five jobs in [CI run 34272966825](https://github.com/L0stInFades/Unfurl/actions/runs/34272966825) passed for release commit `99a893062a0d2903e901c63b14b54bf567554f10`: core tests, Windows application, formatting, clang-tidy, and release packaging. All 11 uploaded asset digests matched the signed local release. After GitHub's stable feed redirect propagated, anonymous downloads of every latest asset passed SHA-256, signature, embedded setup resource, package identity, and update-policy verification.

The public update check changed from `NoUpdates` to `Available` while `0.1.1.0` was still installed. Starting that version through `L0stInFades.Unfurl_4373pcb113s44!Unfurl` downloaded and staged `0.1.1.1`; deployment event 638 and `0x80073D02` confirmed Windows deferred the swap while the app remained running. Closing only the test-launched window and launching through the same Start menu identity opened `C:\Program Files\WindowsApps\L0stInFades.Unfurl_0.1.1.1_x64__4373pcb113s44\Unfurl.exe`. The package then reported version `0.1.1.1`, the same public feed, and `NoUpdates`. No manual Unfurl package update, final App Installer action, or force-close flag was used. Public download records are under `artifacts/optimization/public-release/`; update API responses, process paths, events, and the installed-app screenshot are under `artifacts/optimization/` with `public-update-*` and `update-*-publication.json` names.

Run the update diagnostic from PowerShell 7. Its read-only WinRT query uses an explicit Windows PowerShell compatibility session because that Windows projection requires .NET Framework; parameter handling, validation, and output run in PowerShell 7. The native setup executable does not launch PowerShell.

```powershell
pwsh -NoProfile -File eng/inspect-update.ps1 -ExpectedAvailability NoUpdates
```
