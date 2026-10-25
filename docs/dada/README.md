# Xiaomi 15 / dada adaptation snapshot

This branch imports the available device work from `iOS16.5-dada-dev1.0` into a fork of [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel). It retains the upstream redwood and QEMU implementation.

| Area | Available in this import |
|---|---|
| UEFI loader | r16h C/H source, shared headers, touch/battery support, GPU-query sources, Python/Zig loader build |
| Mu-Silicium platform | dada/SM8750 snapshot with original license and dependency commit IDs |
| Windows tooling | launcher, device checks, log entry points, exact release manifest, existing offline tests |
| XNU changes | Not present as source in the supplied archive; the local compiled kernel is not a substitute for those changes |
| Userland changes | Not present as source in the supplied archive; retain the upstream userland repository separately |

The runtime release identifies the loader as `dada-touch-spi-v16-mynewt-r16h`, uses an 8 GiB memory limit and persistent follow/log capture. Those describe the existing release bundle. This import does not add a working `common/build.sh dada` profile: that path would also need the original XNU/userland modifications and matching boot-image packer work.

## Entry points

- [Loader source/build](../../xnuloader/dada/README.md)
- [UEFI platform/dependencies](../../uefi/dada/README.md)
- [Windows launch/logging](../../tools/dada/README.md)
- [Source provenance](source-manifest.json)
- [Import verification](verification.json)

The existing IMG embeds an XNU kernel and Boot APFS. System/APFS/XNUP/IMG files, standalone EFI artifacts, fastboot binaries and device logs are kept locally and excluded from this commit. The source snapshot retains the available DTB and ACPI platform resources. No Apple iOS system image is uploaded in this import.

## Completion boundary

The release archive reported successful loading on dada, but those are historical results. Current verification checks the local packaged image hashes/APFS/XNUP structure, imports the source without changing its content (except the build output path), and runs the original mocked Windows launcher tests. It does not validate a rebuilt kernel, complete FD, boot IMG or real-device startup.

To integrate a complete upstream-style dada target, export the original dada XNU changes and userland changes from their source workspaces, add device-aware loader and packing entry points, restore the pinned Mu/vendor dependencies, then build and test the resulting images. The source archive alone cannot supply the missing patches.
