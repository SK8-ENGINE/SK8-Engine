from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from prepare_hawaiian_dream import (  # noqa: E402
    _group_material_parameters,
    _material_metadata,
)


@dataclass
class Parameter:
    kind: str
    value: str


class MaterialPipelineTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
