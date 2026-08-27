from __future__ import annotations

import math
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SHADOW_SHADER = (
    ROOT / "src" / "native" / "shaders" / "scene_shadows.hlsli"
)


def _parse_kernel(shader: str, name: str, count: int) -> list[tuple[float, float]]:
    declaration = re.search(
        rf"static const float2 {name}\[{count}\]\s*=\s*\{{(?P<body>.*?)\}};",
        shader,
        flags=re.DOTALL,
    )
    if declaration is None:
        raise AssertionError(f"{name}[{count}] declaration is missing")
    values = [
        (float(x), float(y))
        for x, y in re.findall(
            r"float2\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)",
            declaration.group("body"),
        )
    ]
    if len(values) != count:
        raise AssertionError(
            f"{name} contains {len(values)} coordinates, expected {count}"
        )
    return values


class RuntimeShadowKernelContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.shader = SHADOW_SHADER.read_text(encoding="utf-8")

    def test_precomputed_vogel_kernels_preserve_sample_coordinates(self) -> None:
        for count in (13, 24, 26):
            actual = _parse_kernel(self.shader, f"kVogel{count}", count)
            for index, (actual_x, actual_y) in enumerate(actual):
                radius = math.sqrt((index + 0.5) / count)
                angle = index * 2.399963
                expected = (
                    math.cos(angle) * radius,
                    math.sin(angle) * radius,
                )
                self.assertAlmostEqual(actual_x, expected[0], delta=1e-9)
                self.assertAlmostEqual(actual_y, expected[1], delta=1e-9)

    def test_receiver_loops_use_constants_without_changing_tap_counts(self) -> None:
        self.assertIn("float2 o = kVogel24[j];", self.shader)
        self.assertIn(
            "return tap_count > 13 ? kVogel26[index] : kVogel13[index];",
            self.shader,
        )
        self.assertEqual(
            self.shader.count("float2 o = VariableVogelTap("),
            2,
        )
        self.assertIn("return acc / 25.0;", self.shader)
        self.assertIn(
            "int n = r_uc > 0.3 * ucpm ? 26 : 13;",
            self.shader,
        )
        self.assertIn(
            "int tap_count = r_uc > 0.3 * ucpm ? 26 : 13;",
            self.shader,
        )
        self.assertIn("float s = acc / float(n);", self.shader)
        self.assertIn("float shadow = acc / float(tap_count);", self.shader)
        self.assertNotRegex(
            self.shader,
            r"float\s+a\s*=\s*float\(j\)\s*\*\s*2\.399963",
        )
        self.assertNotRegex(
            self.shader,
            r"float\s+r\s*=\s*sqrt\(\(float\(j\)",
        )


if __name__ == "__main__":
    unittest.main()
