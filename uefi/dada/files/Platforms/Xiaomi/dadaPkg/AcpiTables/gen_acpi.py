#!/usr/bin/env python3
"""Generate binary ACPI data tables for Xiaomi 15 (dada), Qualcomm SM8750 (sun).

DSDT is compiled separately from DSDT.asl via iasl. This script emits the
fixed-format data tables: APIC (MADT), GTDT, FACP (FADT), PPTT.
"""
import struct
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

OEM_ID = b"XIAOMI"       # exactly 6 bytes
OEM_TABLE_ID = b"DADA    "  # exactly 8 bytes
OEM_REV = 0x00008750
ASL_COMPILER_ID = b"INTL"
ASL_COMPILER_REV = 0x20230628

GICD_BASE = 0x16000000
GICR_BASE = 0x16080000
GICR_STRIDE = 0x40000
GICR_SIZE_PER_PE = 0x20000
TIMER_BASE = 0x16800000

# (ProcessorUID, MPIDR, efficiency_class)
CPUS = [
    (0, 0x000, 0),
    (1, 0x100, 0),
    (2, 0x200, 0),
    (3, 0x300, 0),
    (4, 0x400, 0),
    (5, 0x500, 0),
    (6, 0x10000, 2),
    (7, 0x10100, 2),
]
NUM_AFF1 = 3


def redist_addr(mpidr: int) -> int:
    aff0 = mpidr & 0xFF
    aff1 = (mpidr >> 8) & 0xFF
    aff2 = (mpidr >> 16) & 0xFF
    return GICR_BASE + (aff2 * NUM_AFF1 + aff1) * GICR_STRIDE + aff0 * GICR_SIZE_PER_PE


def checksum(data: bytearray) -> None:
    data[9] = 0
    data[9] = (-sum(data)) & 0xFF


def make_header(sig: bytes, length: int, revision: int) -> bytes:
    return struct.pack(
        "<4sIBB6s8sI4sI",
        sig, length, revision, 0, OEM_ID, OEM_TABLE_ID, OEM_REV,
        ASL_COMPILER_ID, ASL_COMPILER_REV,
    )


def build_apic() -> bytes:
    body = bytearray()
    body += struct.pack("<II", 0, 0)  # Local Apic Address, Flags

    for uid, mpidr, eff in CPUS:
        redist = redist_addr(mpidr)
        sub = bytearray()
        sub += bytes([0x0B, 0x50, 0x00, 0x00])       # Type, Length, Reserved
        sub += struct.pack("<I", uid)                 # CPU Interface Number
        sub += struct.pack("<I", uid)                 # Processor UID
        sub += struct.pack("<I", 1)                   # Flags: Enabled
        sub += struct.pack("<I", 0)                   # Parking Protocol Version
        sub += struct.pack("<I", 0x17)                # Performance Interrupt
        sub += struct.pack("<Q", 0)                   # Parked Address
        sub += struct.pack("<Q", 0)                   # Base Address
        sub += struct.pack("<Q", 0)                   # Virtual GIC Base Address
        sub += struct.pack("<Q", 0)                   # Hypervisor GIC Base Address
        sub += struct.pack("<I", 0x19)                # Virtual GIC Interrupt
        sub += struct.pack("<Q", redist)              # Redistributor Base Address
        sub += struct.pack("<Q", mpidr)               # ARM MPIDR
        sub += bytes([eff, 0x00])                     # Efficiency Class, Reserved
        sub += struct.pack("<H", 0)                   # SPE Overflow Interrupt
        assert len(sub) == 0x50, len(sub)
        body += sub

    gicd = bytearray()
    gicd += bytes([0x0C, 0x18, 0x00, 0x00])
    gicd += struct.pack("<I", 0)
    gicd += struct.pack("<Q", GICD_BASE)
    gicd += struct.pack("<I", 0)
    gicd += bytes([0x03, 0x00, 0x00, 0x00])
    assert len(gicd) == 0x18, len(gicd)
    body += gicd

    length = 36 + len(body)
    table = bytearray(make_header(b"APIC", length, 5) + body)
    checksum(table)
    return bytes(table)


def build_gtdt() -> bytes:
    body = bytearray()
    body += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)
    body += struct.pack("<I", 0)
    body += struct.pack("<II", 0x1D, 0x04)
    body += struct.pack("<II", 0x1E, 0x04)
    body += struct.pack("<II", 0x1B, 0x04)
    body += struct.pack("<II", 0x1A, 0x04)
    body += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)
    body += struct.pack("<II", 1, 0x60)

    block = bytearray()
    block += bytes([0x00])
    block += struct.pack("<H", 0x3C)
    block += bytes([0x00])
    block += struct.pack("<Q", TIMER_BASE)
    block += struct.pack("<I", 1)
    block += struct.pack("<I", 0x14)
    block += bytes([0x00, 0x00, 0x00, 0x00])
    block += struct.pack("<Q", TIMER_BASE + 0x1000)
    block += struct.pack("<Q", TIMER_BASE + 0x2000)
    block += struct.pack("<I", 0x28)
    block += struct.pack("<I", 0)
    block += struct.pack("<I", 0x26)
    block += struct.pack("<I", 0)
    block += struct.pack("<I", 0x02)
    assert len(block) == 0x3C, len(block)
    body += block

    length = 36 + len(body)
    table = bytearray(make_header(b"GTDT", length, 2) + body)
    checksum(table)
    return bytes(table)


def build_facp() -> bytes:
    buf = bytearray(0x114)
    struct.pack_into("<I", buf, 0x70, 0x00300000)
    buf[0x74] = 0x03
    buf[0x77] = 0x03
    struct.pack_into("<Q", buf, 0x78, 0x9020B4)
    buf[0x80] = 0x01
    struct.pack_into("<H", buf, 0x81, 0x0001)
    struct.pack_into("<Q", buf, 0x10C, 0x4D4F4351)
    buf[0x2D] = 0x08
    header = make_header(b"FACP", 0x114, 5)
    buf[0:36] = header
    checksum(buf)
    return bytes(buf)


def build_pptt() -> bytes:
    body = bytearray()

    # A processor node with no private resources is 20 bytes (ACPI PPTT).
    body += struct.pack("<BBHIIII", 0, 0x14, 0, 0x01, 0, 0, 0)
    pkg_offset = 36

    for uid, _, _ in CPUS:
        body += struct.pack("<BBHIIII", 0, 0x14, 0, 0x02, pkg_offset, uid, 0)

    length = 36 + len(body)
    table = bytearray(make_header(b"PPTT", length, 1) + body)
    checksum(table)
    return bytes(table)


def main():
    tables = {
        "APIC.aml": build_apic(),
        "GTDT.aml": build_gtdt(),
        "FACP.aml": build_facp(),
        "PPTT.aml": build_pptt(),
    }
    for name, data in tables.items():
        path = os.path.join(OUT_DIR, name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"Wrote {path} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
