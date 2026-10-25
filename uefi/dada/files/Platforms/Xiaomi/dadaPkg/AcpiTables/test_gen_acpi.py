#!/usr/bin/env python3
"""Regression checks for the serialized dada MADT and PPTT layouts.

Run from any directory with: python3 path/to/test_gen_acpi.py
The checks cover binary structure, not correctness of hardware addresses.
"""

from pathlib import Path
import struct
import unittest
from unittest.mock import patch

import gen_acpi


EXPECTED_CPUS = (
    (0, 0x000, 0), (1, 0x100, 0), (2, 0x200, 0), (3, 0x300, 0),
    (4, 0x400, 0), (5, 0x500, 0), (6, 0x10000, 2), (7, 0x10100, 2),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_header(data, signature, revision=None):
    require(len(data) >= 36, "Truncated ACPI header")
    require(data[:4] == signature, "Unexpected ACPI signature")
    require(struct.unpack_from("<I", data, 4)[0] == len(data),
            "ACPI length does not match file size")
    require(sum(data) % 256 == 0, "Invalid ACPI checksum")
    if revision is not None:
        require(data[8] == revision, "Unexpected ACPI table revision")


def walk_nodes(data, offset):
    nodes = []
    while offset < len(data):
        require(offset + 2 <= len(data), "Truncated subtable header")
        kind, length = struct.unpack_from("<BB", data, offset)
        require(length >= 4, f"Invalid subtable length {length} at {offset}")
        require(offset + length <= len(data), "Subtable exceeds table boundary")
        nodes.append((offset, kind, data[offset:offset + length]))
        offset += length
    require(offset == len(data), "Subtables do not end at table boundary")
    return nodes


def validate_apic(data):
    validate_header(data, b"APIC", 5)
    require(len(data) == 708, "Unexpected dada MADT size")
    require(struct.unpack_from("<II", data, 36) == (0, 0),
            "Unexpected MADT fixed header")
    nodes = walk_nodes(data, 44)
    gicc = [node for _, kind, node in nodes if kind == 11]
    gicd = [node for _, kind, node in nodes if kind == 12]
    require(len(nodes) == 9 and len(gicc) == 8 and len(gicd) == 1,
            "Expected eight GICC structures and one GIC distributor")
    identities = []
    for node in gicc:
        require(len(node) == 80, "GICC must use the 80-byte ACPI 6.3 layout")
        uid = struct.unpack_from("<I", node, 8)[0]
        require(struct.unpack_from("<I", node, 4)[0] == uid,
                "Unexpected CPU interface number")
        require(struct.unpack_from("<I", node, 12)[0] == 1,
                "CPU must remain enabled")
        require(node[2:4] == b"\0\0" and node[77] == 0,
                "Nonzero GICC reserved field")
        identities.append((uid, struct.unpack_from("<Q", node, 68)[0], node[76]))
    require(tuple(identities) == EXPECTED_CPUS,
            "CPU UID, 64-bit MPIDR or efficiency class changed")
    require(len(gicd[0]) == 24, "GIC distributor must be 24 bytes")
    require(gicd[0][20:] == b"\x03\0\0\0",
            "Unexpected GIC version or reserved bytes")
    return nodes


def validate_pptt(data):
    validate_header(data, b"PPTT", 1)
    require(len(data) == 216, "Unexpected dada PPTT size")
    nodes = walk_nodes(data, 36)
    require(len(nodes) == 9, "Expected one package and eight CPU nodes")
    cpu_ids = []
    for index, (offset, kind, node) in enumerate(nodes):
        require(kind == 0 and len(node) == 20,
                "Processor node without private resources must be 20 bytes")
        reserved, flags, parent, uid, resources = struct.unpack_from("<HIIII", node, 2)
        require(reserved == 0 and resources == 0,
                "Unexpected reserved field or private resource count")
        if index == 0:
            require(offset == 36 and (flags, parent, uid) == (1, 0, 0),
                    "Invalid package node")
        else:
            require(flags == 2 and parent == 36,
                    "Invalid CPU flags or parent reference")
            cpu_ids.append(uid)
    require(cpu_ids == list(range(8)), "PPTT CPU IDs must match MADT UIDs")
    return nodes


class AcpiLayoutTests(unittest.TestCase):
    def test_generator_apic(self):
        validate_apic(gen_acpi.build_apic())

    def test_generator_pptt(self):
        validate_pptt(gen_acpi.build_pptt())

    def test_checked_in_tables(self):
        directory = Path(__file__).resolve().parent
        for name, validate in (("APIC.aml", validate_apic),
                               ("PPTT.aml", validate_pptt)):
            with self.subTest(table=name):
                validate((directory / name).read_bytes())

    def test_other_generated_tables_match(self):
        directory = Path(__file__).resolve().parent
        for name, build in (("GTDT.aml", gen_acpi.build_gtdt),
                            ("FACP.aml", gen_acpi.build_facp)):
            with self.subTest(table=name):
                data = (directory / name).read_bytes()
                validate_header(data, name[:4].encode("ascii"))
                self.assertEqual(data, build())

    def test_mpidr_preserves_upper_32_bits(self):
        with patch.object(gen_acpi, "CPUS", [(0, 0x100000000, 2)]):
            node = walk_nodes(gen_acpi.build_apic(), 44)[0][2]
        self.assertEqual(struct.unpack_from("<Q", node, 68)[0], 0x100000000)
        self.assertEqual(node[76], 2)

    def test_zero_length_node_is_rejected(self):
        for build, validate, offset in ((gen_acpi.build_apic, validate_apic, 44),
                                        (gen_acpi.build_pptt, validate_pptt, 36)):
            with self.subTest(table=build.__name__):
                data = bytearray(build())
                data[offset + 1] = 0
                gen_acpi.checksum(data)
                with self.assertRaisesRegex(ValueError, "Invalid subtable length"):
                    validate(data)

    def test_invalid_pptt_parent_is_rejected(self):
        data = bytearray(gen_acpi.build_pptt())
        struct.pack_into("<I", data, 56 + 8, 37)
        gen_acpi.checksum(data)
        with self.assertRaisesRegex(ValueError, "parent reference"):
            validate_pptt(data)

    def test_bad_header_is_rejected(self):
        data = bytearray(gen_acpi.build_apic())
        data[9] ^= 1
        with self.assertRaisesRegex(ValueError, "checksum"):
            validate_apic(data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
