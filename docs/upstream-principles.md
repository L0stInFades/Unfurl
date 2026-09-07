# Upstream principles mapped to Windows

The upstream Unfold repository was inspected at commit `1bbff88` on 2026-09-07. Its README, `AGENTS.md`, `ArchiveCore`, `Compression`, `FoldView`, test fixtures, and release scripts establish the following product and engineering constraints.

| Unfold observation | Unfurl decision |
| --- | --- |
| The whole product is organized around one drop surface for compression and extraction. | MainPage has one drag target; a single archive gets a bounded preview, while multiple archives are kept as a sequential extraction batch. |
| Archive preview reads headers without creating output and caps the number of rows. | `ArchiveEngine::preview` limits enumeration to 250 entries and reports truncation. |
| libarchive work stays on a serial background queue; cancellation is checked inside compressed data. | Core methods accept a cancellation callback and the WinUI page runs them on a worker thread. |
| Extraction is staged and committed only after the writer closes; existing names are preserved. | `make_staging`, `publish_staged_directory`, and `unique_path` enforce the same invariant. |
| Paths, symbolic links, hard links, and special files are treated as hostile input. | `safe_relative_path` rejects escapes and link targets are validated before disk writes. |
| Metal is prepared only for a short visible transition and released at rest. | Unfurl uses WinUI controls and a drag highlight, with no application render loop; progress is event-driven. |
| Release scripts strip, sign, embed codec licenses, and keep diagnostics out of production. | `package-release.ps1` emits a small portable artifact; CI separates core and application jobs. |

Windows-specific changes are deliberate: Segoe UI Variable typography with a CJK fallback, Mica, WinUI controls, the native title bar, PerMonitorV2 DPI scaling, `MddBootstrapInitialize2` for Windows App SDK 2.4, and per-user Explorer commands. The archive core remains UI-agnostic so it can be tested without a XAML host.

The upstream project is MIT licensed and embeds 7-Zip under its own licenses. Unfurl's core uses the vcpkg libarchive port for ZIP/TAR families and native 7Z writing, while reading 7Z/RAR is delegated to libarchive's format readers. Passwords and split volumes remain ZIP-only because libarchive's 7Z writer does not expose those options.
