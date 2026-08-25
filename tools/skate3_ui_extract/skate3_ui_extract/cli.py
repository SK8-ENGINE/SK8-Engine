from __future__ import annotations

import argparse
import json
from pathlib import Path

from .apt import inspect_apt
from .big import BigArchive
from .bundle import inspect_bundle
from .career_main import compile_career_main, compile_career_main_states
from .game_settings import compile_game_settings
from .geo import parse_geo
from .menu_audio import extract_menu_audio
from .project import DEFAULT_PREFIXES, extract_project
from .retail_menu import extract_menu_page
from .rw4 import extract_textures


def write_json(path: Path, value: object, force: bool) -> None:
    path = Path(path)
    if path.exists() and not force:
        raise FileExistsError(f"refusing to overwrite {path}; pass --force")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Read-only Skate 3 front-end asset extraction tools"
    )
    commands = result.add_subparsers(dest="command", required=True)

    big_list = commands.add_parser("big-list", help="list an EB BIG v3 archive")
    big_list.add_argument("archive", type=Path)
    big_list.add_argument("--json", action="store_true")

    big_extract = commands.add_parser(
        "big-extract", help="extract an EB BIG v3 archive"
    )
    big_extract.add_argument("archive", type=Path)
    big_extract.add_argument("output", type=Path)
    big_extract.add_argument("--force", action="store_true")

    apt = commands.add_parser("apt-json", help="export APT animation data")
    apt.add_argument("apt", type=Path)
    apt.add_argument("const", type=Path)
    apt.add_argument("output", type=Path)
    apt.add_argument("--force", action="store_true")

    geo = commands.add_parser("geo-json", help="export GEO triangles and UVs")
    geo.add_argument("geo", type=Path)
    geo.add_argument("output", type=Path)
    geo.add_argument("--force", action="store_true")

    rw4 = commands.add_parser(
        "rw4-extract", help="extract RPS3/RX2 texture payloads and metadata"
    )
    rw4.add_argument("arena", type=Path)
    rw4.add_argument("output", type=Path)
    rw4.add_argument(
        "--png", action="store_true", help="decode supported Xbox 360 textures to PNG"
    )
    rw4.add_argument("--force", action="store_true")

    bundle = commands.add_parser(
        "bundle", help="inventory matching APT/GEO/texture bundles"
    )
    bundle.add_argument("root", type=Path)
    bundle.add_argument("output", type=Path)
    bundle.add_argument("--force", action="store_true")

    project = commands.add_parser(
        "project-extract",
        help="build an exact local native-UI asset cache from installed game data",
    )
    project.add_argument("game_root", type=Path)
    project.add_argument("output", type=Path)
    project.add_argument(
        "--include-dynamic",
        action="store_true",
        help="include the larger fedynamic.big front-end archive",
    )
    project.add_argument(
        "--prefix",
        action="append",
        help="archive path prefix to include; repeat for multiple roots",
    )
    project.add_argument(
        "--no-png",
        action="store_true",
        help="keep original RX2 and texture payloads without decoded PNG previews",
    )
    project.add_argument(
        "--update",
        action="store_true",
        help="reuse a current cache or refresh it when source archives changed",
    )
    project.add_argument("--force", action="store_true")

    menu_data = commands.add_parser(
        "menu-data-json",
        help="extract a source-traceable menu page from a flat retail executable image",
    )
    menu_data.add_argument("image", type=Path)
    menu_data.add_argument("title_id")
    menu_data.add_argument("output", type=Path)
    menu_data.add_argument(
        "--base-address", type=lambda value: int(value, 0), default=0x82000000
    )
    menu_data.add_argument("--force", action="store_true")

    career_main = commands.add_parser(
        "career-main-json",
        help="compile the source-authored Career > Main scene and selected copy",
    )
    career_main.add_argument("cache", type=Path)
    career_main.add_argument("image", type=Path)
    career_main.add_argument("output", type=Path)
    career_main.add_argument(
        "--party-play",
        choices=("enabled", "disabled", "unresolved"),
        default="disabled",
    )
    career_main.add_argument(
        "--progression",
        choices=("welcome", "progress", "finish"),
        default="welcome",
    )
    career_main.add_argument("--show-dlc", action="store_true")
    career_main.add_argument("--selected-option", type=int, default=0)
    career_main.add_argument(
        "--copy-profile",
        choices=("custom", "retail"),
        default="custom",
        help="render project placeholder copy or the original retail strings",
    )
    career_main.add_argument("--force", action="store_true")

    career_states = commands.add_parser(
        "career-main-states-json",
        help=(
            "compile every selectable Career > Main state and its source-authored "
            "native animation tracks"
        ),
    )
    career_states.add_argument("cache", type=Path)
    career_states.add_argument("image", type=Path)
    career_states.add_argument("output", type=Path)
    career_states.add_argument(
        "--party-play",
        choices=("enabled", "disabled", "unresolved"),
        default="disabled",
    )
    career_states.add_argument(
        "--progression",
        choices=("welcome", "progress", "finish"),
        default="welcome",
    )
    career_states.add_argument("--show-dlc", action="store_true")
    career_states.add_argument(
        "--copy-profile",
        choices=("custom", "retail"),
        default="custom",
        help="render project placeholder copy or the original retail strings",
    )
    career_states.add_argument("--force", action="store_true")

    menu_audio = commands.add_parser(
        "menu-audio-extract",
        help=(
            "extract source-mapped front-end sound events and their exact "
            "embedded XMA streams"
        ),
    )
    menu_audio.add_argument("game_root", type=Path)
    menu_audio.add_argument("output", type=Path)
    menu_audio.add_argument(
        "--ffmpeg",
        type=Path,
        help="ffmpeg executable used only to decode the extracted XMA payloads",
    )
    menu_audio.add_argument("--force", action="store_true")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if args.command == "big-list":
        archive = BigArchive(args.archive)
        if args.json:
            print(json.dumps(archive.manifest(), indent=2))
        else:
            for entry in archive.entries:
                print(
                    f"{entry.index:5d}  {entry.unpacked_size:10d}  "
                    f"c{entry.compression}  {entry.path}"
                )
    elif args.command == "big-extract":
        archive = BigArchive(args.archive)
        written = archive.extract(args.output, args.force)
        print(f"Extracted {len(written)} files to {args.output}")
    elif args.command == "apt-json":
        write_json(args.output, inspect_apt(args.apt, args.const), args.force)
        print(f"Wrote {args.output}")
    elif args.command == "geo-json":
        write_json(args.output, parse_geo(args.geo), args.force)
        print(f"Wrote {args.output}")
    elif args.command == "rw4-extract":
        manifest = extract_textures(args.arena, args.output, args.force, args.png)
        write_json(args.output / "manifest.json", manifest, args.force)
        print(f"Extracted {len(manifest['textures'])} textures to {args.output}")
    elif args.command == "bundle":
        write_json(args.output, inspect_bundle(args.root), args.force)
        print(f"Wrote {args.output}")
    elif args.command == "project-extract":
        prefixes = tuple(args.prefix) if args.prefix else DEFAULT_PREFIXES
        manifest = extract_project(
            args.game_root,
            args.output,
            include_dynamic=args.include_dynamic,
            prefixes=prefixes,
            decode_png=not args.no_png,
            force=args.force,
            update=args.update,
        )
        summary = manifest["summary"]
        if manifest.get("cache_reused"):
            print(f"UI asset cache is current: {args.output}")
        else:
            print(
                f"Extracted {summary['files']} exact UI files, "
                f"{summary['timelines']} timelines and "
                f"{summary['textures_decoded']} texture previews to {args.output}"
            )
        if summary["errors"]:
            print(
                f"Completed with {summary['errors']} recoverable parse/decode errors; "
                "see manifest.json"
            )
    elif args.command == "menu-data-json":
        write_json(
            args.output,
            extract_menu_page(
                args.image, args.title_id, base_address=args.base_address
            ),
            args.force,
        )
        print(f"Wrote {args.output}")
    elif args.command == "career-main-json":
        value = compile_career_main(
            args.cache,
            args.image,
            selected_option=args.selected_option,
            party_play_state=args.party_play,
            progression_state=args.progression,
            dlc_visible=args.show_dlc,
            copy_profile=args.copy_profile,
        )
        write_json(args.output, value, args.force)
        print(
            f"Wrote {args.output} "
            f"({value['validation']['counts']['unresolved']} unresolved fields)"
        )
    elif args.command == "career-main-states-json":
        index, variants = compile_career_main_states(
            args.cache,
            args.image,
            party_play_state=args.party_play,
            progression_state=args.progression,
            dlc_visible=args.show_dlc,
            copy_profile=args.copy_profile,
        )
        output = Path(args.output)
        for filename, scene in variants:
            write_json(output / filename, scene, args.force)
        write_json(
            output / "game_settings.json",
            compile_game_settings(args.cache, args.image),
            args.force,
        )
        write_json(output / "index.json", index, args.force)
        print(
            f"Wrote {len(variants)} source-authored Career > Main selection states "
            f"and the source-authored Game Settings screen to {output}"
        )
    elif args.command == "menu-audio-extract":
        value = extract_menu_audio(
            args.game_root,
            args.output,
            ffmpeg=args.ffmpeg,
            force=args.force,
        )
        print(
            f"Extracted {len(value['events'])} retail menu events and "
            f"{len(value['streams'])} exact embedded streams to {args.output}"
        )
    return 0
