from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct
import sys
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from prepare_hawaiian_dream import (  # noqa: E402
    _bind_material_groups_by_guid,
    _group_material_parameters,
    _material_metadata,
    _material_texture_id,
    _texture_id,
)


@dataclass
class Parameter:
    kind: str
    value: str


def _binding_fixture() -> tuple[bytes, list[Parameter]]:
    data = bytearray(0x800)
    file_table = 0x100
    records = [
        (0x200, 0x180, 0x00EB0005),
        (0x400, 0x100, 0x00EB000B),
        (0x600, 0x80, 0x00EB0023),
        (0x680, 0x80, 0x00EB0023),
    ]
    struct.pack_into(">I", data, 0x20, len(records))
    struct.pack_into(">I", data, 0x30, file_table)
    for index, (offset, size, section_type) in enumerate(records):
        record_offset = file_table + index * 24
        struct.pack_into(
            ">5I",
            data,
            record_offset,
            offset,
            0,
            size,
            16,
            0,
        )
        struct.pack_into(">I", data, record_offset + 20, section_type)

    material_guids = [
        0x1111111122222222,
        0x3333333344444444,
        0x5555555566666666,
    ]
    material_offset = 0x200
    header_size = 32
    parameters_size = header_size + len(material_guids) * 32
    struct.pack_into(
        ">8I",
        data,
        material_offset,
        len(material_guids),
        len(material_guids),
        20,
        header_size,
        parameters_size,
        5,
        0,
        header_size,
    )
    kind_offset = 0x120
    data[material_offset + kind_offset : material_offset + kind_offset + 5] = (
        b"Name\0"
    )
    for index, guid in enumerate(material_guids):
        struct.pack_into(
            ">8I",
            data,
            material_offset + header_size + index * 32,
            kind_offset,
            0xFFFF,
            0xDEADBEEF,
            0xDEADC0DE,
            guid >> 32,
            guid & 0xFFFFFFFF,
            0,
            0,
        )

    handles = [0x00800009, 0x00800003, 0x00800006]
    external_offset = 0x400
    struct.pack_into(
        ">5I",
        data,
        external_offset,
        len(material_guids),
        20,
        20 + len(material_guids) * 24,
        0,
        0,
    )
    for index, (guid, handle) in enumerate(zip(material_guids, handles)):
        struct.pack_into(
            ">6I",
            data,
            external_offset + 20 + index * 24,
            0,
            0xFEFFFFFF,
            guid >> 32,
            guid & 0xFFFFFFFF,
            0x00EB0066,
            handle,
        )

    struct.pack_into(">I", data, 0x600 + 0x24, handles[2])
    struct.pack_into(">I", data, 0x680 + 0x24, handles[0])
    parameters = [
        Parameter("Name", "ground"),
        Parameter("AttribulatorMaterialName", "environment.default"),
        Parameter("Name", "vending_machine"),
        Parameter("AttribulatorMaterialName", "dynamicobject.default"),
        Parameter("Name", "foliage"),
        Parameter("AttribulatorMaterialName", "tree.default"),
    ]
    return bytes(data), parameters


class MaterialPipelineTests(unittest.TestCase):
    def test_external_guid_handles_control_mesh_material_order(self) -> None:
        data, parameters = _binding_fixture()
        groups = _group_material_parameters(parameters)

        selected, bindings = _bind_material_groups_by_guid(data, groups, 2)

        self.assertEqual(
            [
                _material_metadata(selected, index)["shader_name"]
                for index in range(2)
            ],
            ["tree.default", "environment.default"],
        )
        self.assertEqual(
            [binding["group_index"] for binding in bindings],
            [2, 0],
        )

    def test_skate2_material_guid_namespace_can_fall_back_to_import_order(
        self,
    ) -> None:
        data, parameters = _binding_fixture()
        changed = bytearray(data)
        # Give the first external reference a GUID that differs from its Name
        # parameter, matching the Skate 2 BAM material-table variant.
        struct.pack_into(">2I", changed, 0x400 + 20 + 8, 0xAAAAAAAA, 0xBBBBBBBB)
        groups = _group_material_parameters(parameters)

        selected, bindings = _bind_material_groups_by_guid(
            bytes(changed),
            groups,
            2,
            allow_import_order_fallback=True,
        )

        self.assertEqual(
            [
                _material_metadata(selected, index)["shader_name"]
                for index in range(2)
            ],
            ["tree.default", "environment.default"],
        )
        self.assertTrue(
            all(
                binding["strategy"] == "external_reference_order"
                for binding in bindings
            )
        )

    def test_missing_diffuse_does_not_shift_following_meshes(self) -> None:
        parameters = [
            Parameter("Name", "ground"),
            Parameter("diffuse", "ground_0x1111111111111111"),
            Parameter("Name", "water"),
            Parameter("AttribulatorMaterialName", "ocean.default"),
            Parameter("normal", "water_normal"),
            Parameter("Name", "foliage"),
            Parameter("AttribulatorMaterialName", "tree.default"),
            Parameter("transparent", "leaves_0x2222222222222222"),
            Parameter("diffuse", "leaves_0x2222222222222222"),
        ]
        groups = _group_material_parameters(parameters)

        self.assertEqual(
            _material_metadata(groups, 0)["texture_id"],
            "0x1111111111111111",
        )
        self.assertIsNone(_material_metadata(groups, 1)["texture_id"])
        self.assertEqual(
            _material_metadata(groups, 2)["texture_id"],
            "0x2222222222222222",
        )

    def test_retail_shader_controls_alpha_mode(self) -> None:
        parameters = [
            Parameter("Name", "opaque"),
            Parameter("diffuse", "wall_0x1111111111111111"),
            Parameter("Name", "cutout"),
            Parameter("AttribulatorMaterialName", "tree.default"),
            Parameter("transparent", "leaf_0x2222222222222222"),
            Parameter("Name", "glass"),
            Parameter(
                "AttribulatorMaterialName",
                "environment.transparent",
            ),
            Parameter("transparent", "glass_0x3333333333333333"),
        ]
        groups = _group_material_parameters(parameters)

        self.assertEqual(_material_metadata(groups, 0)["alpha_mode"], 0)
        self.assertEqual(_material_metadata(groups, 1)["alpha_mode"], 1)
        self.assertEqual(_material_metadata(groups, 2)["alpha_mode"], 2)

    def test_transparent_channel_is_an_albedo_fallback(self) -> None:
        groups = _group_material_parameters(
            [
                Parameter("Name", "fence"),
                Parameter(
                    "AttribulatorMaterialName",
                    "environmentsimple.alphatest",
                ),
                Parameter(
                    "transparent",
                    "fence_0x4444444444444444",
                ),
            ]
        )
        metadata = _material_metadata(groups, 0)

        self.assertEqual(metadata["texture_id"], "0x4444444444444444")
        self.assertEqual(metadata["texture_channel"], "transparent")
        self.assertEqual(metadata["alpha_mode"], 1)

    def test_all_retail_texture_channels_survive_material_binding(self) -> None:
        groups = _group_material_parameters(
            [
                Parameter("Name", "lit_wall"),
                Parameter(
                    "diffuse",
                    "wall_d_0x1111111111111111",
                ),
                Parameter(
                    "normal",
                    "wall_n_0x2222222222222222",
                ),
                Parameter(
                    "specular",
                    "wall_s_0x3333333333333333",
                ),
                Parameter(
                    "lightmap",
                    "wall_l_0x4444444444444444",
                ),
                Parameter(
                    "macrooverlay",
                    "wall_m_0x5555555555555555",
                ),
            ]
        )

        metadata = _material_metadata(groups, 0)

        self.assertEqual(
            metadata["retail_texture_ids"],
            {
                "diffuse": "0x1111111111111111",
                "normal": "0x2222222222222222",
                "specular": "0x3333333333333333",
                "lightmap": "0x4444444444444444",
                "macrooverlay": "0x5555555555555555",
            },
        )

    def test_symbolic_skate2_layerpage_resolves_to_stable_texture_id(
        self,
    ) -> None:
        source_name = (
            "layerpage_[0x0000000c03e38704]"
            "[0x000002f003e38705]"
            "[0x000495ec03e38706]_HighLOD_0056"
        )
        texture_id = _texture_id(
            b"prefix\0" + source_name.encode("ascii") + b".Texture\0",
            0x1234,
        )

        self.assertEqual(
            texture_id,
            _material_texture_id(source_name),
        )
        self.assertTrue(texture_id.startswith("retail_symbolic_"))
        self.assertLessEqual(len(texture_id), 63)

    def test_symbolic_skate2_chromaticity_resolves_to_stable_texture_id(
        self,
    ) -> None:
        source_name = (
            "chromo_[0x0000000c03e38704]"
            "[0x000002f003e38705]"
            "[0x000495ec03e38706]_HighLOD_0056"
        )
        texture_id = _texture_id(
            b"prefix\0" + source_name.encode("ascii") + b".Texture\0",
            0x5678,
        )

        self.assertEqual(
            texture_id,
            _material_texture_id(source_name),
        )
        self.assertTrue(texture_id.startswith("retail_symbolic_"))
        self.assertLessEqual(len(texture_id), 63)


if __name__ == "__main__":
    unittest.main()
