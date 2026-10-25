# dada r16h loader snapshot

This is the Xiaomi 15 / SM8750 device snapshot from `iOS16.5-dada-dev1.0`, based on HackiOS by [OOSEX GANG](https://github.com/oosexgang/hackios-xnu-kernel).

- `loader-r16h/`: EFI loader, touch support, GPIO/clock code, memory window and Mynewt BQ27Z561 battery bridge.
- `include/sys/`: shared dada headers.
- `gpu-query/`: firmware validation/query DXE sources; hardware GPU acceleration is not implemented by this snapshot.
- `third-party/mynewt/`: upstream license, notice and pinned source references.

Rebuild the loader with Python 3 and **Zig 0.14.1**:

```powershell
python .\xnuloader\dada\Build-Loader.py --zig C:\tools\zig-0.14.1\zig.exe
```

The output defaults to `xnuloader/dada/out/loader`. The script builds the loader, applies PE relocations for base `0xB9200000`, and writes a build log/report. It neither repacks the existing boot IMG nor accesses a phone.

The source originates from HackiOS and inherits the repository's APSL 2.0 where applicable; third-party files retain their original notices. Mu platform sources and their license are under `uefi/dada/files`. See [the dada port notes](../../docs/dada/README.md) for the missing kernel/userland changes and current verification boundary.

This device snapshot is separate from the upstream `xnuloader/build.sh` entry point. Existing redwood/QEMU builds continue to use the upstream loader.
