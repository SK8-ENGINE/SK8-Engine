"""Prepare the official Skate 3 University district for Blender."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from prepare_hawaiian_dream import prepare


DISTRICT_NAME = "DIST_University"


def _workspace() -> Path:
    return Path(__file__).resolve().parents[1]


def main() -> int:
    workspace = _workspace()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stream-dir",
        type=Path,
        default=(
            workspace
            / "raw"
            / "university"
            / "data"
            / "content"
            / "world"
            / "stream"
            / DISTRICT_NAME
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=workspace / "intermediate" / "university",
    )
    parser.add_argument(
        "--utt-root",
        type=Path,
        default=Path(r"C:\Users\Daddy\Documents\Skate3Research\UTT-1.1.7"),
    )
    args = parser.parse_args()
    manifest_path = prepare(
        stream_directory=args.stream_dir.resolve(),
        output_root=args.output.resolve(),
        utt_root=args.utt_root.resolve(),
        district_name=DISTRICT_NAME,
        map_name="University District",
        package_name="Official Skate 3 base game",
        cache_format="skate3-university-cache-v1",
        texture_stream_names=("Tex",),
    )
    print(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    print(json.dumps(manifest["summary"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
