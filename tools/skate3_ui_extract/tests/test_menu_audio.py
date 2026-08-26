from __future__ import annotations

import struct
import unittest

from skate3_ui_extract.menu_audio import (
    Stream,
    _bind_splc_patches,
    _menu_event_identity,
    _menu_symbol_rows,
    _splc_playback_parameters,
    _xma1_riff,
    lookup8,
)


class MenuAudioTests(unittest.TestCase):
    def test_lookup8_matches_retail_vlt_symbols(self) -> None:
        self.assertEqual(lookup8("sk8_menu"), 0x795A5293DFD8B507)
        self.assertEqual(lookup8("fe"), 0x5831CB95F3E90598)
        self.assertEqual(lookup8("core_nav_up"), 0x73D7CC18FE21DBE9)
        self.assertEqual(lookup8("core_nav_down"), 0x7F975D2B64259AC2)
        self.assertEqual(lookup8("crossbar_up"), 0x02099DD4C4CC9E59)
        self.assertEqual(lookup8("crossbar_down"), 0x7BB8068AE28B26FC)
        self.assertEqual(lookup8("core_a_button"), 0x373D38D7EA167A71)
        self.assertEqual(lookup8("core_b_button"), 0x4D927CD64CC8B818)
        self.assertEqual(lookup8("core_fade"), 0xE5DF7B7CFEE55C92)
        self.assertEqual(lookup8("crossbar_in"), 0xE3C3762D6D1D0153)
        self.assertEqual(lookup8("crossbar_out"), 0xEFF5C21D83AD974B)
        self.assertEqual(lookup8("core_popup"), 0x13213C0417CECF40)

    def test_xma1_wrapper_preserves_payload_and_format(self) -> None:
        payload = bytes(range(32))
        wrapped = _xma1_riff(Stream(7, 0x1234, 48000, 2048, payload))
        self.assertEqual(wrapped[:4], b"RIFF")
        self.assertEqual(wrapped[8:12], b"WAVE")
        self.assertEqual(wrapped[12:16], b"fmt ")
        self.assertEqual(struct.unpack_from("<H", wrapped, 20)[0], 0x0165)
        self.assertEqual(struct.unpack_from("<I", wrapped, 36)[0], 48000)
        self.assertEqual(wrapped[52:56], b"data")
        self.assertEqual(struct.unpack_from("<I", wrapped, 56)[0], len(payload))
        self.assertEqual(wrapped[60:], payload)

    def test_inherited_menu_event_identity_uses_extended_pair(self) -> None:
        event_hash = lookup8("crossbar_up")
        collection = bytearray(0x50)
        struct.pack_into(">QQ", collection, 0x10, 0x1111, 0x2222)
        struct.pack_into(
            ">QQ",
            collection,
            0x40,
            event_hash,
            lookup8("fe"),
        )
        self.assertEqual(
            _menu_event_identity(bytes(collection), {event_hash: "crossbar_up"}),
            ("crossbar_up", event_hash, lookup8("fe"), 0x40),
        )

    def test_menu_audio_row_uses_ptrn_symbol_instead_of_adjacent_value(self) -> None:
        bin_data = bytearray(0x80)
        struct.pack_into(">IfI", bin_data, 0x10, 0, 0.7, 224)
        struct.pack_into(">IfI", bin_data, 0x20, 0, 0.27, 311)
        bin_data[0x60:0x6C] = b"crossbar_up"
        bin_data[0x6C] = 0

        rows = _menu_symbol_rows(
            bytes(bin_data),
            {
                0x10: 0x50,
                0x20: 0x60,
            },
            {"crossbar_up"},
        )

        self.assertEqual(rows["crossbar_up"]["bin_data_offset"], "0x20")
        self.assertEqual(rows["crossbar_up"]["symbol_string_offset"], "0x60")
        self.assertAlmostEqual(rows["crossbar_up"]["volume"], 0.27)
        self.assertEqual(rows["crossbar_up"]["sound_enum"], 311)

    def test_splc_event_uses_referenced_graph_not_same_numbered_graph(self) -> None:
        graph_rows = [
            {"graph": 0, "grain_count": 0, "parameters": ["unused"]},
            {"graph": 1, "grain_count": 1, "parameters": ["vertical-up"]},
        ]
        grain = {"type": "sample", "stream_indices": [30]}

        patches = _bind_splc_patches(graph_rows, [1, 0], [grain])

        self.assertEqual(patches[0]["root"], 1)
        self.assertEqual(patches[0]["parameters"], ["vertical-up"])
        self.assertEqual(patches[0]["grains"], [grain])
        self.assertEqual(patches[1]["root"], 0)
        self.assertEqual(patches[1]["grains"], [])

    def test_splc_playback_fields_match_retail_runtime_layout(self) -> None:
        playback = _splc_playback_parameters(
            [1.122462, 0.95, 0.05, 1.0, 108.5],
            [
                0.5,
                0.840896,
                1.0,
                0.0,
                0.053473,
                0.0,
                0.055,
                0.015,
                0.039,
                0.0,
                0.707107,
                0.145633,
                0.025,
                0.0,
                0.0,
                1.0,
            ],
        )

        self.assertAlmostEqual(playback["graph_gain"], 1.122462)
        self.assertAlmostEqual(playback["graph_pitch_base"], 0.95)
        self.assertAlmostEqual(playback["graph_pitch_random_range"], 0.05)
        self.assertAlmostEqual(playback["gain"], 0.5)
        self.assertAlmostEqual(playback["pitch_base"], 0.840896)
        self.assertAlmostEqual(playback["delay_seconds"], 0.053473)
        self.assertAlmostEqual(playback["gain_random_min"], 0.707107)
        self.assertAlmostEqual(playback["pitch_random_range"], 0.145633)
        self.assertAlmostEqual(
            playback["delay_random_range_seconds"], 0.025
        )


if __name__ == "__main__":
    unittest.main()
