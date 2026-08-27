#!/usr/bin/env python3
"""Regression tests for replacement-safe RWCM package embedding."""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from embed_retail_collision_archive import (  # noqa: E402
    ARCHIVE_MAGIC,
    EXTENSION_TAG,
    embed_archive,
)
from export_skate2_skybox import (  # noqa: E402
    _find_extension_offset,
    _read_extensions,
    _stored,
)


def _minimal_package(extensions: list[tuple[bytes, bytes]]) -> bytes:
    package = bytearray(b"SKATE12\0")
    package.extend(struct.pack("<I", 0x12345678))
    package.extend(struct.pack("<I", 4))
    package.extend(b"test")
    package.extend(bytes(49 * 4))
    package.extend(bytes(9 * 4))
    package.extend(_stored(b""))
    package.extend(_stored(b""))
    package.extend(_stored(b""))
    package.extend(struct.pack("<I", len(extensions)))
    for tag, payload in extensions:
        package.extend(tag)
        package.extend(struct.pack("<II", 1, len(payload)))
        package.extend(_stored(payload))
    return bytes(package)


def _archive(payload_byte: int) -> bytes:
    name = b"collision/test"
    mesh = bytes([payload_byte]) * 96
    return (
        ARCHIVE_MAGIC
        + struct.pack("<I", 1)
        + struct.pack("<I", len(name))
        + name
        + struct.pack("<I", len(mesh))
        + mesh
    )


def _decode_record(record: bytes) -> tuple[int, bytes]:
    schema, decoded_size, method, stored_size = struct.unpack_from(
        "<IIII", record, 4
    )
    stored = record[20 : 20 + stored_size]
    payload = stored if method == 0 else zlib.decompress(stored)
    if len(payload) != decoded_size:
        raise AssertionError("decoded size mismatch")
    return schema, payload


class EmbeddedRetailCollisionTests(unittest.TestCase):
    def test_replaces_rwcm_and_preserves_other_extensions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.skate"
            archive = root / "exact.rwcmset"
            output = root / "output.skate"
            source.write_bytes(
                _minimal_package(
                    [(b"SKYB", b"sky"), (EXTENSION_TAG, _archive(1))]
                )
            )
            replacement = _archive(2)
            archive.write_bytes(replacement)

            report = embed_archive(source, archive, output)

            offset = _find_extension_offset(output)
            records = _read_extensions(output, offset)
            self.assertEqual([tag for tag, _ in records], [b"SKYB", b"RWCM"])
            self.assertEqual(_decode_record(records[0][1])[1], b"sky")
            schema, embedded = _decode_record(records[1][1])
            self.assertEqual(schema, 1)
            self.assertEqual(embedded, replacement)
            self.assertEqual(report["meshes"], 1)

    def test_rejects_trailing_archive_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.skate"
            archive = root / "bad.rwcmset"
            output = root / "output.skate"
            source.write_bytes(_minimal_package([]))
            archive.write_bytes(_archive(3) + b"x")
            with self.assertRaisesRegex(ValueError, "trailing bytes"):
                embed_archive(source, archive, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
