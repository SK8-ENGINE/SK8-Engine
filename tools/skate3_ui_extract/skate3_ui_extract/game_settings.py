from __future__ import annotations

import copy
import json
from pathlib import Path

from .binary import FormatError
from .bitmap_font import measure_bitmap_text
from .career_main import (
    SELECTED_GLOW_TRACK,
    SELECTED_INNER_TEXT_TRACK,
    SELECTED_OUTER_TEXT_TRACK,
    _glow_length_matrix,
    _motion_clip,
    _scene_item_key,
    compile_career_main,
)
from .retail_menu import extract_game_settings_menu
from .scene_graph import (
    AssetCache,
    SceneFlattener,
    compose_matrix,
    transform_point,
)
from .timeline import playback_frame, resolve_display_list


OPTIONS_BUNDLE = "data/fe/source/screens/options/options"
PANEL_BUNDLE = "source/controls/panel"
OPTION_ROW_BUNDLE = "source/controls/highlight_option"
HIGHLIGHT_TEXT_BUNDLE = "source/controls/menu_part_text_hilite"

PANEL_MARGIN_LEFT = 16.0
PANEL_MARGIN_TOP = 12.0
PANEL_MARGIN_HEADER = 8.0
PANEL_HEADER_TEXT_HEIGHT = 24.0
PANEL_HEADER_HEIGHT = PANEL_HEADER_TEXT_HEIGHT + 2.0 * PANEL_MARGIN_HEADER
PANEL_BUTTON_HELP_OFFSET = 32.0
PANEL_BOTTOM_MARGIN = 16.0
MINIMUM_CONTENT_WIDTH = 270.0
MINIMUM_CONTENT_HEIGHT = 192.0
OPTION_HEIGHT = 36.0
OPTION_ORIGIN_X = -44.599998474121094
OPTION_ORIGIN_Y = 6.0
OPTION_TITLE_X = 45.20000076293945


def _localized(language: dict) -> dict[str, dict]:
    result = {}
    for item in language["entries"]:
        if item["label"] and item["label"] not in result:
            result[item["label"]] = item
    return result


def _transform_scene(
    scene: dict,
    matrix: list[float],
    *,
    alpha: float = 1.0,
    prefix: str = "",
) -> dict:
    result = copy.deepcopy(scene)
    for primitive in result["primitives"]:
        primitive["path"] = prefix + primitive["path"]
        primitive["matrix"] = compose_matrix(matrix, primitive["matrix"])
        primitive["color"][3] *= alpha
        for triangle in primitive["triangles"]:
            for vertex in triangle:
                vertex["position"] = transform_point(matrix, vertex["position"])
    for text in result["text"]:
        text["path"] = prefix + text["path"]
        text["matrix"] = compose_matrix(matrix, text["matrix"])
        text["alpha"] *= alpha
    return result


def _append_scene(target: dict, source: dict, draw_order: int) -> int:
    for plural in ("primitives", "text"):
        for item in source[plural]:
            item["draw_order"] = draw_order
            draw_order += 1
            target[plural].append(item)
    target["unresolved"].extend(source.get("unresolved", []))
    return draw_order


def _panel_matrices(width: float, height: float) -> dict[str, list[float]]:
    shadow_horizontal = (width - 16.0) / 8.0
    shadow_vertical = (height - 16.0) / 8.0
    horizontal_fill = (width - 16.0) / 16.0
    core_height = (
        height
        - PANEL_HEADER_HEIGHT
        - PANEL_BUTTON_HELP_OFFSET
        - PANEL_BOTTOM_MARGIN
    )
    return {
        "/panelShadow/topLeft": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "/panelShadow/topCenter": [
            shadow_horizontal,
            0.0,
            0.0,
            1.0,
            width / 2.0,
            0.0,
        ],
        "/panelShadow/topRight": [0.0, 1.0, -1.0, 0.0, width, 0.0],
        "/panelShadow/botRight": [-1.0, 0.0, 0.0, -1.0, width, height],
        "/panelShadow/botLeft": [0.0, -1.0, 1.0, 0.0, 0.0, height],
        "/panelShadow/midLeft": [
            1.0,
            0.0,
            0.0,
            shadow_vertical,
            0.0,
            height / 2.0,
        ],
        "/panelShadow/midRight": [
            -1.0,
            0.0,
            0.0,
            shadow_vertical,
            width,
            height / 2.0,
        ],
        "/panelShadow/botCenter": [
            shadow_horizontal,
            0.0,
            0.0,
            -1.0,
            width / 2.0,
            height,
        ],
        "/panelParts/panelTopFill": [
            horizontal_fill,
            0.0,
            0.0,
            0.5,
            8.0,
            0.0,
        ],
        "/panelParts/panelTopLeft": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "/panelParts/panelTopRight": [0.0, 1.0, -1.0, 0.0, width, 0.0],
        "/panelParts/panelBottomRight": [
            -1.0,
            0.0,
            0.0,
            -1.0,
            width,
            height,
        ],
        "/panelParts/panelBottomLeft": [
            0.0,
            -1.0,
            1.0,
            0.0,
            0.0,
            height,
        ],
        "/panelParts/panelBottomFill": [
            horizontal_fill,
            0.0,
            0.0,
            0.5,
            8.0,
            height - 8.0,
        ],
        "/panelParts/panelCoreFill": [
            width / 16.0,
            0.0,
            0.0,
            core_height / 16.0,
            0.0,
            PANEL_HEADER_HEIGHT + 8.0,
        ],
        "/panelParts/panelHeaderFill": [
            width / 16.0,
            0.0,
            0.0,
            PANEL_HEADER_HEIGHT / 16.0,
            0.0,
            8.0,
        ],
        "/panelParts/panelButtonsFill": [
            width / 16.0,
            0.0,
            0.0,
            PANEL_BUTTON_HELP_OFFSET / 16.0,
            0.0,
            height - PANEL_BUTTON_HELP_OFFSET - 8.0,
        ],
    }


def _flatten_panel(
    cache: AssetCache,
    title: str,
    width: float,
    height: float,
    origin_x: float,
    origin_y: float,
) -> tuple[dict, dict]:
    matrices = _panel_matrices(width, height)
    hidden = {
        "/panelLabels",
        "/panelSections",
        "/panelGakBlue",
        "/panelGakYellow",
        "/statusBox",
        "/statusLabel",
        "/buttonHelp",
        "/buttonHelpLower",
        "/panelPatternFill",
        "/panelParts/panelTabsFill",
    }

    def state_provider(path: str, bundle: dict, character: dict) -> dict:
        state = {}
        if path in hidden:
            state["visible"] = False
        if path in matrices:
            state["matrix"] = matrices[path]
        if path == "/panelTitle/titleText":
            state["text"] = title
        if character.get("movie", {}).get("frame_count", 0) > 1:
            state["frame"] = 0
        return state

    panel = SceneFlattener(cache, state_provider).flatten(PANEL_BUNDLE, 97)
    if panel["unresolved"]:
        raise FormatError(f"Game Settings panel did not flatten: {panel['unresolved'][0]}")
    panel = _transform_scene(
        panel,
        [1.0, 0.0, 0.0, 1.0, origin_x, origin_y],
        prefix="/settings/panel",
    )

    # Panel.TileFill attaches exact 128x128 Pulpboard tiles at runtime. The
    # APT placeholder itself is empty, so reproduce that attachment operation
    # using the exported retail texture and clipped edge UVs.
    pulpboard = SceneFlattener(cache, lambda *_: {}).flatten(PANEL_BUNDLE, 3)
    if (
        pulpboard["unresolved"]
        or len(pulpboard["primitives"]) != 1
        or pulpboard["text"]
    ):
        raise FormatError("retail Pulpboard tile did not flatten cleanly")
    source = pulpboard["primitives"][0]
    tiles = []
    tile_size = 128.0
    tile_index = 0
    y = 0.0
    while y < height:
        tile_height = min(tile_size, height - y)
        x = 0.0
        while x < width:
            tile_width = min(tile_size, width - x)
            item = copy.deepcopy(source)
            item["path"] = f"/settings/panel/pattern/tile{tile_index}"
            item["matrix"] = [
                1.0,
                0.0,
                0.0,
                1.0,
                origin_x + x,
                origin_y + y,
            ]
            left = origin_x + x
            top = origin_y + y
            right = left + tile_width
            bottom = top + tile_height
            u = tile_width / tile_size
            v = tile_height / tile_size
            item["triangles"] = [
                [
                    {"position": [left, bottom], "uv": [0.0, v]},
                    {"position": [left, top], "uv": [0.0, 0.0]},
                    {"position": [right, top], "uv": [u, 0.0]},
                ],
                [
                    {"position": [right, top], "uv": [u, 0.0]},
                    {"position": [right, bottom], "uv": [u, v]},
                    {"position": [left, bottom], "uv": [0.0, v]},
                ],
            ]
            tiles.append(item)
            tile_index += 1
            x += tile_size
        y += tile_size
    return panel, {
        "tile_character_id": 3,
        "tile_size": 128,
        "tiles": tiles,
        "build_panel": {
            "content_width": width - 2.0 * PANEL_MARGIN_LEFT,
            "content_height": (
                height
                - 2.0 * PANEL_MARGIN_TOP
                - PANEL_HEADER_HEIGHT
                - PANEL_BUTTON_HELP_OFFSET
                - PANEL_BOTTOM_MARGIN
            ),
            "panel_width": width,
            "panel_height": height,
            "center": [origin_x, origin_y],
        },
    }


def _flatten_option_row(
    cache: AssetCache,
    label: str,
    *,
    selected: bool,
    origin_x: float,
    origin_y: float,
    row_index: int,
    label_width: float,
) -> dict:
    row_prefix = f"/settings/row{row_index}"
    text_state = "selected" if selected else "unselected"

    def state_provider(path: str, bundle: dict, character: dict) -> dict:
        state = {}
        if path == "/mTitle":
            state["frame"] = playback_frame(
                character, text_state, play=True
            )
        elif path in (
            "/mTitle/mGlow",
            "/mTitle/mOuterGlow",
            "/mTitle/mInnerGlow",
        ):
            state["visible"] = selected
            if character.get("movie", {}).get("frame_count", 0) > 1:
                state["frame"] = 0
        elif path == "/mTitle/mIcons":
            state["frame"] = 0
        elif path.endswith("/TEXTLABEL"):
            state["text"] = label
            if "/mTitle/" in path:
                state["color_argb"] = (
                    "#ff70bbff" if selected else "#ff385570"
                )

        if selected and path.startswith("/mTitle/mGlow"):
            mapped_path = "/row/mText" + path[len("/mTitle") :]
            matrix = _glow_length_matrix(
                mapped_path, "/row", label_width
            )
            if matrix is not None:
                state["matrix"] = matrix
        if (
            "frame" not in state
            and character.get("movie", {}).get("frame_count", 0) > 1
        ):
            state["frame"] = 0
        return state

    row = SceneFlattener(
        cache, state_provider, include_zero_alpha=True
    ).flatten(OPTION_ROW_BUNDLE, 11)
    if row["unresolved"]:
        raise FormatError(
            f"Game Settings option row did not flatten: {row['unresolved'][0]}"
        )
    row = _transform_scene(
        row,
        [1.0, 0.0, 0.0, 1.0, origin_x, origin_y],
        prefix=row_prefix,
    )
    if selected:
        selected_text_path = row_prefix + "/mTitle"
        for item in row["primitives"] + row["text"]:
            path = item["path"]
            if path.startswith(selected_text_path + "/mGlow/mGlowBase/"):
                item["animation_track"] = SELECTED_GLOW_TRACK
            elif path.startswith(selected_text_path + "/mOuterGlow/"):
                item["animation_track"] = SELECTED_OUTER_TEXT_TRACK
            elif path.startswith(selected_text_path + "/mInnerGlow/"):
                item["animation_track"] = SELECTED_INNER_TEXT_TRACK
    return row


def _root_transform(options_root: dict, frame: int) -> tuple[list[float], float]:
    display = resolve_display_list(options_root, frame)
    if display["unresolved"] or len(display["objects"]) != 1:
        raise FormatError(
            f"Game Settings root frame {frame} did not resolve cleanly"
        )
    properties = display["objects"][0]["properties"]
    color = properties.get("color_transform_argb")
    if not color or len(color) != 8:
        raise FormatError(
            f"Game Settings root frame {frame} has no alpha transform"
        )
    return list(properties["matrix"]), color[0] / 255.0


def compile_game_settings(
    cache_root: Path,
    image_path: Path,
) -> dict:
    cache = AssetCache(cache_root)
    retail = extract_game_settings_menu(image_path)
    options_bundle = cache.load_bundle(OPTIONS_BUNDLE)
    options_root = options_bundle["apt"]["root"]
    if (
        options_root["movie"]["width"] != 1280
        or options_root["movie"]["height"] != 720
        or options_root["movie"]["frame_count"] != 30
    ):
        raise FormatError("Game Settings root timeline failed validation")

    language_path = (
        cache.root / cache.manifest["languages"]["english_global"]["metadata"]
    )
    language = json.loads(language_path.read_text(encoding="utf-8"))
    localized = _localized(language)
    title_source = localized.get(retail["title_id"])
    if not title_source:
        raise FormatError("Game Settings title localization is absent")

    text_control = cache.load_bundle(HIGHLIGHT_TEXT_BUNDLE)
    outer_text = text_control["characters"][3]
    glow_text = text_control["characters"][10]
    if (
        outer_text["bounds"] != glow_text["bounds"]
        or outer_text["text"]["font_height"]
        != glow_text["text"]["font_height"]
    ):
        raise FormatError("Game Settings glow font alias check failed")
    cache.add_font_alias(
        "FuturaOuterGlow",
        "Futura Glow",
        {
            "bundle": text_control["key"],
            "evidence": "paired retail glow declarations have identical metrics",
        },
    )
    title_text = text_control["characters"][7]
    title_font_character = text_control["characters"][
        title_text["text"]["font_id"]
    ]
    title_font = cache.font_asset(title_font_character["font"]["name"])
    if not title_font:
        raise FormatError("exact Game Settings row bitmap font is absent")

    rows = []
    for source in retail["options"]:
        localized_row = localized.get(source["label_id"])
        if not localized_row:
            raise FormatError(
                f"localization is absent for {source['label_id']}"
            )
        measurement = measure_bitmap_text(
            title_font["definition"],
            localized_row["value"],
            title_text["text"]["font_height"],
        )
        rows.append(
            {
                **source,
                "label": localized_row["value"],
                "language_index": localized_row["index"],
                "measurement": measurement,
                "state": "selected" if source["order"] == 0 else "unselected",
            }
        )

    measured_content_width = max(
        OPTION_TITLE_X + row["measurement"]["width"] for row in rows
    )
    content_width = max(MINIMUM_CONTENT_WIDTH, measured_content_width)
    content_height = max(
        MINIMUM_CONTENT_HEIGHT,
        OPTION_ORIGIN_Y + len(rows) * OPTION_HEIGHT,
    )
    panel_width = content_width + 2.0 * PANEL_MARGIN_LEFT
    panel_height = (
        content_height
        + 2.0 * PANEL_MARGIN_TOP
        + PANEL_HEADER_HEIGHT
        + PANEL_BUTTON_HELP_OFFSET
        + PANEL_BOTTOM_MARGIN
    )
    panel_x = 640.0 - panel_width / 2.0
    panel_y = 360.0 - panel_height / 2.0
    content_x = panel_x + PANEL_MARGIN_LEFT
    content_y = panel_y + PANEL_HEADER_HEIGHT + 8.0

    scene = {"primitives": [], "text": [], "unresolved": []}
    panel, panel_provenance = _flatten_panel(
        cache,
        title_source["value"],
        panel_width,
        panel_height,
        panel_x,
        panel_y,
    )
    draw_order = _append_scene(
        scene,
        {
            "primitives": panel["primitives"],
            "text": [],
            "unresolved": panel["unresolved"],
        },
        0,
    )
    for tile in panel_provenance.pop("tiles"):
        tile["draw_order"] = draw_order
        draw_order += 1
        scene["primitives"].append(tile)
    draw_order = _append_scene(
        scene,
        {"primitives": [], "text": panel["text"], "unresolved": []},
        draw_order,
    )
    for row in rows:
        row_scene = _flatten_option_row(
            cache,
            row["label"],
            selected=row["order"] == 0,
            origin_x=content_x + OPTION_ORIGIN_X,
            origin_y=content_y + OPTION_ORIGIN_Y + row["order"] * OPTION_HEIGHT,
            row_index=row["order"],
            label_width=row["measurement"]["width"],
        )
        draw_order = _append_scene(scene, row_scene, draw_order)
    if scene["unresolved"]:
        raise FormatError(
            f"Game Settings scene is unresolved: {scene['unresolved'][0]}"
        )

    # Reuse the exact synchronized HighlightText alpha tracks already compiled
    # for the crossbar renderer; the selected option is the same shipped
    # HighlightText component.
    career = compile_career_main(
        cache_root,
        image_path,
        selected_category=4,
        selected_option=0,
        copy_profile="retail",
    )
    selected_glow = career["animations"]["selected_glow"]

    settled_scene = {
        "primitives": scene["primitives"],
        "text": scene["text"],
    }
    open_snapshots = []
    for frame in range(1, 20):
        matrix, alpha = _root_transform(options_root, frame)
        open_snapshots.append(
            _transform_scene(settled_scene, matrix, alpha=alpha)
        )
    close_snapshots = []
    for frame in range(20, 30):
        matrix, alpha = _root_transform(options_root, frame)
        close_snapshots.append(
            _transform_scene(settled_scene, matrix, alpha=alpha)
        )
    milliseconds_per_frame = options_root["movie"]["milliseconds_per_frame"]
    motion_clips = {
        "open": _motion_clip(
            "open",
            settled_scene,
            open_snapshots,
            milliseconds_per_frame,
            {
                "bundle": options_bundle["key"],
                "character_id": options_root["id"],
                "label": "intro",
                "frames": list(range(1, 20)),
            },
        ),
        "close": _motion_clip(
            "close",
            settled_scene,
            close_snapshots,
            milliseconds_per_frame,
            {
                "bundle": options_bundle["key"],
                "character_id": options_root["id"],
                "label": "outro",
                "frames": list(range(20, 30)),
            },
        ),
    }

    for kind, plural in (("primitive", "primitives"), ("text", "text")):
        for item in settled_scene[plural]:
            item["item_key"] = _scene_item_key(kind, item)

    return {
        "format": "skate3-career-main-scene",
        "version": 3,
        "stage": {"width": 1280, "height": 720},
        "sources": {
            "asset_cache": str(cache.root),
            "options_apt": options_bundle["apt"]["apt_source"],
            "panel_apt": cache.load_bundle(PANEL_BUNDLE)["apt"]["apt_source"],
            "highlight_option_apt": cache.load_bundle(OPTION_ROW_BUNDLE)[
                "apt"
            ]["apt_source"],
            "retail_menu": retail,
            "language": str(language_path),
        },
        "runtime_state": {
            "screen": "GameSettings",
            "selected_option": 0,
            "interactive": False,
            "animation_policy": "source-authored options intro/outro and row glow",
        },
        "title": {
            "value": title_source["value"],
            "language_index": title_source["index"],
        },
        "options": rows,
        "layout": {
            "panel": panel_provenance,
            "content_origin": [content_x, content_y],
            "option_origin": [OPTION_ORIGIN_X, OPTION_ORIGIN_Y],
            "option_height": OPTION_HEIGHT,
            "constants_source": {
                "bundle": options_bundle["key"],
                "functions": ["CreateOptions", "Panel.BuildPanel"],
                "minimum_content_width": MINIMUM_CONTENT_WIDTH,
                "minimum_content_height": MINIMUM_CONTENT_HEIGHT,
                "panel_margin_left": PANEL_MARGIN_LEFT,
                "panel_margin_top": PANEL_MARGIN_TOP,
                "header_height": PANEL_HEADER_HEIGHT,
                "button_help_offset": PANEL_BUTTON_HELP_OFFSET,
            },
        },
        "animations": {
            "selected_glow": selected_glow,
            "motion": {
                "milliseconds_per_frame": milliseconds_per_frame,
                "clips": motion_clips,
            },
        },
        "scene": settled_scene,
        "validation": {
            "unresolved": [],
            "renderable": True,
            "counts": {
                "primitives": len(settled_scene["primitives"]),
                "text": len(settled_scene["text"]),
                "unresolved": 0,
            },
        },
    }
