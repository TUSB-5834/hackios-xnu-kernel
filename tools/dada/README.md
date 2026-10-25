# Windows launcher for the dada r16h release

This imports the Windows entry points from `iOS16.5-dada-dev1.0` (iOS 16.5 / 20F66, r16h-wallpaper-r1). Python is not required to start the existing bundle; Windows PowerShell 5.1 is supported.

## Local files

The following paths are relative to this directory. They are local resources excluded from Git:

```text
firmware/                         System and Boot APFS, XNU kernel, XNUP payload
uefi/hackios-dada-v16-wallpaper-r16h-r1.img
tools/platform-tools/             fastboot.exe and its original DLL dependencies
logs/ and log-out/                runtime logs
validation/                      offline launcher test output
```

`package.json` records the exact image sizes and SHA-256 hashes. The original release files have been copied into this checkout locally. A fresh clone must supply these files from the matching local release bundle; upstream iOS download/build scripts do not reproduce the unpublished dada kernel/userland changes.

## Run

- `Start-iOS.bat`: menu for temporary boot, log capture, device checks and verification.
- `Get-iOS-Logs.bat` / `Get-Logs.cmd`: attach to the supported running loader and collect logs.
- `Check-Device.cmd`: check the attached device.
- `Verify-Files.cmd`: offline SHA-256 and APFS/XNUP checks; no USB commands.
- `tests/Test-Launcher.ps1`: existing offline mocked launcher scenarios; no real device commands.

The phone uses the stock Android Bootloader driver in FASTBOOT and WinUSB for xnuloader (VID 18D1 / PID D00D). Startup verifies `product=dada` and the r16h loader identity. The launcher uploads the system and payload into RAM and starts the kernel; it does not flash or erase partitions. Rebooting loses this RAM session.

The only launcher code change during import is an explicit rejection of absolute/drive-qualified package paths. All release filenames, hashes, boot arguments and device checks are retained. The package manifest is release metadata, not an npm package.
