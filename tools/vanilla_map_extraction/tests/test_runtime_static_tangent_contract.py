from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCENE_SHADER = ROOT / "src" / "native" / "shaders" / "scene.hlsl"


class RuntimeStaticTangentContractTests(unittest.TestCase):
    def test_owned_static_tangents_cannot_activate_skinning(self) -> None:
        source = SCENE_SHADER.read_text(encoding="utf-8")
        match = re.search(
            r"bool\s+is_skinned\s*=\s*(?P<condition>[^;]+);",
            source,
        )
        self.assertIsNotNone(match, "scene vertex shader has no skinning guard")
        condition = match.group("condition")
        self.assertIn(
            "cam_pos.w >= 0.0",
            condition,
            "negative owned-world material families must disable skinning",
        )
        self.assertIn("tint.g > 0.0", condition)
        self.assertIn("wsum > 0.001", condition)

    def test_tangent_frame_uses_the_same_skinning_decision(self) -> None:
        source = SCENE_SHADER.read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"o\.tanb\s*=\s*is_skinned\s*\?",
            "static tangent decoding must not use the old tint-only test",
        )


if __name__ == "__main__":
    unittest.main()
