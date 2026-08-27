from __future__ import annotations

import math
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SSAO_SHADER = ROOT / "src" / "native" / "shaders" / "ssao.hlsl"


class RuntimeSsaoSliceContractTests(unittest.TestCase):
    def test_fixed_slice_basis_preserves_three_slice_angles(self) -> None:
        shader = SSAO_SHADER.read_text(encoding="utf-8")
        declaration = re.search(
            r"static const float2 kAoSliceDirection\[AO_SLICES\]\s*="
            r"\s*\{(?P<body>.*?)\};",
            shader,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(declaration, "AO slice basis is missing")
        directions = [
            (float(x.rstrip("f")), float(y.rstrip("f")))
            for x, y in re.findall(
                r"float2\(\s*([-+0-9.eEf]+)\s*,\s*([-+0-9.eEf]+)\s*\)",
                declaration.group("body"),
            )
        ]
        self.assertEqual(len(directions), 3)
        for index, actual in enumerate(directions):
            angle = index * math.pi / 3.0
            self.assertAlmostEqual(actual[0], math.cos(angle), delta=1e-9)
            self.assertAlmostEqual(actual[1], math.sin(angle), delta=1e-9)

    def test_slice_loop_reuses_one_per_pixel_rotation(self) -> None:
        shader = SSAO_SHADER.read_text(encoding="utf-8")
        self.assertIn(
            "float2 noise_rotation = float2(cos(noise_phi), sin(noise_phi));",
            shader,
        )
        self.assertIn("float2 basis = kAoSliceDirection[s];", shader)
        self.assertNotIn(
            "(float(s) + noise_dir) * (kPi / float(AO_SLICES))",
            shader,
        )


if __name__ == "__main__":
    unittest.main()
