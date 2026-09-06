# Performance budget

The release gate treats startup and idle resource use as product behavior. The WinUI page creates no archive reader, codec, preview model, or worker until a path is dropped. Archive work is bounded by the selected file streams and never keeps a timer alive while idle.

The initial release budget for an x64 Windows 11 build is:

| Metric | Budget | Measurement |
| --- | ---: | --- |
| Cold start to first interactive frame | 350 ms | Windows Performance Recorder or a timestamped UI test |
| Idle private working set after 10 s | 90 MiB | Process Explorer / `Get-Process` |
| Idle CPU after first frame | < 0.5% | 30 s sample with no input |
| Idle GPU engine activity | 0% sustained | GPUView; no composition animation is scheduled |
| Uncompressed executable | < 3 MiB | `Get-Item Unfurl.exe` |

Release builds use the vcpkg dynamic triplet so system-shared runtime dependencies are not copied into every executable. Codec initialization is lazy, output is streamed in 128 KiB blocks, and staged trees are removed on every failure path.
