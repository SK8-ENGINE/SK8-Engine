from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from retail_grind_splines import (  # noqa: E402
    classify_grind_coordinate_frame,
    decode_grind_splines,
    grind_cell_translation,
    translate_native_segment_payload,
)


def _fixture() -> bytes:
    data = bytearray(0x400)
    section_offset = 0x100
    rail_count = 1
    segment_count = 2
    segment_table = 16 + 32
    section_size = segment_table + segment_count * 144
    file_table = 0x380
    struct.pack_into(">I", data, 0x20, 1)
    struct.pack_into(">I", data, 0x30, file_table)
    struct.pack_into(
        ">5I",
        data,
        file_table,
        section_offset,
        0,
        section_size,
        16,
        0,
    )
    struct.pack_into(">I", data, file_table + 20, 0x00EB0004)
    data[:7] = b"\x89RW4xb2"
    struct.pack_into(
        ">4I",
        data,
        section_offset,
        rail_count,
        segment_count,
        16,
        segment_table,
    )
    struct.pack_into(
        ">QQ4I",
        data,
        section_offset + 16,
        0x1111222233334444,
        0x5555666677778888,
        0,
        segment_table,
        segment_table + 144,
        0,
    )

    # First cubic ends at (1, 0, 0); the second returns to the origin.
    for segment, start, coefficient_a in (
        (0, (0.0, 0.0, 0.0), (1.0, 0.0, 0.0)),
        (1, (1.0, 0.0, 0.0), (-1.0, 0.0, 0.0)),
    ):
        offset = section_offset + segment_table + segment * 144
        struct.pack_into(">4f", data, offset, *coefficient_a, 0.0)
        struct.pack_into(">4f", data, offset + 48, *start, 1.0)
        struct.pack_into(">I", data, offset + 120, 16)
    return bytes(data)


class GrindPipelineTests(unittest.TestCase):
    def test_decodes_exact_closed_native_cubic(self) -> None:
        rails = decode_grind_splines(_fixture())

        self.assertEqual(len(rails), 1)
        self.assertEqual(rails[0]["spline_id"], "0x1111222233334444")
        self.assertEqual(
            rails[0]["type_signature"],
            "0x5555666677778888",
        )
        self.assertTrue(rails[0]["closed"])
        self.assertEqual(rails[0]["segment_count"], 2)
        self.assertEqual(len(rails[0]["native_segment_payloads"]), 2)
        self.assertEqual(
            len(rails[0]["native_segment_payloads"][0]),
            120 * 2,
        )

    def test_rejects_discontinuous_segments(self) -> None:
        data = bytearray(_fixture())
        second_start = 0x100 + 48 + 144 + 48
        struct.pack_into(">3f", data, second_start, 2.0, 0.0, 0.0)

        with self.assertRaisesRegex(ValueError, "discontinuous"):
            decode_grind_splines(bytes(data))

    def test_classifies_and_translates_cell_local_skate2_rail(self) -> None:
        payload = bytearray(120)
        struct.pack_into(">3f", payload, 48, 2.0, 3.0, 4.0)
        struct.pack_into(">3f", payload, 80, 1.0, 2.0, 3.0)
        struct.pack_into(">3f", payload, 96, 3.0, 4.0, 5.0)
        payload_hex = payload.hex()

        self.assertEqual(
            grind_cell_translation("cSim_900_-1700_high.xsf"),
            (900.0, 0.0, -1700.0),
        )
        self.assertEqual(
            classify_grind_coordinate_frame(
                "cSim_900_-1700_high.xsf",
                [payload_hex],
            ),
            "cell_local",
        )

        translated = bytes.fromhex(
            translate_native_segment_payload(
                payload_hex,
                (900.0, 0.0, -1700.0),
            )
        )
        self.assertEqual(
            struct.unpack_from(">3f", translated, 48),
            (902.0, 3.0, -1696.0),
        )
        self.assertEqual(
            struct.unpack_from(">3f", translated, 80),
            (901.0, 2.0, -1697.0),
        )
        self.assertEqual(
            struct.unpack_from(">3f", translated, 96),
            (903.0, 4.0, -1695.0),
        )

    def test_preserves_ambiguous_central_rail(self) -> None:
        payload = bytearray(120)
        struct.pack_into(">3f", payload, 48, 50.0, 0.0, 50.0)

        self.assertEqual(
            classify_grind_coordinate_frame(
                "cSim_100_100_high.xsf",
                [payload.hex()],
            ),
            "ambiguous",
        )


if __name__ == "__main__":
    unittest.main()
