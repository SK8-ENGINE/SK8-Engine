from __future__ import annotations

import json
from pathlib import Path

from .binary import FormatError
from .bitmap_font import measure_bitmap_text
from .retail_menu import extract_menu_page
from .scene_graph import AssetCache, SceneFlattener
from .timeline import playback_frame, playback_frames, resolve_display_list


CORE_BUNDLE = "data/fe/source/screens/main/core_menu"
DIMMER_BUNDLE = "data/fe/source/screens/main/dimmer"
PAGE_TITLE_ID = "ID_CROSSBAR_SINGLE_PLAYER_CHALLENGE_MODE_TITLE"
RETAIL_CAREER_BLUR_KERNEL = 8.0
RETAIL_CAREER_BLUR_COLOR = (90 / 255, 85 / 255, 81 / 255)
SELECTED_GLOW_TRACK = "selected_glow.background_base"
SELECTED_OUTER_TEXT_TRACK = "selected_glow.outer_text"
SELECTED_INNER_TEXT_TRACK = "selected_glow.inner_text"
CAREER_COLUMN_Y = 225.0
CAREER_BASE_SPACING = 44.0
CAREER_SELECT_MARGIN = 9.0
CUSTOM_MODE_TITLE = "Custom"
CUSTOM_MENU_COPY = (
    {
        "label": "Custom Maps",
        "helper": "Explore community-built spots and worlds",
    },
    {
        "label": "Custom Models",
        "helper": "Swap skaters, boards, props, and gear",
    },
    {
        "label": "Mod Library",
        "helper": "Browse and manage installed gameplay mods",
    },
    {
        "label": "Session Lab",
        "helper": "Create custom challenges and rule sets",
    },
    {
        "label": "Replay Studio",
        "helper": "Capture and edit cinematic skate clips",
    },
    {
        "label": "Creator Hub",
        "helper": "Build, test, and share custom content",
    },
    {
        "label": "Online Hub",
        "helper": "Join community sessions and featured events",
    },
)


def _named(display: dict) -> dict[str, dict]:
    return {
        item["properties"]["name"]: item
        for item in display["objects"]
        if item["properties"].get("name")
    }


def _localized(language: dict) -> dict[str, dict]:
    result = {}
    for item in language["entries"]:
        if item["label"] and item["label"] not in result:
            result[item["label"]] = item
    return result


def _position_column_rows(
    source_matrices: list[list[float]], selected_option: int
) -> tuple[float, dict[int, list[float]]]:
    """Reproduce core_menu PositionColumn() from its ActionScript bytecode.

    The function body at APT offsets 0xCBE4-0xCCB4 places the column at
    COLUMN_Y + SELECT_MARGIN only for option zero. Its row loop then adds
    SELECT_MARGIN when i is either the selected option or the option directly
    below it. This creates extra clearance on both sides of the enlarged row.
    """
    if not source_matrices:
        raise FormatError("PositionColumn requires at least one row matrix")

    column_y = CAREER_COLUMN_Y
    if selected_option == 0:
        column_y += CAREER_SELECT_MARGIN

    row_matrices = {}
    previous_y = None
    for index, source_matrix in enumerate(source_matrices):
        matrix = list(source_matrix)
        if index == 0:
            y = matrix[5]
        else:
            y = previous_y + CAREER_BASE_SPACING
            if index in (selected_option, selected_option + 1):
                y += CAREER_SELECT_MARGIN
        matrix[5] = y
        row_matrices[index] = matrix
        previous_y = y
    return column_y, row_matrices


def _alpha_track(character: dict, depth: int) -> list[int]:
    frame_count = character.get("movie", {}).get("frame_count", 0)
    if frame_count <= 0:
        raise FormatError(f"character {character.get('id')} has no animation frames")
    values = []
    for frame in range(frame_count):
        display = resolve_display_list(character, frame)
        if display["unresolved"]:
            raise FormatError(
                f"character {character['id']} frame {frame} did not resolve cleanly"
            )
        matches = [item for item in display["objects"] if item["depth"] == depth]
        if len(matches) != 1:
            raise FormatError(
                f"character {character['id']} frame {frame} expected one "
                f"display object at depth {depth}, found {len(matches)}"
            )
        color = matches[0]["properties"].get(
            "color_transform_argb", [255, 255, 255, 255, 0, 0, 0, 0]
        )
        if len(color) != 8:
            raise FormatError(
                f"character {character['id']} frame {frame} has a malformed "
                "color transform"
            )
        values.append(color[0])
    return values


def _timeline_frame(frames: list[int], tick: int) -> int:
    return frames[min(tick, len(frames) - 1)]


def _scene_item_key(kind: str, item: dict) -> str:
    suffix = item["unit"] if kind == "primitive" else 0
    return (
        f"{kind}:{item['path']}:{item['bundle']}:"
        f"{item['character_id']}:{suffix}"
    )


def _scene_item_opacity(kind: str, item: dict) -> float:
    if kind == "primitive":
        return float(item["color"][3])
    argb = item["color_argb"]
    return float(item["alpha"]) * (int(argb[1:3], 16) / 255.0)


def _index_scene(scene: dict) -> dict[str, tuple[str, dict]]:
    result = {}
    for kind, plural in (("primitive", "primitives"), ("text", "text")):
        for item in scene[plural]:
            key = _scene_item_key(kind, item)
            if key in result:
                raise FormatError(f"duplicate animated scene item key {key!r}")
            result[key] = (kind, item)
    return result


def _motion_clip(
    name: str,
    base_scene: dict,
    snapshots: list[dict],
    milliseconds_per_frame: int,
    source: dict,
) -> dict:
    if not snapshots:
        raise FormatError(f"animation clip {name!r} has no frames")
    base = _index_scene(base_scene)
    indexed_frames = [_index_scene(scene) for scene in snapshots]
    tracks = []
    for key, (kind, base_item) in base.items():
        matrices = []
        opacities = []
        for frame_index, frame in enumerate(indexed_frames):
            current = frame.get(key)
            if current is None:
                raise FormatError(
                    f"animation clip {name!r} frame {frame_index} is "
                    f"missing scene item {key!r}"
                )
            current_kind, current_item = current
            if current_kind != kind:
                raise FormatError(
                    f"animation clip {name!r} changes the kind of {key!r}"
                )
            matrices.append([float(value) for value in current_item["matrix"]])
            opacities.append(_scene_item_opacity(kind, current_item))

        base_matrix = [float(value) for value in base_item["matrix"]]
        base_opacity = _scene_item_opacity(kind, base_item)
        matrix_changes = any(
            any(abs(left - right) > 1e-6 for left, right in zip(matrix, base_matrix))
            for matrix in matrices
        )
        opacity_changes = any(
            abs(opacity - base_opacity) > 1e-6 for opacity in opacities
        )
        if not matrix_changes and not opacity_changes:
            continue
        track = {"item": key}
        if matrix_changes:
            track["matrix"] = matrices
        if opacity_changes:
            track["opacity"] = opacities
        tracks.append(track)

    extras = set().union(*(set(frame) for frame in indexed_frames)) - set(base)
    if extras:
        raise FormatError(
            f"animation clip {name!r} introduces items absent from its "
            f"settled scene: {sorted(extras)[:3]}"
        )
    return {
        "milliseconds_per_frame": milliseconds_per_frame,
        "frame_count": len(snapshots),
        "loop": False,
        "tracks": tracks,
        "source": source,
    }


def _glow_length_matrix(path: str, row_path: str, length: float) -> list[float] | None:
    glow = row_path + "/mText/mGlow"
    values = {
        glow + "/mOuterGlow": [1.0, 0.0, 0.0, 1.0, length / 2.0, 0.0],
        glow + "/mOuterGlow/mLeft": [
            1.0000152587890625,
            0.0,
            0.0,
            1.0,
            -length / 2.0,
            0.0,
        ],
        glow + "/mOuterGlow/mFill": [
            (length - 24.0) / 16.0,
            0.0,
            0.0,
            1.0,
            -length / 2.0 + 12.0,
            0.0,
        ],
        glow + "/mOuterGlow/mRight": [
            -1.0,
            0.0,
            0.0,
            1.0,
            length / 2.0,
            0.0,
        ],
        glow + "/mGlowBase/mBase": [
            1.0,
            0.0,
            0.0,
            1.0,
            length / 2.0,
            0.0,
        ],
        glow + "/mGlowBase/mBase/mLeft": [
            1.0,
            0.0,
            0.0,
            1.0,
            -length / 2.0,
            0.0,
        ],
        glow + "/mGlowBase/mBase/mFill": [
            (length - 16.0) / 16.0,
            0.0,
            0.0,
            1.0,
            -length / 2.0 + 8.0,
            0.0,
        ],
        glow + "/mGlowBase/mBase/mRight": [
            -1.0,
            0.0,
            0.0,
            1.0,
            length / 2.0,
            0.0,
        ],
        glow + "/mGlowBase/mEdgeRight": [
            -1.0,
            0.0,
            0.0,
            0.9999847412109375,
            length,
            0.0,
        ],
    }
    return values.get(path)


def _flatten_dimmer(cache: AssetCache) -> tuple[dict, dict]:
    dimmer = cache.load_bundle(DIMMER_BUNDLE)
    root = dimmer["apt"]["root"]
    resting_frame = playback_frame(root, "intro", play=True)
    display = resolve_display_list(root, resting_frame)
    if display["unresolved"]:
        raise FormatError("dimmer intro timeline did not resolve cleanly")
    if len(display["objects"]) != 1:
        raise FormatError(
            f"dimmer resting frame expected one display object, "
            f"found {len(display['objects'])}"
        )
    properties = display["objects"][0]["properties"]
    color_transform = properties.get("color_transform_argb")
    if not color_transform or len(color_transform) != 8:
        raise FormatError("dimmer resting frame has no authored color transform")

    def state_provider(path: str, bundle: dict, character: dict) -> dict:
        if bundle["key"] == DIMMER_BUNDLE and character["id"] == 0:
            return {"frame": resting_frame}
        return {}

    asset_scene = SceneFlattener(cache, state_provider).flatten(DIMMER_BUNDLE, 0)
    if asset_scene["unresolved"]:
        raise FormatError("dimmer scene did not flatten cleanly")
    if not asset_scene["primitives"] or asset_scene["text"]:
        raise FormatError("dimmer scene has unexpected authored contents")
    scene = {
        "primitives": [],
        "text": [],
        "unresolved": [],
    }
    return scene, {
        "bundle": DIMMER_BUNDLE,
        "apt": dimmer["apt"]["apt_source"],
        "frame_label": "intro",
        "resting_frame": resting_frame,
        "milliseconds_per_frame": root["movie"]["milliseconds_per_frame"],
        "color_transform_argb": color_transform,
        "primitive_count": len(scene["primitives"]),
        "asset_primitive_count": len(asset_scene["primitives"]),
        "career_runtime_usage": {
            "rendered": False,
            "reason": (
                "the settled Career backdrop is produced by the retail "
                "two-pass blur modulation; the probed black ScreenManager "
                "quad belongs to a different frontend state"
            ),
        },
        "career_backdrop": {
            "source": "native settled blur source probe",
            "kernel": RETAIL_CAREER_BLUR_KERNEL,
            "per_pass_color": list(RETAIL_CAREER_BLUR_COLOR),
            "per_pass_color_u8": [90, 85, 81],
        },
    }


def compile_career_main(
    cache_root: Path,
    image_path: Path,
    *,
    selected_category: int = 0,
    selected_option: int = 0,
    party_play_state: str = "disabled",
    progression_state: str = "welcome",
    dlc_visible: bool = False,
    copy_profile: str = "custom",
) -> dict:
    if party_play_state not in ("enabled", "disabled", "unresolved"):
        raise ValueError("party_play_state must be enabled, disabled, or unresolved")
    if copy_profile not in ("custom", "retail"):
        raise ValueError("copy_profile must be custom or retail")
    cache = AssetCache(cache_root)
    page = extract_menu_page(image_path, PAGE_TITLE_ID)
    if not 0 <= selected_category < len(page["categories"]):
        raise ValueError(
            f"selected_category must be between 0 and {len(page['categories']) - 1}"
        )
    selected_category_record = page["categories"][selected_category]
    page["options"] = [
        {
            **page["all_options"][option_index],
            "order": order,
        }
        for order, option_index in enumerate(
            selected_category_record["option_indices"]
        )
    ]
    use_custom_copy = copy_profile == "custom" and selected_category == 0
    if use_custom_copy and len(page["options"]) != len(CUSTOM_MENU_COPY):
        raise FormatError(
            "custom menu copy does not match the extracted Career option count"
        )
    if not 0 <= selected_option < len(page["options"]):
        raise ValueError(
            f"selected_option must be between 0 and {len(page['options']) - 1}"
        )
    if (
        page["options"][selected_option]["internal_name"] == "PartyPlay"
        and party_play_state != "enabled"
    ):
        raise ValueError("selected_option cannot select disabled PartyPlay")
    core = cache.load_bundle(CORE_BUNDLE)
    root = core["apt"]["root"]
    active_frame_overrides: dict[str, int] = {}
    root_frame = playback_frame(root, "animatein", play=True)
    root_display = resolve_display_list(root, root_frame)
    if root_display["unresolved"]:
        raise FormatError("core_menu root timeline did not resolve cleanly")
    root_named = _named(root_display)

    characters = core["characters"]
    title_character = characters[root_named["mTitle"]["properties"]["character_id"]]
    title_frame = 0  # bounce returns to frame zero, whose action is stop()
    title_display = resolve_display_list(title_character, title_frame)
    title_named = _named(title_display)

    column_anim = characters[
        root_named["mColumnAnim"]["properties"]["character_id"]
    ]
    column_display = resolve_display_list(column_anim, 0)
    column_named = _named(column_display)
    column_character = characters[
        column_named["mColumn"]["properties"]["character_id"]
    ]
    rows_display = resolve_display_list(column_character, 0)
    rows_named = _named(rows_display)

    language_path = (
        cache.root / cache.manifest["languages"]["english_global"]["metadata"]
    )
    language = json.loads(language_path.read_text(encoding="utf-8"))
    localized = _localized(language)

    def text_for(label_id: str) -> tuple[str | None, dict | None]:
        item = localized.get(label_id)
        return (item["value"], item) if item else (None, None)

    mode_title, mode_source = text_for("ID_CROSSBAR_SINGLE_PLAYER_MODE_TITLE")
    column_title, column_source = text_for(
        selected_category_record["label_id"]
    )
    if mode_title is None or column_title is None:
        raise FormatError("Career/Main localization entries are absent")
    retail_mode_title = mode_title
    if copy_profile == "custom":
        mode_title = CUSTOM_MODE_TITLE

    mode_text_character = characters[46]
    mode_font_character = characters[mode_text_character["text"]["font_id"]]
    mode_font = cache.font_asset(mode_font_character["font"]["name"])
    if not mode_font:
        raise FormatError("exact title bitmap font is absent")
    mode_measure = measure_bitmap_text(
        mode_font["definition"],
        mode_title,
        mode_text_character["text"]["font_height"],
    )
    title_column_matrix = list(title_named["mColumn"]["properties"]["matrix"])
    title_column_matrix[4] = mode_measure["width"] + 23.0
    title_arrow_matrix = list(title_named["mArrow"]["properties"]["matrix"])
    title_arrow_matrix[4] = mode_measure["width"] + 3.0

    text_control = cache.load_bundle("source/controls/menu_part_text_hilite")
    outer_text = text_control["characters"][3]
    glow_text = text_control["characters"][10]
    if (
        outer_text["bounds"] != glow_text["bounds"]
        or outer_text["text"]["font_height"] != glow_text["text"]["font_height"]
        or text_control["characters"][2]["font"]["name"] != "FuturaOuterGlow"
        or text_control["characters"][9]["font"]["name"] != "Futura Glow"
    ):
        raise FormatError("FuturaOuterGlow/Futura Glow metric alias check failed")
    cache.add_font_alias(
        "FuturaOuterGlow",
        "Futura Glow",
        {
            "bundle": text_control["key"],
            "outer_font_character_id": 2,
            "glow_font_character_id": 9,
            "outer_text_character_id": 3,
            "glow_text_character_id": 10,
            "evidence": (
                "paired outer/inner glow declarations have identical bounds "
                "and font height; Futura Glow is the packaged bitmap family "
                "used by the paired glow layer"
            ),
        },
    )
    title_text = text_control["characters"][7]
    title_font_character = text_control["characters"][title_text["text"]["font_id"]]
    title_font = cache.font_asset(title_font_character["font"]["name"])
    if not title_font:
        raise FormatError("exact selected-row title bitmap font is absent")

    glow_bundle, glow_character = cache.resolve_character(text_control["key"], 1)
    outer_glow_character = text_control["characters"][5]
    inner_glow_character = text_control["characters"][12]
    glow_frame_count = glow_character["movie"]["frame_count"]
    if (
        glow_frame_count != 300
        or outer_glow_character["movie"]["frame_count"] != glow_frame_count
        or inner_glow_character["movie"]["frame_count"] != glow_frame_count
    ):
        raise FormatError("selected-row glow timelines are not synchronized")
    milliseconds_per_frame = text_control["apt"]["root"]["movie"][
        "milliseconds_per_frame"
    ]
    if (
        milliseconds_per_frame
        != glow_bundle["apt"]["root"]["movie"]["milliseconds_per_frame"]
    ):
        raise FormatError("selected-row glow timelines use conflicting frame rates")
    text_init_control = next(
        control
        for control in text_control["apt"]["root"]["frames"][0]["controls"]
        if control["type_name"] == "do_init_action"
        and control.get("sprite_id") == 46
    )
    glow_init_control = next(
        control
        for control in glow_bundle["apt"]["root"]["frames"][0]["controls"]
        if control["type_name"] == "do_init_action"
        and control.get("sprite_id") == 21
    )
    background_alpha = _alpha_track(glow_character, 11)
    outer_text_alpha = _alpha_track(outer_glow_character, 1)
    inner_text_alpha = _alpha_track(inner_glow_character, 1)
    selected_glow_animation = {
        "milliseconds_per_frame": milliseconds_per_frame,
        "frame_count": glow_frame_count,
        "loop": True,
        "tracks": {
            SELECTED_GLOW_TRACK: {
                "baseline_alpha_u8": background_alpha[0],
                "alpha_u8": background_alpha,
                "source": {
                    "bundle": glow_bundle["key"],
                    "character_id": glow_character["id"],
                    "depth": 11,
                },
            },
            SELECTED_OUTER_TEXT_TRACK: {
                "baseline_alpha_u8": outer_text_alpha[0],
                "alpha_u8": outer_text_alpha,
                "source": {
                    "bundle": text_control["key"],
                    "character_id": outer_glow_character["id"],
                    "depth": 1,
                },
            },
            SELECTED_INNER_TEXT_TRACK: {
                "baseline_alpha_u8": inner_text_alpha[0],
                "alpha_u8": inner_text_alpha,
                "source": {
                    "bundle": text_control["key"],
                    "character_id": inner_glow_character["id"],
                    "depth": 1,
                },
            },
        },
        "set_length": {
            "source": {
                "text_bundle": text_control["key"],
                "text_apt": text_control["apt"]["apt_source"],
                "text_init_action_offset": text_init_control["actions_offset"],
                "glow_bundle": glow_bundle["key"],
                "glow_apt": glow_bundle["apt"]["apt_source"],
                "glow_init_action_offset": glow_init_control["actions_offset"],
            },
            "call": "mGlow.setLength(mTitle.TEXTLABEL._width)",
            "branch": "len > 32",
            "outer_fill_endcaps": 24.0,
            "base_fill_endcaps": 16.0,
            "edge_alpha_percent": "min(100, (len - 32) * 100 / 32)",
        },
    }

    category_records = page["categories"]
    row_records = []
    unresolved = []
    for option in page["options"]:
        label_id = option["label_id"]
        label = None
        label_source = None
        label_binding = "descriptor_label_id"
        if label_id.startswith("ID_"):
            label, label_source = text_for(label_id)
        elif option["internal_name"] == "PartyPlay":
            # This TU descriptor retains a development literal. Bind it through
            # the one canonical language ID formed by the exact internal name;
            # the ID and shipped value must both be present and unique.
            canonical_id = "ID_CROSSBAR_PARTY_PLAY_MODE_TITLE"
            label, label_source = text_for(canonical_id)
            if not label_source or label != "Party Play":
                raise FormatError("canonical PartyPlay localization is absent")
            label_binding = "canonical_internal_name_language_id"
        if label is None:
            unresolved.append(
                {
                    "field": f"options[{option['order']}].label",
                    "reason": f"no localized value for {label_id!r}",
                }
            )
            label = ""

        helper, helper_source = text_for(option["helper_id"])
        if helper is None:
            helper = ""
        retail_label = label
        retail_helper = helper
        if use_custom_copy:
            custom_copy = CUSTOM_MENU_COPY[option["order"]]
            label = custom_copy["label"]
            helper = custom_copy["helper"]
            label_binding = "custom_menu_copy"

        if option["order"] == selected_option:
            state = "selected"
        elif option["internal_name"] == "PartyPlay":
            state = party_play_state
            if state == "unresolved":
                unresolved.append(
                    {
                        "field": "options[6].enabled",
                        "reason": "native IsItemEnabled result has not been supplied",
                    }
                )
        else:
            state = "unselected"
        row_records.append(
            {
                **option,
                "label": label,
                "label_binding": label_binding,
                "label_language_index": (
                    label_source["index"]
                    if label_source and not use_custom_copy
                    else None
                ),
                "helper": helper,
                "helper_language_index": (
                    helper_source["index"]
                    if helper_source and not use_custom_copy
                    else None
                ),
                "retail_copy": {
                    "label": retail_label,
                    "label_language_index": (
                        label_source["index"] if label_source else None
                    ),
                    "helper": retail_helper,
                    "helper_language_index": (
                        helper_source["index"] if helper_source else None
                    ),
                },
                "state": state,
                "title_measurement": measure_bitmap_text(
                    title_font["definition"],
                    label,
                    title_text["text"]["font_height"],
                ),
            }
        )

    row_states = {item["order"]: item["state"] for item in row_records}
    row_icons = {item["order"]: item["icon_frame"] for item in row_records}
    row_labels = {item["order"]: item["label"] for item in row_records}
    row_helpers = {item["order"]: item["helper"] for item in row_records}
    row_glow_lengths = {
        item["order"]: item["title_measurement"]["width"] for item in row_records
    }
    category_icons = {
        item["slot"]: item["icon_frame"] for item in category_records
    }

    column_y, row_matrices = _position_column_rows(
        [
            rows_named[f"mSubcat{index}"]["properties"]["matrix"]
            for index in range(8)
        ],
        selected_option,
    )
    column_matrix = list(column_named["mColumn"]["properties"]["matrix"])
    selected_category_path = f"cat{selected_category}"
    column_matrix[4] = root_named[selected_category_path]["properties"]["matrix"][4]
    column_matrix[5] = column_y

    core_key = cache.normalize_bundle(CORE_BUNDLE)
    category_prefix = "/cat"
    row_prefix = "/mColumnAnim/mColumn/mSubcat"

    def row_index(path: str) -> int | None:
        if not path.startswith(row_prefix):
            return None
        tail = path[len(row_prefix) :]
        digits = ""
        for character in tail:
            if not character.isdigit():
                break
            digits += character
        return int(digits) if digits else None

    def state_provider(path: str, bundle: dict, character: dict) -> dict:
        state: dict = {}
        if path == "/" and bundle["key"] == core_key:
            state["frame"] = root_frame
        elif path == "/mProgressionPanel":
            state["frame"] = playback_frame(
                character, progression_state, play=True
            )
        elif path == "/mDLC":
            state["visible"] = dlc_visible
        elif path == "/mTitle":
            state["frame"] = title_frame
        elif path == "/mTitle/mColumn":
            state["matrix"] = title_column_matrix
            animated_title_frame = active_frame_overrides.get("/mTitle")
            if animated_title_frame is not None:
                animated_title = _named(
                    resolve_display_list(title_character, animated_title_frame)
                )["mColumn"]["properties"]["matrix"]
                authored_title = title_named["mColumn"]["properties"]["matrix"]
                state["matrix"] = list(state["matrix"])
                state["matrix"][4] += animated_title[4] - authored_title[4]
                state["matrix"][5] += animated_title[5] - authored_title[5]
        elif path == "/mTitle/mArrow":
            state["matrix"] = title_arrow_matrix
        elif path == "/mTitle/mMode/textLabel":
            state["text"] = mode_title
        elif path == "/mTitle/mColumn/textLabel":
            state["text"] = column_title
        elif path.startswith(category_prefix):
            segment = path.split("/", 2)[1]
            if segment.startswith("cat") and segment[3:].isdigit():
                category_index = int(segment[3:])
                if category_index >= len(category_records):
                    state["visible"] = False
                elif path == f"/cat{category_index}":
                    label = (
                        "selected"
                        if category_index == selected_category
                        else "stopunselected"
                    )
                    state["frame"] = playback_frame(
                        character,
                        label,
                        play=category_index == selected_category,
                    )
                elif path.endswith(("/mIconSelected", "/mIconUnselected")):
                    state["frame"] = playback_frame(
                        character, category_icons[category_index], play=True
                    )
        elif path == "/mColumnAnim/mColumn":
            state["matrix"] = column_matrix
        elif path == "/mColumnAnim/mSubColumn":
            state["visible"] = False
        elif path.startswith(row_prefix):
            index = row_index(path)
            if index is None:
                return state
            if index >= len(row_records):
                state["visible"] = False
                return state
            row_state = row_states[index]
            row_path = f"{row_prefix}{index}"
            if row_state == "selected":
                matrix = _glow_length_matrix(
                    path, row_path, row_glow_lengths[index]
                )
                if matrix is not None:
                    state["matrix"] = matrix
            if path == row_path:
                state["matrix"] = row_matrices[index]
                if row_state == "selected":
                    state["frame"] = playback_frame(character, "selected", play=True)
                elif row_state == "unselected":
                    state["frame"] = playback_frame(
                        character, "stopunselected", play=False
                    )
                elif row_state == "disabled":
                    state["frame"] = playback_frame(character, "disabled", play=True)
            elif path == row_path + "/mIcon":
                state["frame"] = playback_frame(
                    character, row_icons[index], play=True
                )
            elif path == row_path + "/mText":
                label = "unselected" if row_state == "unresolved" else row_state
                state["frame"] = playback_frame(character, label, play=True)
            elif path in (
                row_path + "/mText/mGlow",
                row_path + "/mText/mOuterGlow",
                row_path + "/mText/mInnerGlow",
            ):
                state["visible"] = row_state == "selected"
                if character.get("movie", {}).get("frame_count", 0) > 1:
                    state["frame"] = 0
            elif path == row_path + "/mText/mIcons":
                state["frame"] = playback_frame(character, "none", play=True)
            elif path == row_path + "/mText/mIcons/mCoop":
                state["frame"] = playback_frame(character, "none", play=True)
            elif path.endswith("/TEXTLABEL"):
                state["text"] = row_labels[index]
                if "/mTitle/" in path:
                    colors = {
                        "selected": "#ff70bbff",
                        "unselected": "#ff385570",
                        "disabled": "#ff555555",
                        "unresolved": "#ff385570",
                    }
                    state["color_argb"] = colors[row_state]
            elif path == row_path + "/mHelptext/labelText":
                state["text"] = row_helpers[index]

        if path in active_frame_overrides:
            state["frame"] = active_frame_overrides[path]
        if (
            "frame" not in state
            and character.get("movie", {}).get("frame_count", 0) > 1
            and not any(
                control["type_name"] == "frame_label"
                for frame in character.get("frames", [])
                for control in frame["controls"]
            )
        ):
            # Animation playback is deliberately deferred. Free-running clips
            # are frozen at their exact first frame.
            state["frame"] = 0
        return state

    dimmer_scene, dimmer_provenance = _flatten_dimmer(cache)
    flattener = SceneFlattener(
        cache, state_provider, include_zero_alpha=True
    )
    menu_scene = flattener.flatten(CORE_BUNDLE, 0)
    unresolved.extend(menu_scene["unresolved"])

    def flatten_animation_frame(overrides: dict[str, int]) -> dict:
        active_frame_overrides.clear()
        active_frame_overrides.update(overrides)
        frame_flattener = SceneFlattener(
            cache, state_provider, include_zero_alpha=True
        )
        frame_scene = frame_flattener.flatten(CORE_BUNDLE, 0)
        active_frame_overrides.clear()
        if frame_scene["unresolved"]:
            raise FormatError(
                "Career animation frame did not resolve cleanly: "
                f"{frame_scene['unresolved'][0]}"
            )
        return frame_scene

    core_frame_duration = root["movie"]["milliseconds_per_frame"]
    if core_frame_duration != milliseconds_per_frame:
        raise FormatError("Career animation bundles use conflicting frame rates")

    category_character = characters[
        root_named["cat0"]["properties"]["character_id"]
    ]
    category_display = _named(resolve_display_list(category_character, 0))
    category_selected_icon = characters[
        category_display["mIconSelected"]["properties"]["character_id"]
    ]
    category_unselected_icon = characters[
        category_display["mIconUnselected"]["properties"]["character_id"]
    ]
    row_character = characters[
        rows_named["mSubcat0"]["properties"]["character_id"]
    ]
    row_display = _named(resolve_display_list(row_character, 0))
    row_icon_character = characters[
        row_display["mIcon"]["properties"]["character_id"]
    ]
    _, row_text_character = cache.resolve_character(
        core["key"], row_display["mText"]["properties"]["character_id"]
    )

    root_open_frames = playback_frames(root, "animatein")
    title_bounce_frames = playback_frames(title_character, "bounce")
    category_selected_frames = playback_frames(category_character, "selected")
    row_selected_frames = playback_frames(row_character, "selected")
    row_unselected_frames = playback_frames(row_character, "unselected")
    row_disabled_frames = playback_frames(row_character, "disabled")
    text_selected_frames = playback_frames(row_text_character, "selected")
    text_unselected_frames = playback_frames(row_text_character, "unselected")
    text_disabled_frames = playback_frames(row_text_character, "disabled")

    open_snapshots = []
    for tick, root_animation_frame in enumerate(root_open_frames):
        overrides = {
            "/": root_animation_frame,
            "/mTitle": _timeline_frame(title_bounce_frames, tick),
            f"/cat{selected_category}": _timeline_frame(
                category_selected_frames, tick
            ),
        }
        for category_index in range(len(category_records)):
            icon_frames_selected = playback_frames(
                category_selected_icon, category_icons[category_index]
            )
            icon_frames_unselected = playback_frames(
                category_unselected_icon, category_icons[category_index]
            )
            category_path = f"/cat{category_index}"
            overrides[category_path + "/mIconSelected"] = _timeline_frame(
                icon_frames_selected, tick
            )
            overrides[category_path + "/mIconUnselected"] = _timeline_frame(
                icon_frames_unselected, tick
            )
        for row_index_value, row_state in row_states.items():
            row_path = f"{row_prefix}{row_index_value}"
            if row_state == "selected":
                overrides[row_path] = _timeline_frame(row_selected_frames, tick)
                overrides[row_path + "/mText"] = _timeline_frame(
                    text_selected_frames, tick
                )
            elif row_state == "disabled":
                overrides[row_path] = _timeline_frame(row_disabled_frames, tick)
                overrides[row_path + "/mText"] = _timeline_frame(
                    text_disabled_frames, tick
                )
            else:
                overrides[row_path + "/mText"] = _timeline_frame(
                    text_unselected_frames, tick
                )
            icon_frames = playback_frames(
                row_icon_character, row_icons[row_index_value]
            )
            overrides[row_path + "/mIcon"] = _timeline_frame(icon_frames, tick)
        open_snapshots.append(flatten_animation_frame(overrides))

    close_frames = playback_frames(root, "outro")
    close_snapshots = [
        flatten_animation_frame({"/": frame}) for frame in close_frames
    ]
    motion_clips = {
        "open": _motion_clip(
            "open",
            menu_scene,
            open_snapshots,
            core_frame_duration,
            {
                "bundle": core["key"],
                "character_id": root["id"],
                "label": "animatein",
                "frames": root_open_frames,
                "actionscript": 'mScreen.gotoAndPlay("animatein")',
            },
        ),
        "close": _motion_clip(
            "close",
            menu_scene,
            close_snapshots,
            core_frame_duration,
            {
                "bundle": core["key"],
                "character_id": root["id"],
                "label": "outro",
                "frames": close_frames,
                "completion": "ScreenManager.OutroComplete(screen.Id)",
            },
        ),
    }

    category_unselected_frames = playback_frames(
        category_character, "unselected"
    )
    for source_category in (
        selected_category - 1,
        selected_category + 1,
    ):
        if source_category < 0 or source_category >= len(category_records):
            continue
        category_snapshots = []
        category_frame_count = max(
            len(category_selected_frames),
            len(category_unselected_frames),
            len(title_bounce_frames),
        )
        selected_category_item = f"/cat{selected_category}"
        source_category_item = f"/cat{source_category}"
        for tick in range(category_frame_count):
            category_snapshots.append(
                flatten_animation_frame(
                    {
                        selected_category_item: _timeline_frame(
                            category_selected_frames, tick
                        ),
                        source_category_item: _timeline_frame(
                            category_unselected_frames, tick
                        ),
                        "/mTitle": _timeline_frame(title_bounce_frames, tick),
                    }
                )
            )
        clip_name = f"from_category_{source_category}"
        motion_clips[clip_name] = _motion_clip(
            clip_name,
            menu_scene,
            category_snapshots,
            core_frame_duration,
            {
                "bundle": core["key"],
                "category_character_id": category_character["id"],
                "old_category": source_category,
                "new_category": selected_category,
                "commands": [
                    'old.gotoAndPlay("unselected")',
                    'new.gotoAndPlay("selected")',
                    'mTitle.gotoAndPlay("bounce")',
                    "PositionColumn()",
                ],
            },
        )

    for source_option in (selected_option - 1, selected_option + 1):
        if (
            source_option < 0
            or source_option >= len(row_records)
            or row_records[source_option]["state"] == "disabled"
        ):
            continue
        navigation_snapshots = []
        navigation_frame_count = max(
            len(row_selected_frames),
            len(row_unselected_frames),
            len(text_selected_frames),
            len(text_unselected_frames),
        )
        selected_path = f"{row_prefix}{selected_option}"
        source_path = f"{row_prefix}{source_option}"
        for tick in range(navigation_frame_count):
            navigation_snapshots.append(
                flatten_animation_frame(
                    {
                        selected_path: _timeline_frame(row_selected_frames, tick),
                        selected_path + "/mText": _timeline_frame(
                            text_selected_frames, tick
                        ),
                        source_path: _timeline_frame(row_unselected_frames, tick),
                        source_path + "/mText": _timeline_frame(
                            text_unselected_frames, tick
                        ),
                    }
                )
            )
        clip_name = f"from_{source_option}"
        motion_clips[clip_name] = _motion_clip(
            clip_name,
            menu_scene,
            navigation_snapshots,
            core_frame_duration,
            {
                "bundle": core["key"],
                "row_character_id": row_character["id"],
                "text_bundle": text_control["key"],
                "text_character_id": row_text_character["id"],
                "old_option": source_option,
                "new_option": selected_option,
                "commands": [
                    'old.gotoAndPlay("unselected")',
                    'old.mText.SetState("unselected")',
                    'new.gotoAndPlay("selected")',
                    'new.mText.SetState("selected")',
                    "PositionColumn()",
                ],
            },
        )

    # Keep the dimmer asset inspection in the provenance, but do not paint it
    # in Career. The settled live capture proves this page's dark warmth comes
    # from the fixed-resolution H/V blur and its per-pass c1 modulation.
    menu_draw_offset = len(dimmer_scene["primitives"]) + len(dimmer_scene["text"])
    for item in menu_scene["primitives"] + menu_scene["text"]:
        item["draw_order"] += menu_draw_offset
    selected_row_path = f"{row_prefix}{selected_option}/mText"
    for item in menu_scene["primitives"] + menu_scene["text"]:
        path = item["path"]
        if path.startswith(selected_row_path + "/mGlow/mGlowBase/"):
            item["animation_track"] = SELECTED_GLOW_TRACK
        elif path.startswith(selected_row_path + "/mOuterGlow/"):
            item["animation_track"] = SELECTED_OUTER_TEXT_TRACK
        elif path.startswith(selected_row_path + "/mInnerGlow/"):
            item["animation_track"] = SELECTED_INNER_TEXT_TRACK
    scene = {
        "primitives": dimmer_scene["primitives"] + menu_scene["primitives"],
        "text": dimmer_scene["text"] + menu_scene["text"],
    }
    for kind, plural in (("primitive", "primitives"), ("text", "text")):
        for item in scene[plural]:
            item["item_key"] = _scene_item_key(kind, item)

    init_control = next(
        control
        for control in root["frames"][0]["controls"]
        if control["type_name"] == "do_init_action"
        and control.get("sprite_id") == 168
    )
    return {
        "format": "skate3-career-main-scene",
        "version": 3,
        "stage": {
            "width": root["movie"]["width"],
            "height": root["movie"]["height"],
        },
        "sources": {
            "asset_cache": str(cache.root),
            "core_apt": core["apt"]["apt_source"],
            "core_action_offset": init_control["actions_offset"],
            "dimmer": dimmer_provenance,
            "retail_menu": {
                key: page[key]
                for key in (
                    "source",
                    "source_sha256",
                    "title_source_va",
                    "option_array_source_va",
                    "option_table",
                )
            },
            "language": str(language_path),
        },
        "runtime_state": {
            "selected_category": selected_category,
            "selected_option": selected_option,
            "copy_profile": copy_profile,
            "progression": progression_state,
            "dlc_visible": dlc_visible,
            "party_play": party_play_state,
            "animation_policy": (
                "play source-authored open, close, row-state, title-bounce, "
                "icon, and synchronized selected-row glow timelines"
            ),
            "dimmer_policy": "freeze authored intro at its source stop frame",
        },
        "title": {
            "mode": mode_title,
            "mode_language_index": (
                mode_source["index"] if copy_profile == "retail" else None
            ),
            "retail_mode": retail_mode_title,
            "retail_mode_language_index": mode_source["index"],
            "column": column_title,
            "column_language_index": column_source["index"],
            "mode_measurement": mode_measure,
        },
        "categories": category_records,
        "options": row_records,
        "layout": {
            "root_frame": root_frame,
            "root_display": root_display,
            "column_matrix": column_matrix,
            "row_matrices": row_matrices,
            "constants": {
                "COLUMN_Y": CAREER_COLUMN_Y,
                "BASE_SPACING": CAREER_BASE_SPACING,
                "SELECT_MARGIN": CAREER_SELECT_MARGIN,
            },
            "position_column_source": {
                "bundle": core["key"],
                "function": "PositionColumn",
                "bytecode_start": 52196,
                "bytecode_end": 52404,
                "semantics": (
                    "column_y = COLUMN_Y + SELECT_MARGIN only when "
                    "selected_option == 0; add SELECT_MARGIN before the "
                    "selected row and before the row directly below it"
                ),
            },
        },
        "animations": {
            "selected_glow": selected_glow_animation,
            "motion": {
                "milliseconds_per_frame": core_frame_duration,
                "clips": motion_clips,
            },
        },
        "scene": {
            "primitives": scene["primitives"],
            "text": scene["text"],
        },
        "validation": {
            "unresolved": unresolved,
            "renderable": not unresolved,
            "counts": {
                "primitives": len(scene["primitives"]),
                "text": len(scene["text"]),
                "unresolved": len(unresolved),
            },
        },
    }


def compile_career_main_states(
    cache_root: Path,
    image_path: Path,
    *,
    party_play_state: str = "disabled",
    progression_state: str = "welcome",
    dlc_visible: bool = False,
    copy_profile: str = "custom",
) -> tuple[dict, list[tuple[str, dict]]]:
    page = extract_menu_page(image_path, PAGE_TITLE_ID)
    variants = []
    for category_index, category in enumerate(page["categories"]):
        option_count = len(category["option_indices"])
        for option_index in range(option_count):
            try:
                scene = compile_career_main(
                    cache_root,
                    image_path,
                    selected_category=category_index,
                    selected_option=option_index,
                    party_play_state=party_play_state,
                    progression_state=progression_state,
                    dlc_visible=dlc_visible,
                    copy_profile=copy_profile,
                )
            except ValueError as error:
                if "disabled PartyPlay" in str(error):
                    continue
                raise
            filename = (
                f"category_{category_index}_option_{option_index}.json"
            )
            variants.append((filename, scene))
    index = {
        "format": "skate3-career-main-scene-index",
        "version": 4,
        "copy_profile": copy_profile,
        "animation_policy": (
            "source-authored open, close, row navigation, title, icon, and "
            "selected-row glow playback"
        ),
        "variants": [
            {
                "category": scene["runtime_state"]["selected_category"],
                "option": scene["runtime_state"]["selected_option"],
                "internal_name": scene["options"][
                    scene["runtime_state"]["selected_option"]
                ]["internal_name"],
                "label": scene["options"][
                    scene["runtime_state"]["selected_option"]
                ]["label"],
                "path": filename,
            }
            for filename, scene in variants
        ],
    }
    return index, variants
