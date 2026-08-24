from __future__ import annotations

from pathlib import Path
import unittest


LAUNCHER = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "Launch-UniversityVisualCheck.ps1"
)


class UniversityLauncherContractTests(unittest.TestCase):
    def test_visual_check_uses_native_monitor_fullscreen(self) -> None:
        source = LAUNCHER.read_text(encoding="utf-8")

        self.assertIn("'--fullscreen=true'", source)
        self.assertNotIn("'--fullscreen=false'", source)
        self.assertNotIn("--window_width=1280", source)
        self.assertNotIn("--window_height=720", source)
        self.assertNotIn("--draw_resolution_scale", source)
        self.assertNotIn("--resolution_scale", source)


if __name__ == "__main__":
    unittest.main()
