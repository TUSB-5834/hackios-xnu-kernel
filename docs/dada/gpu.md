# dada / Adreno 830 Gen8 source port

This experimental backend ports the GMU, SMMU, HFI, secure firmware loading,
command ring and fence path from `hxfuxyy/ios-macan` commit
`bf6106be492670acd05fac0ee7b7d037a48eafd9`. That project's device is SM8845 /
Adreno 829; dada is SM8750 / Adreno 830 Gen8.0.1. Source hashes and the hardware
references are recorded in `gpu-provenance.json`.

The port changes the GPUCC offsets, chip/revision values, GMEM size (12 MiB),
64 MiB GMEM base alignment, UBWC 5.0 configuration, non-context register table,
ZAP reserved address and initial power levels. It reads firmware through the
existing APFS helper and exports the existing `RedwoodGPU` msm-compatible ABI.
The bounds-checked GMU plan is consumed for allocation and loading. Version
verification reads the version from the SM8750 firmware's DTCM payload.
The allocator rejects zero/overflow requests and invalid frees, tracks live
buffers, supports `gpu_va=2` guard gaps and bounds indirect submissions.

## Build and initialization

The portable `xnu-clang` wrapper defines `XNU_TARGET_DADA=1` when
`XNU_TARGET=dada`. The sources are listed in `xnu/redwood.patch` and copied by
`xnu/apply.sh`. A complete dada CPU/GIC/input baseline is still needed: this GPU
port does not supply an entire device target.

The backend is inactive without `dada_gpu=N`. Initialization stops at the first
failed step. `dada_gpu_pause=0` suppresses the diagnostic pauses.

| Argument | Requested sequence |
| --- | --- |
| `dada_gpu=0` | No GPU hardware access |
| `dada_gpu=1` | Read GCC and RPMh command DB |
| `dada_gpu=13` | CX clocks, SMMU, GMU firmware/start and HFI messages |
| `dada_gpu=18 dada_gpu_keep=1` | GX, SQE, authenticated ZAP, scratch-write proof, then publish RedwoodGPU |
| `dada_gpu=19` | Disabled: the inherited Adreno 829 picture's CCU/attribute layout is not valid evidence for Adreno 830 |

The APFS firmware directory is `fw/`, with `gen80000_gmu.bin`,
`gen80000_sqe.fw` and `gen80000_zap.mbn`. GMU is validated before MMIO, SQE is
required starting at step 15, ZAP at step 17. There is no missing-ZAP bypass.
The source does not bundle firmware. `gen80000_aqe.fw` belongs to the vendor
AQE/hardware-scheduler path; this direct-ring backend does not initialize AQE.
No validated fault recovery, full GPU scheduler or Metal implementation is
provided. GMU/clock/power sequencing still needs real-device validation.

## Isolated compilation and test image

In Linux/WSL, with an existing configured dada XNU build and its toolchain:

```sh
THEOS=/opt/theos python3 tools/dada/check-gpu-objects.py \
  --xnu /root/xnu-dada --env /root/kern/xnu-env.sh \
  --mesa /path/to/mesa-26.3.0-rc1 --out /path/to/object-check
bash tools/dada/tests/gpu-host.sh /path/to/gen80000_gmu.bin
```

The first command compiles five ARM64 Mach-O kernel objects and the Darwin DRM
shim, and checks the GPU interface symbols. The second runs host-only ASan and
UBSan tests of malformed firmware and randomized VA allocation/free sequences.
Neither command accesses a phone or changes the baseline XNU source/build.

`tools/dada/build-gpu-test.py` creates a new output directory, compiles GPU
objects and relinks a copy of the full baseline XNU object archive. It embeds
locally supplied GMU/SQE/AQE files only in this local diagnostic build, avoiding
changes to the user's original APFS. It records hashes and limits in
`build.json`. This is a full kernel link with incremental GPU recompilation;
it is not a rebuild of all baseline source files. The optional embedded-header
macro is only enabled by this script, and generated headers/artifacts belong
under the ignored local `tools/dada/validation/` directory.

## Mesa

`tools/dada/build-mesa.sh` carries ios-macan's Darwin Mesa patch and DRM shim,
pinned to Mesa 26.3.0-rc1 and its SHA-256. The target userland lives in a separate
repository. Building/installing `libOSMesa.8.dylib` and matching EAGL/userland
glue is a separate integration step. Compilation of the shim alone does not
validate rendering or ABI compatibility of every userland client.

## Verified scope (2026-10-25)

Five GPU-related kernel objects and the Darwin DRM shim compile against the
local configured XNU headers. GPU interface symbols resolve. Host tests pass
under ASan/UBSan; the supplied GMU file parses as 15 blocks, 4 payloads and
11 preallocations. The local diagnostic kernel links without undefined symbols
and passes ARM64 Mach-O segment bounds checks. No hardware test has been run.

The diagnostic Windows package retains the original r16h UEFI, System and Boot
APFS, uses the newly linked kernel, and provides read-only, GMU and software
control payloads. It does not modify the original release package. The local
XNU baseline is not byte-identical to the old release kernel, so input/battery
behavior may differ even with GPU disabled.

### Diagnostic r2 correction

The r1 incremental link reused exported kernel-cache flags, including a rename
of `__LASTDATA_CONST` to `__LAST`. The raw dada XNU baseline retains both segments
and gives them different early page permissions. r2 removes that transformation
and checks segment names, segment bounds, the entry point and loader ABI markers
against the baseline kernel. The regression check rejects r1 and accepts r2.
This is a confirmed build defect, but the failed r1 device run did not retain a
kernel log, so it is not confirmed as that run's sole failure cause.

The Windows launcher now captures `result`, `return-reason` and the returned
`get_staged` snapshot if a start returns immediately. `Logs` also supports an
already stopped loader. It never sends `oem log` before export, because that
command would replace a completed run's snapshot. Offline tests cover early
return, export disconnection and logs after return, in addition to prior cases.

### Diagnostic r3 GIC workaround

The retained r2 device log reports ESR `0x96000010`, PC
`0xfffffff00748fb80`, before GPU initialization. Symbol/disassembly lookup in
that exact kernel identifies a `GICR_ISENABLER0` read inside the inlined
`gic_save_firmware_state` in `gic_v3_irq_cpu_init`. The existing local baseline
has the same snapshot code; its redistributor affinity check completed before
this read aborted. The earlier platform DT warning does not identify the
faulting instruction.

`build-gpu-test.py --gic-no-firmware-snapshot` checks the diagnosed baseline
source hash, patches an isolated source copy, and replaces `gic_v3_irq.o` in
the copied archive alongside the GPU objects. The patch removes additional
firmware mask/priority snapshot and restore paths while retaining ordinary
interrupt initialization. Preprocessing verifies those paths are absent. The
existing XNU source and build remain unchanged. The patch applies only to the
recorded local baseline; it does not silently alter other GIC versions.

This is a diagnostic workaround, with no successful r3 device run yet. It also
disables firmware interrupt-state restoration when returning to the loader;
manually reboot to Android after testing. New failures require a fresh retained
log rather than assuming this change fixes the entire boot path.

### Diagnostic r4 pthread correction

The retained r3 device log confirms GIC initialization completed and the kernel
reached `bsd_init`. It then panicked in `pthread_init` because the pthread function
table was NULL, before GPU initialization. The local SDK `libpthread_kext.a`
contained only a `pthread_start` stub returning success. Both the baseline and
the diagnostic kernel linked that stub. Symbol presence alone did not establish
a working pthread implementation.

The diagnostic builder now requires `--pthread /path/to/libpthread`, pinned to
Apple libpthread-514.100.2 (`a6c70ca0e544347a0377babf748f831577ec24f9`).
`tools/dada/build-pthread-kext.py` builds its three `kern/` source files in the
output directory with the same symbol renames as the project's existing build
recipe. It checks the real function table and registration relocation. The final
kernel links that isolated archive instead of the SDK stub and verifies that
`pthread_start` calls `pthread_kext_register`. The SDK remains unchanged.

r4 retains the GIC workaround. Compilation, linkage and package checks passed;
successful boot and GPU initialization still need device evidence.

## Attribution

Credit to `hxfuxyy/ios-macan` for the original Gen8 backend and Darwin Mesa
integration; `oosexgang/hackios-xnu-kernel` for the XNU/APFS/RedwoodGPU platform;
Qualcomm/The Linux Foundation for the Gen8 hardware references. The macan driver
and DRM shim at the pinned revision contain no per-file license notice, and
that revision has no top-level LICENSE. This document records that fact and
does not assign an invented license to those files. The existing XNU license
notices and Mesa patch's embedded copyright/permission notices are retained.
The numeric register tables are derived from the referenced Qualcomm
GPL-2.0-only sources; those sources' notices must be considered separately
from the repository's APSL license. No new binary is published by this port.
