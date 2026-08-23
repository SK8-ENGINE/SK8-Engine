from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct
import sys
import unittest

import numpy


sys.path.insert(
    0,
    str(Path(__file__).resolve().parents[1] / "tools"),
)

from retail_lightmap_uv import decode_lightmap_uvs  # noqa: E402


@dataclass(frozen=True)
class Attribute:
    offset: int
    descriptor: bytes


def descriptor(
    offset: int,
    format_code: int,
    usage_index: int,
) -> bytes:
    return struct.pack(
        ">HHIBBBB I",
        0,
        offset,
        format_code,
        0,
        5,
        usage_index,
        7,
        1,
    )


class RetailLightmapUvTests(unittest.TestCase):
    def test_short4_secondary_texcoord_keeps_sign_bits(self) -> None:
        stride = 28
        data = bytearray(stride * 2)
        struct.pack_into(">4h", data, 16, -16384, 8192, 123, 456)
        struct.pack_into(
            ">4h",
            data,
            stride + 16,
            32767,
            -32767,
            -123,
            -456,
        )
        result = decode_lightmap_uvs(
            bytes(data),
            vertex_buffer_offset=0,
            vertex_count=2,
            vertex_stride=stride,
            attributes=(
                Attribute(12, descriptor(12, 0x002C235F, 0)),
                Attribute(16, descriptor(16, 0x001A215A, 1)),
            ),
        )
        self.assertIsNotNone(result)
        assert result is not None
        numpy.testing.assert_allclose(
            result.values,
            ((-16384 / 32767, 8192 / 32767), (1.0, -1.0)),
        )
        self.assertEqual(result.format_code, 0x001A215A)

    def test_short2n_secondary_texcoord(self) -> None:
        data = bytearray(20)
        struct.pack_into(">2h", data, 16, 4096, 16384)
        result = decode_lightmap_uvs(
            bytes(data),
            vertex_buffer_offset=0,
            vertex_count=1,
            vertex_stride=20,
            attributes=(
                Attribute(12, descriptor(12, 0x002C235F, 0)),
                Attribute(16, descriptor(16, 0x002C2159, 1)),
            ),
        )
        self.assertIsNotNone(result)
        assert result is not None
        numpy.testing.assert_allclose(
            result.values[0],
            (4096 / 32767, 16384 / 32767),
        )

    def test_single_texcoord_has_no_lightmap_uv(self) -> None:
        result = decode_lightmap_uvs(
            bytes(16),
            vertex_buffer_offset=0,
            vertex_count=1,
            vertex_stride=16,
            attributes=(
                Attribute(12, descriptor(12, 0x002C235F, 0)),
            ),
        )
        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
