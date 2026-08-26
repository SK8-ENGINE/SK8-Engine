from __future__ import annotations

import hashlib
import struct
from pathlib import Path

from .binary import FormatError, align


class FlatExecutable:
    def __init__(self, path: Path, base_address: int = 0x82000000):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.base_address = base_address

    def va(self, offset: int) -> int:
        return self.base_address + offset

    def offset(self, address: int) -> int:
        offset = address - self.base_address
        if offset < 0 or offset >= len(self.data):
            raise FormatError(f"address {address:#x} lies outside {self.path}")
        return offset

    def u32(self, offset: int) -> int:
        if offset < 0 or offset > len(self.data) - 4:
            raise FormatError(f"offset {offset:#x} lies outside {self.path}")
        return struct.unpack_from(">I", self.data, offset)[0]

    def i32(self, offset: int) -> int:
        if offset < 0 or offset > len(self.data) - 4:
            raise FormatError(f"offset {offset:#x} lies outside {self.path}")
        return struct.unpack_from(">i", self.data, offset)[0]

    def cstring_at_va(self, address: int, limit: int = 512) -> str:
        offset = self.offset(address)
        end = self.data.find(b"\0", offset, min(len(self.data), offset + limit))
        if end < 0:
            raise FormatError(f"unterminated string at {address:#x}")
        try:
            return self.data[offset:end].decode("ascii")
        except UnicodeDecodeError as error:
            raise FormatError(f"non-ASCII string at {address:#x}") from error

    def unique_string(self, value: str) -> tuple[int, int]:
        needle = value.encode("ascii") + b"\0"
        first = self.data.find(needle)
        if first < 0:
            raise FormatError(f"{self.path}: string {value!r} was not found")
        second = self.data.find(needle, first + 1)
        if second >= 0:
            raise FormatError(f"{self.path}: string {value!r} is not unique")
        return first, self.va(first)


def _is_label(value: str) -> bool:
    return value.startswith("ID_") or value.startswith("#")


def _is_icon(value: str) -> bool:
    return bool(value) and value == value.lower() and " " not in value


def _option_at(image: FlatExecutable, offset: int) -> dict | None:
    try:
        fields = [image.u32(offset + index * 4) for index in range(5)]
        internal = image.cstring_at_va(fields[0])
        label = image.cstring_at_va(fields[1])
        icon = image.cstring_at_va(fields[2])
        condition = fields[3]
        helper = image.cstring_at_va(fields[4])
    except FormatError:
        return None
    if (
        not internal
        or internal.startswith(("#", "ID_"))
        or not _is_label(label)
        or not _is_icon(icon)
        or condition not in {*range(0x100), 0xFFFFFFFF}
        or not _is_label(helper)
    ):
        return None
    return {
        "internal_name": internal,
        "label_id": label,
        "icon_frame": icon,
        "condition": -1 if condition == 0xFFFFFFFF else condition,
        "helper_id": helper,
        "source_va": image.va(offset),
    }


def _find_option_record(image: FlatExecutable, internal_name: str) -> int:
    candidates = []
    string_needle = internal_name.encode("ascii") + b"\0"
    string_cursor = 0
    while True:
        string_cursor = image.data.find(string_needle, string_cursor)
        if string_cursor < 0:
            break
        pointer_needle = struct.pack(">I", image.va(string_cursor))
        pointer_cursor = 0
        while True:
            pointer_cursor = image.data.find(pointer_needle, pointer_cursor)
            if pointer_cursor < 0:
                break
            if pointer_cursor % 4 == 0 and _option_at(image, pointer_cursor):
                candidates.append(pointer_cursor)
            pointer_cursor += 1
        string_cursor += 1
    candidates = sorted(set(candidates))
    if len(candidates) != 1:
        raise FormatError(
            f"{image.path}: expected one descriptor for {internal_name!r}, "
            f"found {len(candidates)}"
        )
    return candidates[0]


def find_option_table(image: FlatExecutable) -> list[dict]:
    anchor = _find_option_record(image, "ChallengeMap")
    start = anchor
    while start >= 20 and _option_at(image, start - 20):
        start -= 20
    options = []
    offset = start
    while True:
        option = _option_at(image, offset)
        if not option:
            break
        option["index"] = len(options)
        options.append(option)
        offset += 20
    if len(options) < 40:
        raise FormatError(
            f"{image.path}: descriptor run is unexpectedly short ({len(options)})"
        )
    if options[(anchor - start) // 20]["internal_name"] != "ChallengeMap":
        raise FormatError("option descriptor indexing failed its anchor check")
    return options


def extract_game_settings_menu(
    image_path: Path,
    *,
    base_address: int = 0x82000000,
) -> dict:
    """Recover the top-level Game Settings descriptor/index tables.

    The retail executable stores each settings row as a pair of pointers to
    its localization ID and control type. The top-level page is a separate
    integer index array into that descriptor run.
    """
    image = FlatExecutable(image_path, base_address)
    anchor_offset, anchor_va = image.unique_string(
        "ID_GAMESETTINGS_MUSICPLAYER"
    )
    del anchor_offset
    pointer = struct.pack(">I", anchor_va)
    references = [
        offset
        for offset in range(0, len(image.data) - 3, 4)
        if image.data[offset : offset + 4] == pointer
    ]
    if len(references) != 1:
        raise FormatError(
            f"{image.path}: expected one Game Settings descriptor anchor, "
            f"found {len(references)}"
        )
    table_start = references[0] - 8

    descriptors = []
    offset = table_start
    while offset <= len(image.data) - 8:
        try:
            label_id = image.cstring_at_va(image.u32(offset))
            control_type = image.cstring_at_va(image.u32(offset + 4))
        except FormatError:
            break
        if not (
            (label_id == "0" or label_id.startswith("ID_GAMESETTINGS_"))
            and control_type in ("option", "slider", "selector")
        ):
            break
        descriptors.append(
            {
                "index": len(descriptors),
                "label_id": label_id,
                "control_type": control_type,
                "source_va": f"{image.va(offset):#x}",
            }
        )
        offset += 8
    if len(descriptors) < 36:
        raise FormatError(
            f"{image.path}: Game Settings descriptor run is unexpectedly "
            f"short ({len(descriptors)})"
        )

    expected_indices = [2, 3, 4, 5, 6, 35]
    indices = [image.u32(offset + index * 4) for index in range(6)]
    if indices != expected_indices:
        raise FormatError(
            f"{image.path}: top-level Game Settings option array failed "
            f"validation ({indices!r})"
        )
    if image.u32(offset + 6 * 4) != 0:
        raise FormatError(
            f"{image.path}: top-level Game Settings option array is not "
            "terminated by the retail zero sentinel"
        )

    options = []
    for order, descriptor_index in enumerate(indices):
        descriptor = dict(descriptors[descriptor_index])
        if descriptor["control_type"] != "option":
            raise FormatError(
                "top-level Game Settings row is not an option control"
            )
        descriptor["order"] = order
        options.append(descriptor)

    title_va = image.u32(table_start - 56)
    title_id = image.cstring_at_va(title_va)
    if title_id != "ID_GAMESETTINGS_TITLE":
        raise FormatError(
            f"{image.path}: Game Settings title table failed validation"
        )
    return {
        "format": "skate3-retail-game-settings-menu",
        "source": str(Path(image_path)),
        "source_sha256": hashlib.sha256(image.data).hexdigest(),
        "base_address": f"{base_address:#x}",
        "title_id": title_id,
        "title_source_va": f"{title_va:#x}",
        "descriptor_table": {
            "source_va": f"{image.va(table_start):#x}",
            "count": len(descriptors),
        },
        "option_array_source_va": f"{image.va(offset):#x}",
        "option_indices": indices,
        "options": options,
    }


def _category_at(image: FlatExecutable, offset: int) -> dict | None:
    try:
        internal = image.cstring_at_va(image.u32(offset))
        label = image.cstring_at_va(image.u32(offset + 4))
        icon = image.cstring_at_va(image.u32(offset + 8))
    except FormatError:
        return None
    if (
        not internal
        or internal.startswith(("#", "ID_"))
        or not label.startswith("ID_CROSSBAR_")
        or not _is_icon(icon)
    ):
        return None
    return {
        "internal_name": internal,
        "label_id": label,
        "icon_frame": icon,
        "source_va": image.va(offset),
    }


def _categories_before_options(
    image: FlatExecutable, options: list[dict]
) -> dict[str, dict]:
    option_start = image.offset(options[0]["source_va"])
    category_start = option_start - 5 * 12
    categories = []
    for index in range(5):
        category = _category_at(image, category_start + index * 12)
        if not category:
            raise FormatError("five category descriptors do not precede option table")
        categories.append(category)
    return {category["internal_name"]: category for category in categories}


_CATEGORY_ANCHORS = {
    "SinglePlayer": "ChallengeMap",
    "Multiplayer": "OnlineChallengeMap",
    "Create": "ReplayEditor",
    "Learn": "TrickGuide",
    "Options": "GameSettings",
}


def _extract_category_slots(
    image: FlatExecutable,
    array_offset: int,
    options: list[dict],
    categories: dict[str, dict],
) -> list[dict]:
    result = []
    for slot in range(5):
        slot_offset = array_offset + slot * 8 * 4
        indices = []
        for item in range(8):
            value = image.i32(slot_offset + item * 4)
            if value == -1:
                break
            if value < 0 or value >= len(options):
                raise FormatError(
                    f"invalid option index {value} in category slot {slot}"
                )
            indices.append(value)
        else:
            raise FormatError(f"category slot {slot} has no terminator")
        internals = {options[index]["internal_name"] for index in indices}
        matches = [
            category_name
            for category_name, anchor in _CATEGORY_ANCHORS.items()
            if anchor in internals
        ]
        if len(matches) != 1:
            raise FormatError(
                f"category slot {slot} does not have one exact category anchor"
            )
        category = dict(categories[matches[0]])
        category["slot"] = slot
        category["option_array_source_va"] = f"{image.va(slot_offset):#x}"
        category["option_indices"] = indices
        result.append(category)
    return result


def extract_menu_page(
    image_path: Path,
    title_id: str,
    *,
    base_address: int = 0x82000000,
) -> dict:
    image = FlatExecutable(image_path, base_address)
    options = find_option_table(image)
    categories = _categories_before_options(image, options)
    title_offset, title_va = image.unique_string(title_id)
    array_offset = align(title_offset + len(title_id) + 1, 4)
    indices = []
    for index in range(64):
        value = image.i32(array_offset + index * 4)
        if value == -1:
            break
        if value < 0 or value >= len(options):
            raise FormatError(
                f"{image.path}: invalid option index {value} after {title_id!r}"
            )
        indices.append(value)
    else:
        raise FormatError(f"{image.path}: unterminated option list for {title_id!r}")
    if not indices:
        raise FormatError(f"{image.path}: empty option list for {title_id!r}")

    selected = []
    for order, option_index in enumerate(indices):
        option = dict(options[option_index])
        option["order"] = order
        selected.append(option)

    category_slots = _extract_category_slots(
        image, array_offset, options, categories
    )

    return {
        "format": "skate3-retail-menu-page",
        "source": str(Path(image_path)),
        "source_sha256": hashlib.sha256(image.data).hexdigest(),
        "base_address": f"{base_address:#x}",
        "title_id": title_id,
        "title_source_va": f"{title_va:#x}",
        "option_array_source_va": f"{image.va(array_offset):#x}",
        "options": selected,
        "categories": category_slots,
        "all_options": options,
        "option_table": {
            "source_va": f"{options[0]['source_va']:#x}",
            "count": len(options),
        },
    }
