from __future__ import annotations

from pathlib import Path

from .apt import inspect_apt
from .binary import FormatError
from .geo import parse_geo
from .rw4 import read_textures


def inspect_bundle(root: Path) -> dict:
    """Discover related UI files and return a portable extraction manifest."""
    root = Path(root)
    files = sorted(path for path in root.rglob("*") if path.is_file())
    by_stem: dict[str, dict[str, Path]] = {}
    for path in files:
        suffix = path.suffix.lower()
        if suffix not in (".apt", ".const", ".geo", ".rps3", ".rx2"):
            continue
        key = path.relative_to(root).with_suffix("").as_posix().lower()
        by_stem.setdefault(key, {})[suffix] = path

    bundles = []
    for key, parts in sorted(by_stem.items()):
        record: dict = {
            "name": key,
            "files": {
                suffix.lstrip("."): path.relative_to(root).as_posix()
                for suffix, path in sorted(parts.items())
            },
        }
        errors = []
        if ".apt" in parts and ".const" in parts:
            try:
                apt = inspect_apt(parts[".apt"], parts[".const"])
                root_movie = (apt.get("root") or {}).get("movie", {})
                record["timeline"] = {
                    "frames": root_movie.get("frame_count", 0),
                    "milliseconds_per_frame": root_movie.get(
                        "milliseconds_per_frame", 0
                    ),
                    "characters": len(apt["characters"]),
                }
            except (FormatError, OSError) as error:
                errors.append(f"APT: {error}")
        if ".geo" in parts:
            try:
                record["geometry"] = {
                    "shapes": len(parse_geo(parts[".geo"])["shapes"])
                }
            except (FormatError, OSError) as error:
                errors.append(f"GEO: {error}")
        arena_path = parts.get(".rx2") or parts.get(".rps3")
        if arena_path:
            try:
                arena, textures = read_textures(arena_path)
                record["textures"] = {
                    "platform": arena["platform"],
                    "count": len(textures),
                }
            except (FormatError, OSError) as error:
                errors.append(f"RW4: {error}")
        if errors:
            record["errors"] = errors
        bundles.append(record)
    return {"format": "skate3-ui-bundle-manifest", "root": str(root), "bundles": bundles}
