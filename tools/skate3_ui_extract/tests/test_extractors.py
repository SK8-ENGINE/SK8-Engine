from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from skate3_ui_extract.apt import inspect_apt
from skate3_ui_extract.bitmap_font import measure_bitmap_text, parse_bitmap_font
from skate3_ui_extract.big import BigArchive
from skate3_ui_extract.binary import FormatError
from skate3_ui_extract.career_main import (
    CUSTOM_MENU_COPY,
    CUSTOM_MODE_TITLE,
    RETAIL_CAREER_BLUR_COLOR,
    RETAIL_CAREER_BLUR_KERNEL,
    _alpha_track,
    _glow_length_matrix,
    _motion_clip,
    _position_column_rows,
)
from skate3_ui_extract.geo import parse_geo
from skate3_ui_extract.language import pair_language_tables
from skate3_ui_extract.project import is_selected_ui_path
from skate3_ui_extract.refpack import decompress
from skate3_ui_extract.retail_menu import (
    extract_game_settings_menu,
    extract_menu_page,
)
from skate3_ui_extract.scene_graph import AssetCache, compose_matrix, transform_point
from skate3_ui_extract.timeline import (
    playback_frame,
    playback_frames,
    resolve_display_list,
)
from skate3_ui_extract.xenos import (
    FetchConstant,
    decode_rgba,
    encode_png,
    parse_fetch_constant,
)


class ExtractorTests(unittest.TestCase):
    def test_custom_menu_copy_has_one_unique_entry_per_retail_row(self):
        self.assertEqual(CUSTOM_MODE_TITLE, "Custom")
        self.assertEqual(len(CUSTOM_MENU_COPY), 7)
        self.assertEqual(CUSTOM_MENU_COPY[0]["label"], "Custom Maps")
        self.assertEqual(CUSTOM_MENU_COPY[1]["label"], "Custom Models")
        self.assertEqual(
            len({item["label"] for item in CUSTOM_MENU_COPY}),
            len(CUSTOM_MENU_COPY),
        )
        self.assertTrue(all(item["helper"] for item in CUSTOM_MENU_COPY))

    def test_career_backdrop_matches_settled_retail_blur_probe(self):
        self.assertEqual(RETAIL_CAREER_BLUR_KERNEL, 8.0)
        self.assertEqual(
            RETAIL_CAREER_BLUR_COLOR,
            (90 / 255, 85 / 255, 81 / 255),
        )

    def test_selected_glow_alpha_track_preserves_authored_keyframes(self):
        character = {
            "id": 20,
            "movie": {"frame_count": 3},
            "frames": [
                {
                    "index": 0,
                    "controls": [
                        {
                            "type": 3,
                            "type_name": "place_object2",
                            "flags": 0x0E,
                            "depth": 11,
                            "character_id": 19,
                            "matrix": [1, 0, 0, 1, 0, 0],
                            "color_transform": [127, 255, 255, 255, 0, 0, 0, 0],
                            "offset": 100,
                        }
                    ],
                },
                {
                    "index": 1,
                    "controls": [
                        {
                            "type": 3,
                            "type_name": "place_object2",
                            "flags": 0x09,
                            "depth": 11,
                            "color_transform": [200, 255, 255, 255, 0, 0, 0, 0],
                            "offset": 200,
                        }
                    ],
                },
                {"index": 2, "controls": []},
            ],
        }
        self.assertEqual(_alpha_track(character, 11), [127, 200, 200])

    def test_selected_glow_length_uses_actionscript_endcaps(self):
        row = "/mColumnAnim/mColumn/mSubcat0"
        length = 151.45945945945948
        outer_fill = _glow_length_matrix(
            row + "/mText/mGlow/mOuterGlow/mFill", row, length
        )
        base_fill = _glow_length_matrix(
            row + "/mText/mGlow/mGlowBase/mBase/mFill", row, length
        )
        edge_right = _glow_length_matrix(
            row + "/mText/mGlow/mGlowBase/mEdgeRight", row, length
        )
        self.assertAlmostEqual(outer_fill[0] * 16, length - 24)
        self.assertAlmostEqual(outer_fill[4], -length / 2 + 12)
        self.assertAlmostEqual(base_fill[0] * 16, length - 16)
        self.assertAlmostEqual(base_fill[4], -length / 2 + 8)
        self.assertAlmostEqual(edge_right[4], length)

    def test_position_column_preserves_selected_row_clearance(self):
        source_matrices = [
            [1.0, 0.0, 0.0, 1.0, 0.0, 0.0] for _ in range(4)
        ]

        first_column_y, first_rows = _position_column_rows(source_matrices, 0)
        self.assertEqual(first_column_y, 234.0)
        self.assertEqual(
            [first_rows[index][5] for index in range(4)],
            [0.0, 53.0, 97.0, 141.0],
        )

        second_column_y, second_rows = _position_column_rows(source_matrices, 1)
        self.assertEqual(second_column_y, 225.0)
        self.assertEqual(
            [second_rows[index][5] for index in range(4)],
            [0.0, 53.0, 106.0, 150.0],
        )

    def test_motion_clip_exports_exact_per_item_matrix_and_opacity_frames(self):
        base_item = {
            "path": "/row/mIcon",
            "bundle": "data/fe/source/screens/main/core_menu",
            "character_id": 160,
            "unit": 0,
            "matrix": [1, 0, 0, 1, 20, 30],
            "color": [1, 1, 1, 1],
        }
        base = {"primitives": [base_item], "text": []}
        first = {
            "primitives": [
                {
                    **base_item,
                    "matrix": [0.65, 0, 0, 0.65, 20, 30],
                    "color": [1, 1, 1, 0],
                }
            ],
            "text": [],
        }
        second = {"primitives": [base_item], "text": []}
        clip = _motion_clip(
            "selected",
            base,
            [first, second],
            16,
            {"label": "selected"},
        )
        self.assertEqual(clip["frame_count"], 2)
        self.assertEqual(clip["milliseconds_per_frame"], 16)
        self.assertEqual(len(clip["tracks"]), 1)
        self.assertEqual(
            clip["tracks"][0]["matrix"][0],
            [0.65, 0, 0, 0.65, 20, 30],
        )
        self.assertEqual(clip["tracks"][0]["opacity"], [0.0, 1.0])

    def test_refpack_literal_stream(self):
        self.assertEqual(decompress(b"\x10\xfb\x00\x00\x03\xffabc"), b"abc")

    def test_big_v3_list_and_extract(self):
        header = bytearray(48)
        struct.pack_into(">HHI", header, 0, 0x4542, 3, 1)
        struct.pack_into(">H", header, 8, 0)
        header[10] = 4
        struct.pack_into(">II", header, 12, 80, 32)
        header[20] = 16
        header[21] = 16
        entry_block = bytearray(32)
        struct.pack_into(">III", entry_block, 0, 7, 3, 0)
        compression_block = bytearray(16)
        name = bytearray(16)
        struct.pack_into(">H", name, 0, 0)
        name[2:10] = b"menu.apt"
        directory = bytearray(16)
        directory[:2] = b".\0"
        archive_data = bytes(
            header + entry_block + name + directory + bytearray(0) + b"APT"
        )
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "ui.big"
            path.write_bytes(archive_data)
            archive = BigArchive(path)
            self.assertEqual(archive.entries[0].path, "menu.apt")
            self.assertEqual(archive.read(archive.entries[0]), b"APT")
            output = Path(temp) / "out"
            archive.extract(output)
            self.assertEqual((output / "menu.apt").read_bytes(), b"APT")

    def test_big_rejects_unsafe_path(self):
        with self.assertRaises(FormatError):
            BigArchive._safe_relative("../escape.bin")

    def test_geo_shape_and_triangle(self):
        data = bytearray(120)
        struct.pack_into(">I", data, 4, 1)
        struct.pack_into(">I", data, 8, 16)
        struct.pack_into(">II", data, 16, 42, 1)
        struct.pack_into(">I", data, 24, 32)
        struct.pack_into(">IffffI", data, 32, 2, 1.0, 1.0, 1.0, 1.0, 7)
        struct.pack_into(">ffffff", data, 56, 1, 0, 0, 1, 0, 0)
        struct.pack_into(">I", data, 80, 1)
        struct.pack_into(">ffffff", data, 84, 0, 0, 10, 0, 0, 5)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "menu.geo"
            path.write_bytes(data)
            result = parse_geo(path)
            self.assertEqual(result["shapes"][0]["id"], 42)
            self.assertEqual(result["shapes"][0]["bounds"], [0.0, 0.0, 10.0, 5.0])
            self.assertEqual(result["shapes"][0]["units"][0]["texture_id"], 7)

    def test_empty_retail_geo(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "empty.geo"
            path.write_bytes(b"\0\0\0\0\0\0\0\0")
            self.assertEqual(parse_geo(path)["shapes"], [])

    def test_apt_timeline_label_and_background(self):
        apt = bytearray(160)
        struct.pack_into(">I", apt, 16, 9)
        struct.pack_into(">III", apt, 32, 1, 96, 0)
        struct.pack_into(">IIIII", apt, 44, 0, 0, 1280, 720, 33)
        struct.pack_into(">II", apt, 96, 2, 104)
        struct.pack_into(">II", apt, 104, 112, 120)
        struct.pack_into(">II", apt, 112, 2, 128)
        struct.pack_into(">II", apt, 120, 5, 0x11223344)
        apt[128:134] = b"intro\0"
        const = bytearray(32)
        const[:8] = b"APTCONST"
        struct.pack_into(">III", const, 20, 16, 0, 0)
        with tempfile.TemporaryDirectory() as temp:
            apt_path = Path(temp) / "menu.apt"
            const_path = Path(temp) / "menu.const"
            apt_path.write_bytes(apt)
            const_path.write_bytes(const)
            result = inspect_apt(apt_path, const_path)
            controls = result["root"]["frames"][0]["controls"]
            self.assertEqual(controls[0]["label"], "intro")
            self.assertEqual(controls[1]["color"], "#11223344")

    def test_apt_dynamic_text_and_font_metadata(self):
        apt = bytearray(256)
        # Root animation with a two-entry character table.
        struct.pack_into(">I", apt, 16, 9)
        struct.pack_into(">III", apt, 32, 0, 0, 0)
        struct.pack_into(">IIIII", apt, 44, 2, 96, 1280, 720, 33)
        struct.pack_into(">II", apt, 96, 112, 144)
        # Embedded font declaration.
        struct.pack_into(">IIII", apt, 112, 3, 0x09876543, 0, 0)
        struct.pack_into(">III", apt, 128, 220, 0, 0)
        # Dynamic text declaration.
        struct.pack_into(">IIII", apt, 144, 2, 0x09876543, 0, 0)
        struct.pack_into(">4f", apt, 160, -2.0, -2.0, 300.0, 24.0)
        struct.pack_into(">III", apt, 176, 0, 1, 0xFFAABBCC)
        struct.pack_into(">fIIIII", apt, 188, 18.0, 1, 0, 1, 236, 244)
        apt[220:227] = b"Futura\0"
        apt[236:241] = b"Hello"
        apt[244:248] = b"copy"
        const = bytearray(32)
        const[:8] = b"APTCONST"
        struct.pack_into(">III", const, 20, 16, 0, 0)
        with tempfile.TemporaryDirectory() as temp:
            apt_path = Path(temp) / "menu.apt"
            const_path = Path(temp) / "menu.const"
            apt_path.write_bytes(apt)
            const_path.write_bytes(const)
            result = inspect_apt(apt_path, const_path)
            self.assertEqual(result["characters"][0]["font"]["name"], "Futura")
            text = result["characters"][1]["text"]
            self.assertEqual(text["font_height"], 18.0)
            self.assertEqual(text["color_argb"], "#ffaabbcc")
            self.assertEqual(text["initial_text"], "Hello")
            self.assertEqual(text["variable"], "copy")

    def test_language_pair_preserves_special_glyph_codepoint(self):
        def language_file(strings: list[bytes]) -> bytes:
            declared_count = len(strings) - 2
            pool_offset = 28 + declared_count * 8
            data = bytearray(pool_offset + 8)
            struct.pack_into("<IIIII", data, 0, 0, 0, declared_count, 28, pool_offset)
            return bytes(data) + b"\0".join(strings)

        with tempfile.TemporaryDirectory() as temp:
            labels = Path(temp) / "labels.bin"
            values = Path(temp) / "english.bin"
            labels.write_bytes(
                language_file([b"ID_PROFILE", b"ID_MAP", b"", b""])
            )
            values.write_bytes(
                language_file([b"\xABProfile: Career", b"Challenge Map", b"", b""])
            )
            result = pair_language_tables(labels, values)
            self.assertEqual(result["entries"][0]["value"], "\u00abProfile: Career")
            self.assertEqual(result["pool_entries"], 4)

    def test_bitmap_font_metrics_and_character_mapping(self):
        value = (
            "Version: 3\n"
            "FullName: Test 28\n"
            "Family: Test\n"
            "Style: Normal\n"
            "Weight: 700\n"
            "Size: 28\n"
            "Ascent: 35\n"
            "GlyphMetricsMap: 2 0 0 1 2 3 4 5 6 7,1 0 8 9 10 11 12 13 14\n"
            "CharMapSet: 2 65 0,171 1\n"
            "Texture0: 1024 32 32 test.tga\n"
            "End: 0\n"
        )
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "test.bmpFont"
            path.write_text(value, encoding="cp1252")
            result = parse_bitmap_font(path)
            self.assertEqual(result["glyphs"][1]["x_advance"], 14)
            self.assertEqual(result["glyphs"][1]["atlas_bounds"], [20, -4, 30, 7])
            self.assertEqual(result["characters"][1], {"codepoint": 171, "glyph_index": 1})
            self.assertEqual(result["textures"][0]["width"], 32)
            measured = measure_bitmap_text(result, "A\u00ab", 28)
            self.assertAlmostEqual(measured["advance"], 16.8)
            self.assertAlmostEqual(measured["width"], 27.2)

    def test_timeline_resolves_moves_and_playback_stop(self):
        character = {
            "id": 7,
            "frames": [
                {
                    "index": 0,
                    "controls": [
                        {"type_name": "frame_label", "label": "selected"},
                        {
                            "type": 3,
                            "type_name": "place_object2",
                            "flags": 0x26,
                            "depth": 3,
                            "character_id": 20,
                            "matrix": [1, 0, 0, 1, 10, 20],
                            "name": "icon",
                            "offset": 100,
                        },
                    ],
                },
                {
                    "index": 1,
                    "controls": [
                        {
                            "type": 3,
                            "type_name": "place_object2",
                            "flags": 0x05,
                            "depth": 3,
                            "matrix": [1, 0, 0, 1, 12, 21],
                            "offset": 200,
                        },
                        {
                            "type_name": "do_action",
                            "first_action_opcode": 0x07,
                        },
                    ],
                },
            ],
        }
        self.assertEqual(playback_frame(character, "selected", play=True), 1)
        state = resolve_display_list(character, 1)
        self.assertEqual(state["unresolved"], [])
        item = state["objects"][0]
        self.assertEqual(item["properties"]["character_id"], 20)
        self.assertEqual(item["properties"]["matrix"][4:], [12, 21])
        self.assertEqual(item["provenance"]["matrix"]["control_offset"], 200)

    def test_timeline_playback_wraps_to_an_earlier_stop_frame(self):
        character = {
            "id": 53,
            "frames": [
                {
                    "index": 0,
                    "controls": [
                        {
                            "type_name": "do_action",
                            "first_action_opcode": 0x07,
                        }
                    ],
                },
                {"index": 1, "controls": []},
                {
                    "index": 2,
                    "controls": [
                        {"type_name": "frame_label", "label": "bounce"}
                    ],
                },
                {"index": 3, "controls": []},
            ],
        }
        self.assertEqual(playback_frames(character, "bounce"), [2, 3, 0])
        self.assertEqual(playback_frame(character, "bounce", play=True), 0)

    def test_retail_menu_page_uses_descriptor_indices_after_title(self):
        base = 0x10000000
        data = bytearray(0x4000)
        string_cursor = 0x1000

        def put_string(value: str) -> int:
            nonlocal string_cursor
            encoded = value.encode("ascii") + b"\0"
            offset = string_cursor
            data[offset : offset + len(encoded)] = encoded
            string_cursor += len(encoded)
            return base + offset

        helper = put_string("#")
        table_offset = 0x100
        category_names = (
            "Multiplayer",
            "Options",
            "SinglePlayer",
            "Create",
            "Learn",
        )
        for index, name in enumerate(category_names):
            struct.pack_into(
                ">III",
                data,
                table_offset - 5 * 12 + index * 12,
                put_string(name),
                put_string(f"ID_CROSSBAR_{name.upper()}"),
                put_string(name.lower()),
            )
        option_names = {
            0: "ReplayEditor",
            1: "ChallengeMap",
            2: "OnlineChallengeMap",
            3: "TrickGuide",
            4: "GameSettings",
        }
        for index in range(40):
            internal = option_names.get(index, f"Option{index}")
            values = (
                put_string(internal),
                put_string(f"#Label{index}"),
                put_string(f"icon{index}"),
                0xFFFFFFFF,
                helper,
            )
            struct.pack_into(">IIIII", data, table_offset + index * 20, *values)

        title_id = "ID_CROSSBAR_TEST_MODE_TITLE"
        title_offset = string_cursor
        put_string(title_id)
        array_offset = (title_offset + len(title_id) + 4) & ~3
        slots = (
            (1, 25, 10, -1),
            (2, -1),
            (0, -1),
            (3, -1),
            (4, -1),
        )
        for slot, values in enumerate(slots):
            struct.pack_into(
                ">" + "i" * len(values), data, array_offset + slot * 32, *values
            )

        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "default.bin"
            path.write_bytes(data)
            result = extract_menu_page(path, title_id, base_address=base)
            self.assertEqual(
                [option["internal_name"] for option in result["options"]],
                ["ChallengeMap", "Option25", "Option10"],
            )
            self.assertEqual(
                result["options"][0]["source_va"], base + table_offset + 20
            )
            self.assertEqual(
                [category["internal_name"] for category in result["categories"]],
                ["SinglePlayer", "Multiplayer", "Create", "Learn", "Options"],
            )

    def test_game_settings_uses_retail_descriptor_and_index_tables(self):
        base = 0x10000000
        data = bytearray(0x6000)
        string_cursor = 0x2000

        def put_string(value: str) -> int:
            nonlocal string_cursor
            encoded = value.encode("ascii") + b"\0"
            offset = string_cursor
            data[offset : offset + len(encoded)] = encoded
            string_cursor += len(encoded)
            return base + offset

        table_offset = 0x400
        title = put_string("ID_GAMESETTINGS_TITLE")
        struct.pack_into(">I", data, table_offset - 56, title)
        option_type = put_string("option")
        slider_type = put_string("slider")
        descriptor_labels = []
        for index in range(36):
            if index == 0:
                label = put_string("0")
            elif index == 1:
                label = put_string("ID_GAMESETTINGS_MUSICPLAYER")
            elif index == 2:
                label = put_string("ID_GAMESETTINGS_DIFFICULTY_SETTINGS")
            elif index == 3:
                label = put_string("ID_GAMESETTINGS_AUDIO_SETTINGS")
            elif index == 4:
                label = put_string("ID_GAMESETTINGS_VIDEO_SETTINGS")
            elif index == 5:
                label = put_string("ID_GAMESETTINGS_CONTROL_SETTINGS")
            elif index == 6:
                label = put_string("ID_GAMESETTINGS_ONLINE_SETTINGS")
            elif index == 35:
                label = put_string("ID_GAMESETTINGS_SKATEFEED_SETTINGS")
            else:
                label = put_string(f"ID_GAMESETTINGS_TEST_{index}")
            descriptor_labels.append(label)
            struct.pack_into(
                ">II",
                data,
                table_offset + index * 8,
                label,
                option_type if index in (0, 1, 2, 3, 4, 5, 6, 35) else slider_type,
            )
        array_offset = table_offset + 36 * 8
        struct.pack_into(
            ">IIIIIII",
            data,
            array_offset,
            2,
            3,
            4,
            5,
            6,
            35,
            0,
        )

        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "default.bin"
            path.write_bytes(data)
            result = extract_game_settings_menu(path, base_address=base)
            self.assertEqual(result["title_id"], "ID_GAMESETTINGS_TITLE")
            self.assertEqual(result["option_indices"], [2, 3, 4, 5, 6, 35])
            self.assertEqual(
                [item["label_id"] for item in result["options"]],
                [
                    "ID_GAMESETTINGS_DIFFICULTY_SETTINGS",
                    "ID_GAMESETTINGS_AUDIO_SETTINGS",
                    "ID_GAMESETTINGS_VIDEO_SETTINGS",
                    "ID_GAMESETTINGS_CONTROL_SETTINGS",
                    "ID_GAMESETTINGS_ONLINE_SETTINGS",
                    "ID_GAMESETTINGS_SKATEFEED_SETTINGS",
                ],
            )

    def test_scene_matrix_composition_matches_flash_affine_order(self):
        parent = [2, 0, 0, 3, 10, 20]
        local = [1, 0, 0, 1, 4, 5]
        combined = compose_matrix(parent, local)
        self.assertEqual(combined, [2, 0, 0, 3, 18, 35])
        self.assertEqual(transform_point(combined, [1, 1]), [20, 38])

    def test_scene_font_uses_exact_rgba_texture_not_preview(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            metadata = Path("metadata/data/fe/fonts/futuraheavy.font.json")
            texture_manifest = Path("assets/data/fe/fonts/futuraheavy/manifest.json")
            (root / metadata).parent.mkdir(parents=True)
            (root / texture_manifest).parent.mkdir(parents=True)
            (root / "manifest.json").write_text(
                """{
                  "format": "skate3-native-ui-asset-cache",
                  "bundles": [{
                    "name": "data/fe/fonts/futuraheavy",
                    "font": {
                      "metadata": "metadata/data/fe/fonts/futuraheavy.font.json",
                      "family": "Futura Std Medium"
                    },
                    "textures": {
                      "manifest": "assets/data/fe/fonts/futuraheavy/manifest.json"
                    }
                  }]
                }""",
                encoding="utf-8",
            )
            (root / metadata).write_text(
                '{"format":"skate3-bitmap-font"}', encoding="utf-8"
            )
            (root / texture_manifest).write_text(
                """{
                  "textures": [{
                    "rgba_file": "0000_futuraheavy.Texture.rgba",
                    "preview_file": "0000_futuraheavy.Texture.png"
                  }]
                }""",
                encoding="utf-8",
            )
            asset = AssetCache(root).font_asset("Futura Std Medium")
            self.assertIsNotNone(asset)
            self.assertTrue(asset["texture"].endswith(".rgba"))
            self.assertTrue(asset["preview"].endswith(".png"))

    def test_xbox_fetch_constant_and_dxt1_preview(self):
        fetch = parse_fetch_constant(
            bytes.fromhex("81000002000000540007e07f00000d100000000000000200")
        )
        self.assertEqual((fetch.width, fetch.height), (128, 64))
        self.assertEqual(fetch.format_name, "DXT5")
        self.assertEqual(fetch.pitch, 128)

        # A 4x4, single-color DXT1 block in the first slot of a tiled payload.
        dxt1_fetch = type(fetch)(
            width=4,
            height=4,
            format_code=18,
            format_name="DXT1",
            mipmaps=1,
            tiled=True,
            endian=1,
        )
        linear_block = struct.pack("<HHI", 0xF800, 0xF800, 0)
        tiled = bytearray(8192)
        tiled[:8] = b"".join(
            linear_block[index : index + 2][::-1] for index in range(0, 8, 2)
        )
        rgba = decode_rgba(bytes(tiled), dxt1_fetch)
        self.assertEqual(rgba[:4], b"\xff\x00\x00\xff")
        png = encode_png(4, 4, rgba)
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))

    def test_linear_dxt_rows_use_fetch_pitch(self):
        fetch = FetchConstant(
            width=4,
            height=8,
            format_code=18,
            format_name="DXT1",
            mipmaps=1,
            tiled=False,
            endian=1,
            pitch=128,
        )
        red = struct.pack("<HHI", 0xF800, 0xF800, 0)
        blue = struct.pack("<HHI", 0x001F, 0x001F, 0)
        payload = bytearray(264)
        payload[:8] = b"".join(
            red[index : index + 2][::-1] for index in range(0, 8, 2)
        )
        payload[256:264] = b"".join(
            blue[index : index + 2][::-1] for index in range(0, 8, 2)
        )
        rgba = decode_rgba(bytes(payload), fetch)
        self.assertEqual(rgba[:4], b"\xff\x00\x00\xff")
        self.assertEqual(rgba[4 * 4 * 4 : 4 * 4 * 4 + 4], b"\x00\x00\xff\xff")

    def test_project_scope_selects_only_ui_asset_formats(self):
        prefixes = ("data/fe/source/screens/", "data/fe/source/controls/")
        self.assertTrue(
            is_selected_ui_path("data/fe/source/screens/main/extras.rx2", prefixes)
        )
        self.assertTrue(
            is_selected_ui_path("data/fe/source/controls/menu_picker.apt", prefixes)
        )
        self.assertTrue(
            is_selected_ui_path("data/fe/fonts/futuraheavy.bmpFont", prefixes)
        )
        self.assertTrue(
            is_selected_ui_path(
                "data/fe/languages/labels/LANGUAGE_Labels_Global_skate3ng.BIN",
                prefixes,
            )
        )
        self.assertFalse(
            is_selected_ui_path("data/fe/cas/male/shirt.rx2", prefixes)
        )
        self.assertFalse(
            is_selected_ui_path("data/fe/source/screens/main/readme.txt", prefixes)
        )


if __name__ == "__main__":
    unittest.main()
