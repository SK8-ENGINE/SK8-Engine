"""Validate University material provenance inside the generated Blender file."""

from __future__ import annotations

from collections import Counter
import json

import bpy


EXPECTED_MODE_COUNTS = {0: 6389, 1: 2118, 2: 39}
REGRESSION_BINDINGS = {
    ("0xF6CC7BFCC2C45F8C", 40): (
        "0x861894DE4209CE82",
        "0x00800078",
        24,
        "0x2c70170a001d00aa",
        0,
    ),
    ("0xF6CC7BFCC2C45F8C", 41): (
        "0xFA839F99082D42ED",
        "0x0080007B",
        38,
        "0x2c70170a0004000e",
        0,
    ),
    ("0xF6CC7BFCC2C45F8C", 42): (
        "0x96879C786774C31D",
        "0x0080007E",
        12,
        "0x00008d6a03e3870a",
        0,
    ),
    ("0x759E349006948F63", 2): (
        "0x861894DE4209CE82",
        "0x00800006",
        0,
        "0x2c70170a001d00aa",
        0,
    ),
    ("0x759E349006948F63", 3): (
        "0xA0683D15728B6787",
        "0x00800009",
        3,
        "0x2c70170a00053a88",
        1,
    ),
}


def main() -> int:
    objects = [
        obj
        for obj in bpy.data.objects
        if obj.type == "MESH" and "skate3_asset_id" in obj
    ]
    if len(objects) != 8546:
        raise RuntimeError(
            f"University has {len(objects)} imported mesh parts, expected 8546"
        )

    modes: Counter[int] = Counter()
    unresolved: Counter[int] = Counter()
    lookup: dict[tuple[str, int], bpy.types.Object] = {}
    for obj in objects:
        alpha_mode = int(obj.get("skate3_alpha_mode", 0))
        texture_id = str(obj.get("skate3_texture_id", ""))
        modes[alpha_mode] += 1
        lookup[
            (
                str(obj["skate3_asset_id"]),
                int(obj["skate3_mesh_index"]),
            )
        ] = obj
        if not texture_id:
            if obj.data.materials:
                raise RuntimeError(
                    f"{obj.name!r} has no retail texture but has a material"
                )
            continue
        if not obj.data.materials:
            raise RuntimeError(
                f"{obj.name!r} lost material {texture_id}"
            )
        material = obj.data.materials[0]
        if (
            str(material.get("skate3_texture_id", "")) != texture_id
            or int(material.get("skate3_alpha_mode", -1)) != alpha_mode
        ):
            raise RuntimeError(
                f"{obj.name!r} material provenance does not match the mesh"
            )
        if "skate3_fallback_reason" in material:
            unresolved[alpha_mode] += 1

    if dict(modes) != EXPECTED_MODE_COUNTS:
        raise RuntimeError(
            f"University alpha modes changed: {dict(modes)}"
        )

    for key, expected in REGRESSION_BINDINGS.items():
        obj = lookup.get(key)
        if obj is None:
            raise RuntimeError(f"regression mesh {key!r} is missing")
        actual = (
            str(obj.get("skate3_retail_material_guid", "")),
            str(obj.get("skate3_retail_material_handle", "")),
            int(obj.get("skate3_retail_material_group_index", -1)),
            str(obj.get("skate3_texture_id", "")),
            int(obj.get("skate3_alpha_mode", 0)),
        )
        if actual != expected:
            raise RuntimeError(
                f"regression mesh {key!r}: {actual!r} != {expected!r}"
            )

    print(
        json.dumps(
            {
                "status": "UNIVERSITY_BLEND_MATERIALS_OK",
                "mesh_parts": len(objects),
                "alpha_modes": dict(sorted(modes.items())),
                "unresolved_by_alpha_mode": dict(sorted(unresolved.items())),
                "regression_bindings": len(REGRESSION_BINDINGS),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
