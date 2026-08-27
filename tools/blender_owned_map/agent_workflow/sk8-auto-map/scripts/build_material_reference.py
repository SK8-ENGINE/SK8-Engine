from __future__ import annotations

import ast
from pathlib import Path
import re


SKILL_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = Path(__file__).resolve().parents[5]
ADDON_FILE = (
    SOURCE_ROOT
    / "tools"
    / "blender_owned_map"
    / "owned_world_material_addon"
    / "__init__.py"
)
OUTPUT_FILE = SKILL_ROOT / "references" / "MATERIALS.md"


def _assignments(source: str) -> dict[str, ast.AST]:
    tree = ast.parse(source)
    result = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id not in result:
            result[target.id] = node.value
    return result


def _audio_description(name: str) -> str:
    if name == "Undefined":
        return "Fallback audio when no native surface is selected."
    if name == "Max_Mappable_Surface":
        return "Native upper-bound marker; normally avoid assigning it."
    words = name.replace("_or_", " or ").replace("_", " ")
    families = (
        ("Asphalt", "asphalt"),
        ("Concrete", "concrete"),
        ("Metal", "metal"),
        ("Wood", "wood"),
        ("Plastic", "plastic"),
        ("Glass", "glass"),
        ("Plexiglass", "plexiglass"),
        ("Brick", "brick"),
        ("Grass", "grass"),
        ("Leaves", "leaf litter"),
        ("Bush", "bush or foliage"),
        ("Tile", "ceramic tile"),
        ("Marble", "marble or slate"),
        ("Ice", "ice"),
        ("DeepSnow", "deep snow"),
        ("PackedSnow", "packed snow"),
        ("Paper", "paper"),
        ("Cardboard", "cardboard"),
        ("Cloth", "cloth"),
        ("Wire", "wire or cable"),
    )
    family = next(
        (description for prefix, description in families if name.startswith(prefix)),
        words.casefold(),
    )
    return f"Native rolling, impact, and grind audio for {family}; variant: {words}."


def main() -> None:
    source = ADDON_FILE.read_text(encoding="utf-8")
    values = _assignments(source)
    audio_text = ast.literal_eval(values["_AUDIO_NAMES"])
    audio_names = [
        name.strip()
        for name in audio_text.replace("\n", "").split("|")
    ]
    physics = ast.literal_eval(values["PHYSICS_ITEMS"])
    patterns = ast.literal_eval(values["PATTERN_ITEMS"])
    presets = ast.literal_eval(values["PRESETS"])
    preset_labels = {
        identifier: label
        for identifier, label, _description in ast.literal_eval(
            values["PRESET_ITEMS"]
        )
    }

    lines = [
        "# Skate 3 material reference",
        "",
        "Use these exact numeric IDs in `map_plan.json`.",
        "",
        "## Audio surfaces",
        "",
        "| ID | Name | Description |",
        "|---:|---|---|",
    ]
    for index, name in enumerate(audio_names):
        lines.append(
            f"| {index} | `{name}` | {_audio_description(name)} |"
        )

    lines.extend(
        [
            "",
            "## Physics surfaces",
            "",
            "| ID | Name | Description |",
            "|---:|---|---|",
        ]
    )
    for identifier, name, description in physics:
        lines.append(f"| {identifier} | `{name}` | {description}. |")

    lines.extend(
        [
            "",
            "## Contact patterns",
            "",
            "| ID | Name | Description |",
            "|---:|---|---|",
        ]
    )
    for identifier, name, description in patterns:
        lines.append(f"| {identifier} | `{name}` | {description}. |")

    lines.extend(
        [
            "",
            "## Useful existing presets",
            "",
            "| Preset | Audio | Physics | Pattern | Typical use |",
            "|---|---:|---:|---:|---|",
        ]
    )
    for identifier, values_tuple in presets.items():
        audio, physics_id, pattern, _roughness, _metallic, _alpha = values_tuple
        label = preset_labels.get(identifier, identifier.replace("_", " ").title())
        lines.append(
            f"| `{identifier}` | {audio} | {physics_id} | {pattern} | {label} |"
        )

    OUTPUT_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated {OUTPUT_FILE} with {len(audio_names)} audio surfaces.")


if __name__ == "__main__":
    main()
