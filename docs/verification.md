# Verification

## Environment

The local verification environment is Windows 11 x64, a 2560 x 1600 display at 150% scaling (144 DPI), Windows App SDK 2.4.0, and PowerShell 7. The application reports PerMonitorV2 DPI awareness through `GetWindowDpiAwarenessContext`.

## Core checks

Both MSVC Release and MinGW GCC 15.2 Debug builds pass the archive core suite. Release assertions remain enabled in the test executable. The suite checks ZIP, 7Z, TAR.GZ, TAR.BZ2, TAR.XZ, TAR.ZST, encrypted ZIP, Unicode filenames and output paths, modification times, duplicate output names, bounded preview, unsafe paths, and missing split volumes.

Regression checks also cancel compression and extraction after streaming has begun, assert that no staging directories remain, and round-trip a ZIP containing more than 999 volumes. The writer and split input stream are closed before staging cleanup on Windows.

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

## Coverage limits

Only one physical display and 150% scaling were available. Moving between displays with different DPI, other scaling settings, and a clean-machine runtime installation still need separate hardware or VM checks. Explorer integration scripts are syntax-checked; local UI verification does not install registry entries. The portable application is unpackaged and unsigned.
