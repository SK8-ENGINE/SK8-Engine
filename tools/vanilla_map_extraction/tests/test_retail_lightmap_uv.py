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

from retail_lightmap_uv import (  # noqa: E402
    decode_decal_uvs,
    decode_lightmap_uvs,
    decode_retail_world_frame,
)


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
    @staticmethod
    def _pack_11_11_10(x: int, y: int, z: int) -> int:
        return (
            (x & 0x7FF)
            | ((y & 0x7FF) << 11)
            | ((z & 0x3FF) << 22)
        )

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

    def test_half4_primary_texcoord_exposes_decal_pair(self) -> None:
        stride = 28
        data = bytearray(stride * 2)
        struct.pack_into(">4e", data, 12, 0.1, 0.2, 0.7, 0.8)
        struct.pack_into(
            ">4e", data, stride + 12, -1.0, 2.0, 0.25, 0.75
        )
        result = decode_decal_uvs(
            bytes(data),
            vertex_buffer_offset=0,
            vertex_count=2,
            vertex_stride=stride,
            attributes=(
                Attribute(12, descriptor(12, 0x001A2360, 0)),
                Attribute(20, descriptor(20, 0x001A215A, 1)),
            ),
        )
        self.assertIsNotNone(result)
        assert result is not None
        numpy.testing.assert_allclose(
            result.values,
            ((0.7, 0.8), (0.25, 0.75)),
            rtol=0.0,
            atol=5.0e-4,
        )
        self.assertEqual(result.offset, 16)

    def test_static_world_frame_uses_short4_normal_and_usage6_binormal(
        self,
    ) -> None:
        stride = 28
        data = bytearray(stride * 2)
        struct.pack_into(
            ">4hI",
            data,
            16,
            1234,
            2000,
            16384,
            -8192,
            self._pack_11_11_10(1023, -512, 255),
        )
        struct.pack_into(
            ">4hI",
            data,
            stride + 16,
            -1234,
            -2000,
            -16384,
            8192,
            self._pack_11_11_10(-1023, 512, -255),
        )
        result = decode_retail_world_frame(
            bytes(data),
            vertex_buffer_offset=0,
            vertex_count=2,
            vertex_stride=stride,
            attributes=(
                Attribute(12, descriptor(12, 0x002C235F, 0)),
                Attribute(16, descriptor(16, 0x001A215A, 1)),
                Attribute(
                    24,
                    struct.pack(
                        ">HHIBBBB I",
                        0,
                        24,
                        0x002A2190,
                        0,
                        6,
                        0,
                        0x15,
                        1,
                    ),
                ),
            ),
        )
        self.assertIsNotNone(result)
        assert result is not None
        expected_xy = numpy.asarray(
            ((0.5, -0.25), (-0.5, 0.25)),
            dtype=numpy.float32,
        )
        expected_z = numpy.sqrt(
            1.0 - numpy.sum(expected_xy * expected_xy, axis=1)
        )
        expected_z[1] *= -1.0
        numpy.testing.assert_allclose(
            result.normals,
            numpy.column_stack((expected_xy, expected_z)),
            rtol=0.0,
            atol=5.0e-5,
        )
        numpy.testing.assert_allclose(
            result.tangents,
            (
                (1.0, -512 / 1023, 255 / 511),
                (-1.0, 512 / 1023, -255 / 511),
            ),
            rtol=0.0,
            atol=1.0e-6,
        )
        numpy.testing.assert_array_equal(
            result.tangent_handedness,
            (1.0, -1.0),
        )
        self.assertEqual(result.lightmap_format_code, 0x001A215A)
        self.assertEqual(result.tangent_format_code, 0x002A2190)


if __name__ == "__main__":
    unittest.main()
