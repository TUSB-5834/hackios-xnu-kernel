# UEFI platform snapshot for Xiaomi 15 (dada / SM8750)

`files/` preserves the available Mu-Silicium platform snapshot: `Platforms/Xiaomi/dadaPkg`, `Silicon/Qualcomm/PakalaPkg`, Qualcomm/Silicium support, BootShim, and the dada TOML configuration. `files/LICENSE` is the original Mu license.

- Mu base: `BASE` (`794a84469fa576489a9fe4877ae6505478c8b126`).
- Device-Binaries base: `Device-Binaries.BASE` (`ce3d72176a463aa18f26a200ff05f21ec8f69d6e`).
- Loader source and independent build entry point: `../../xnuloader/dada`.

This is a partial source snapshot. It does not include Mu_Basecore, Common, OpenSSL or the full dada Device-Binaries checkout. The firmware application `ufpdevicefw.efi` is local and ignored by Git; the platform DTB and generated ACPI tables are preserved with the available platform files. Obtain the original dependencies before attempting a complete FD build. `files/.gitmodules`, `files/Building.md`, and `files/Resources/Configs/dada.toml` record the upstream structure.

The bundled Android boot IMG embeds an XNU kernel and Boot APFS in addition to the UEFI components. It is retained only in the ignored local runtime bundle at `tools/dada/uefi/`, along with the system/APFS/XNUP images. It is not a source-only UEFI release.

No complete FD or boot IMG was rebuilt during this import, and no new image was boot-tested. See [dada port notes](../../docs/dada/README.md).
