from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCENE_SHADER = ROOT / "src" / "native" / "shaders" / "scene.hlsl"
PACKAGE_ANALYZER = ROOT / "tools" / "blender_owned_map" / "analyze_skate.py"


class RuntimeOwnedAlphaContractTests(unittest.TestCase):
    def test_exact_retail_cutout_families_ignore_owned_material_tint(self) -> None:
        shader = SCENE_SHADER.read_text(encoding="utf-8")
        cutout = re.search(
            r"bool\s+exact_world_cutout\s*=(?P<body>.*?);"
            r"\s*if\s*\(exact_world_cutout\)\s*\{"
            r"(?P<clip>.*?)\}",
            shader,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(cutout)
        body = cutout.group("body")
        self.assertIn("exact_world_family > 6.5", body)
        self.assertIn("exact_world_family < 7.5", body)
        self.assertIn("exact_world_family > 8.5", body)
        self.assertIn("exact_world_family < 10.5", body)
        self.assertNotIn("tint.g", body)
        self.assertRegex(cutout.group("clip"), r"clip\s*\(\s*albedo\.a")

    def test_package_analyzer_reports_per_material_alpha_contract(self) -> None:
        analyzer = PACKAGE_ANALYZER.read_text(encoding="utf-8")
        self.assertIn('"alpha_mode": alpha_mode', analyzer)
        self.assertRegex(
            analyzer,
            r'"alpha_cutoff":\s*struct\.unpack_from\("<f",\s*fields,\s*60\)',
        )
        self.assertIn('"transparent_texels": alpha.count(0)', analyzer)
        self.assertIn('"opaque_texels": alpha.count(255)', analyzer)


if __name__ == "__main__":
    unittest.main()
