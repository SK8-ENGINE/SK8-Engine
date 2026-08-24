from __future__ import annotations

from pathlib import Path
import sys
import unittest


sys.path.insert(
    0,
    str(Path(__file__).resolve().parents[1] / "tools"),
)

from retail_texture_decode import (  # noqa: E402
    _packed_base_offset,
    _tiled_offset_2d,
    decode_b5g6r5,
)


def _make_b5g6r5_payload(
    width: int,
    height: int,
    value_at,
) -> bytes:
    payload = bytearray(4096)
    packed_x, packed_y = _packed_base_offset(width, height)
    for y in range(height):
        for x in range(width):
            offset = _tiled_offset_2d(
                x + packed_x,
                y + packed_y,
                width,
                1,
            )
            payload[offset : offset + 2] = int(value_at(x, y)).to_bytes(
                2,
                "big",
            )
    return bytes(payload)


class RetailTextureDecodeTests(unittest.TestCase):
    def test_square_packed_base_ignores_tile_origin_padding(self) -> None:
        payload = _make_b5g6r5_payload(
            16,
            16,
            lambda _x, _y: 0x841F,
        )

        rgba = decode_b5g6r5(payload, 16, 16)

        self.assertEqual(
            set(
                tuple(rgba[index : index + 4])
                for index in range(0, len(rgba), 4)
            ),
            {(131, 129, 255, 255)},
        )

    def test_wide_packed_base_uses_lower_half_of_tile(self) -> None:
        payload = _make_b5g6r5_payload(
            64,
            16,
            lambda x, y: ((x & 0x1F) << 11) | ((y & 0x3F) << 5) | 0x1F,
        )

        rgba = decode_b5g6r5(payload, 64, 16)

        first = tuple(rgba[0:4])
        last_offset = ((15 * 64) + 63) * 4
        last = tuple(rgba[last_offset : last_offset + 4])
        self.assertEqual(first, (0, 0, 255, 255))
        self.assertEqual(last, (255, 60, 255, 255))

    def test_nonpacked_texture_reads_from_tile_origin(self) -> None:
        payload = _make_b5g6r5_payload(
            32,
            32,
            lambda x, y: 0xF800 if (x + y) % 2 else 0x07E0,
        )

        rgba = decode_b5g6r5(payload, 32, 32)

        self.assertEqual(tuple(rgba[0:4]), (0, 255, 0, 255))
        self.assertEqual(tuple(rgba[4:8]), (255, 0, 0, 255))

    def test_words_are_interpreted_in_rx2_big_endian_order(self) -> None:
        payload = _make_b5g6r5_payload(
            16,
            16,
            lambda _x, _y: 0xF800,
        )

        rgba = decode_b5g6r5(payload, 16, 16)

        self.assertEqual(tuple(rgba[0:4]), (255, 0, 0, 255))


if __name__ == "__main__":
    unittest.main()
