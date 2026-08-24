from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCENE_RUNTIME = ROOT / "src" / "skate3_native_scene_gpu.cpp"
SCENE_STATE = ROOT / "src" / "skate3_native_scene_gpu_internal.h"
SCENE_SHADER = ROOT / "src" / "native" / "shaders" / "scene.hlsl"


class RuntimeOwnedDecalContractTests(unittest.TestCase):
    def test_decal_upload_has_a_distinct_role_and_cache_entry(self) -> None:
        runtime = SCENE_RUNTIME.read_text(encoding="utf-8")
        state = SCENE_STATE.read_text(encoding="utf-8")

        self.assertRegex(
            runtime,
            r"enum class OwnedTextureRole[^}]+\bDecal\b",
        )
        self.assertRegex(
            runtime,
            r"decal_family\s*\?\s*OwnedTextureRole::Decal",
        )
        self.assertIn(
            "(std::uint64_t(texture_id) << 8u)",
            runtime,
            "texture role must participate in the owned texture cache key",
        )
        self.assertRegex(
            state,
            r"unordered_map<uint64_t,\s*GuestTexture>\s+"
            r"owned_map_textures",
        )

    def test_generated_decal_mips_are_alpha_weighted(self) -> None:
        runtime = SCENE_RUNTIME.read_text(encoding="utf-8")
        branch = re.search(
            r"role\s*==\s*OwnedTextureRole::Decal(?P<body>.*?)"
            r"\n\s*}\s*else\s*{",
            runtime,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(branch, "owned decal mip branch is missing")
        body = branch.group("body")
        self.assertIn("alpha_weight", body)
        self.assertIn("weighted_color", body)
        self.assertRegex(
            body,
            r"input\[pixel\s*\+\s*channel\]\)\s*\*\s*"
            r"input\[pixel\s*\+\s*3\]",
        )
        self.assertRegex(
            body,
            r"alpha_weight\s*==\s*0",
        )

    def test_guest_exact_decal_equation_remains_unchanged(self) -> None:
        shader = SCENE_SHADER.read_text(encoding="utf-8")
        self.assertIn(
            "dlin = lerp(dlin, dk.rgb * dk.rgb, dk.a);",
            shader,
            "owned mip correction must not alter the guest retail shader",
        )


if __name__ == "__main__":
    unittest.main()
