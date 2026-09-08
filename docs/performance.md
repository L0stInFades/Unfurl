# Performance verification

Unfurl follows Microsoft's [WinUI performance workflow](https://learn.microsoft.com/en-us/windows/apps/performance/winui-perf): record CPU and XAML activity with Windows Performance Recorder (WPR), inspect frames and layout in Windows Performance Analyzer (WPA), then validate the resulting change. The GPU profile provides a separate check for sustained rendering while idle. A shorter animation duration alone is not evidence of lower rendering cost.

## Environment and repeatable measurements

Measurements on September 9, 2026 used Windows 11 build 26200, a Ryzen 7 8845H (8 cores / 16 logical processors), approximately 28 GiB of RAM, Windows App SDK 2.4.0, 144 DPI, and a 120 Hz display. The baseline is commit `f0675179f275a4e3dd713c0c1302e9c739663f6e`, version `0.1.1.0`. The new application version is `0.1.1.1`.

Microsoft Windows Performance Toolkit from ADK 10.1.26100.2454 was installed, and `perf_xaml.dll` was enabled in its `perfcore.ini`, as the WinUI guide requires. Both final Light traces report zero lost events and buffers. Raw ETL files, WPA CSV exports, screenshots, and JSON measurements are retained locally under `artifacts/optimization/`; system-wide ETL files are not release assets.

Run from an administrator PowerShell 7 on an interactive desktop, with other profiling and build jobs stopped:

```powershell
./eng/trace-ui.ps1 -Executable ./build/windows-release/Unfurl.exe -Output ./artifacts/perf/current.etl
$wpt = 'C:/Program Files (x86)/Windows Kits/10/Windows Performance Toolkit'
& "$wpt/wpaexporter.exe" -i ./artifacts/perf/current.etl -profile ./eng/Unfurl.wpaProfile -outputfolder ./artifacts/perf/csv
python ./eng/analyze-trace.py --csv ./artifacts/perf/csv --metadata ./artifacts/perf/current.json --refresh-hz 120 --output ./artifacts/perf/summary.json
./eng/measure-launch.ps1 -Samples 3 -SettleSeconds 10 -CpuSeconds 10 -Output ./artifacts/perf/launch.json
```

`trace-ui.ps1` uses the built-in Light profiles by default to limit instrumentation overhead; `-Detail Verbose` enables richer diagnostics. It records a cached launch, three fixed resize sweeps, five options expansion/collapse cycles, navigation, and ten seconds of idle time, with WPR markers delimiting each phase. Use the same profile detail for both versions. The WPA profile expands individual XAML and GPU rows. The analysis script excludes aggregate rows to avoid double-counting events and calculates nearest-rank P95.

`measure-launch.ps1` waits for an enabled, visible Add Files command through UI Automation, then measures private working set, private commit, and idle CPU using Windows process counters. These are cached interactive launches with automation overhead, not reboot-cold startup or first-pixel measurements. `Get-Process` working set is not a substitute for private working set.

After dependency builds stopped, three cached launches per version produced the following medians. The small timing difference is within the limits of this sample and is not treated as a demonstrated startup speedup. Idle memory is essentially unchanged.

| Measurement | Baseline | Final application |
| --- | ---: | ---: |
| Interactive command ready | 438.39 ms | 424.14 ms |
| Interactive range | 426.51–533.72 ms | 416.66–512.27 ms |
| Private working set after 10 s | 64.07 MiB | 64.20 MiB |
| Private commit after 10 s | 108.43 MiB | 109.32 MiB |
| CPU consumed during each 10 s idle sample | 0 ms | 0 ms |

## Build and memory changes

| Measurement | Baseline | New build |
| --- | ---: | ---: |
| Clean MSVC Release application build | 50.51 s | 26.27 s |
| No-op application build | 167.7 ms | 104.6 ms |
| CMake configure, dependencies already restored | 2628.7 ms | 693.5 ms |
| Uncompressed application executable | 501,760 bytes | 500,224 bytes |

These are observed local samples, not a universal speedup guarantee. The final clean measurement uses the application target with `--clean-first --parallel 4`; dependency restoration is excluded. Host background activity was not controlled. The earlier 44.69-second intermediate build included the test target and is not used for this comparison.

Stable C++/WinRT headers now use an MSVC precompiled header. Projection generation runs as a dependency-aware build command instead of on every configure. C++ module scanning is disabled because no source uses modules. Release compilation/linking uses `/Gw`, `/Gy`, `/OPT:REF`, `/OPT:ICF`, and `/INCREMENTAL:NO`, following Microsoft's [PCH guidance](https://learn.microsoft.com/en-us/cpp/build/creating-precompiled-header-files?view=msvc-170) and [linker optimization guidance](https://learn.microsoft.com/en-us/cpp/build/reference/opt-optimizations?view=msvc-170).

Compression now owns one 128 KiB heap buffer for the whole traversal. Previously each recursive call reserved a 128 KiB stack buffer, risking stack exhaustion with deep directories. Split assembly uses 128 KiB instead of 1 MiB, reducing that temporary buffer by 896 KiB. No archive payload is cached in memory. Cleared previews release their vector capacity; progress strings are reused per file; deciding whether to unwrap a single output directory examines at most two children. These are bounded allocation improvements, not a claim that idle working set falls by the same amount. Split-copy throughput across different storage devices was not benchmarked.

## Frame analysis and motion

The initial matched Verbose CPU/XAML/GPU traces found:

| Instrumented workload | Baseline | Optimized implementation |
| --- | ---: | ---: |
| Resize frame P95 | 36.78 ms | 32.99 ms |
| Resize layout total, 134 passes in each trace | 1491.90 ms | 1409.20 ms |
| Options motion frame P95 | 1.20 ms | 1.25 ms |
| Options frames exceeding 8.33 ms | 6 | 7 |
| Idle XAML frames / GPU execution | 0 / 0 ms | 0 / 0 ms |

These diagnostic traces contain substantial instrumentation and background CPU activity. Overall sampled application CPU weight was 12.05 s versus 12.39 s, so they do not establish an overall CPU reduction. Resize still had 134 frames exceeding the display refresh interval in both traces.

The final application was then compared with the baseline using the same Light profiles after dependency builds stopped:

| Final Light-profile workload | Baseline | Final application |
| --- | ---: | ---: |
| Resize frame P95 | 13.50 ms | 14.09 ms |
| Options motion frame P95 | 1.16 ms | 1.13 ms |
| Options frames exceeding 8.33 ms | 1 | 1 |
| Navigation frame P95 | 2.23 ms | 2.27 ms |
| Sampled application CPU weight | 6.44 s | 6.21 s |
| Idle XAML frames / GPU execution | 0 / 0 ms | 0 / 0 ms |

This second comparison does not confirm a consistent frame-time improvement. It shows why the initial Verbose resize result should not be presented as a speedup guarantee. Continuous resizing remains the largest frame-time cost, and the application is not claimed to hold 120 FPS during that stress workload. The final traced executable SHA-256 is `2B71357009ADECDFC43A8A545A34D42C16755FF054FA9B2EB372FFC1EA24273C`.

The UI avoids repeating four responsive-layout setters until the compact/noncompact state changes. Archive progress retains its approximately 100 ms throttle and now permits at most one pending dispatcher callback. Archive work remains on a worker thread, and the idle page has no polling timer.

Motion remains with WinUI's native Expander and ScrollViewer behavior and respects `UISettings.AnimationsEnabled`. The SDK's standard fast/normal/slow durations are 83/167/250 ms; custom shorter durations were not introduced. This follows Microsoft's [timing and easing guidance](https://learn.microsoft.com/en-us/windows/apps/design/motion/timing-and-easing). UI automation samples verify intermediate scroll offsets, reveal expanded fields, and remove temporary scroll space after collapse. The instrumented outliers are recorded above instead of being hidden by a duration change.

## Budgets and limits

The initial targets remain 350 ms cold startup, less than 90 MiB idle private working set, less than 0.5% of one CPU core during idle, no sustained idle GPU work, and a main executable under 3 MiB. Cold first-frame timing has not been measured, so the 350 ms target is not claimed as passed. Cached launches are reported separately.

The signed standalone WinUI 3 installer is approximately 799 KiB and has a 1 MiB release gate. It embeds only its public certificate, update feed, XAML, icon, and Microsoft's bootstrap DLL; it uses the static CRT and downloads only missing runtime packages in 64 KiB chunks. A native Windows preparation dialog handles a missing WinUI runtime before the WinUI page can be displayed. Installer startup does not depend on archive codecs or a PowerShell process.

One display and 150% scaling were available. A pristine-machine runtime installation, standard-user UAC consent, high text scaling, High Contrast, mixed-DPI monitors, and reboot-cold launch remain separate environment checks. See [verification coverage](verification.md) for the tested installer and archive workflows.
