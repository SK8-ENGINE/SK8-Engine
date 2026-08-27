from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCENE_SHADER = ROOT / "src" / "native" / "shaders" / "scene.hlsl"


class RuntimeOwnedHdrContractTests(unittest.TestCase):
    def test_owned_world_keeps_additive_lighting_scene_linear(self) -> None:
        shader = SCENE_SHADER.read_text(encoding="utf-8")
        branch = re.search(
            r"if\s*\(cam_pos\.w\s*<\s*-40\.5.*?"
            r"(?P<body>.*?)"
            r"\n\s*if\s*\(cam_pos\.w\s*<\s*-42\.5",
            shader,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(branch, "owned-world shader branch is missing")
        body = branch.group("body")
        self.assertIn(
            "scene_linear_additive += "
            "emissive_color * emissive_intensity;",
            body,
        )
        self.assertIn(
            "PassGamma(max(lit, 0.0)) + "
            "max(scene_linear_additive, 0.0)",
            body,
        )
        self.assertNotRegex(
            body,
            r"PassGamma\s*\(\s*(?:max\s*\(\s*)?"
            r"(?:scene_linear_additive|lit\s*\+\s*scene_linear_additive)",
            "Blender emission and local-light radiance must remain "
            "scene-linear for bloom and tone mapping",
        )

    def test_legacy_gamma_inverse_would_amplify_neon(self) -> None:
        # The HDR PassGamma high branch is 4 * (c^2 * 2/1.41^2) - 3.
        # Guard the reason for keeping Blender emission out of that path.
        authored_blue = 5.0
        incorrectly_encoded = (
            4.0 * (authored_blue**2 * (2.0 / (1.41**2))) - 3.0
        )
        self.assertGreater(incorrectly_encoded, 90.0)


if __name__ == "__main__":
    unittest.main()
