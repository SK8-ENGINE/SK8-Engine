"""Validate University material provenance inside the generated Blender file."""

from __future__ import annotations

from collections import Counter
import json
import struct

import bpy
from mathutils import Vector


EXPECTED_MODE_COUNTS = {0: 6389, 1: 2118, 2: 39}
EXPECTED_GRIND_RAILS = 4201
EXPECTED_GRIND_SEGMENTS = 27008
EXPECTED_CLOSED_GRIND_RAILS = 372
EXPECTED_COLLISION_SURFACES = 183
EXPECTED_COLLISION_TRIANGLES = 1_133_649
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

    grind_objects = [
        obj
        for obj in bpy.data.objects
        if obj.type == "CURVE"
        and bool(obj.get("skate3_retail_grind", False))
    ]
    if len(grind_objects) != EXPECTED_GRIND_RAILS:
        raise RuntimeError(
            f"University has {len(grind_objects)} retail grind rails, "
            f"expected {EXPECTED_GRIND_RAILS}"
        )
    grind_segments = 0
    closed_grinds = 0
    for obj in grind_objects:
        if len(obj.data.splines) != 1:
            raise RuntimeError(
                f"{obj.name!r} does not contain exactly one retail spline"
            )
        spline = obj.data.splines[0]
        if spline.type != "BEZIER":
            raise RuntimeError(f"{obj.name!r} is not a Bezier spline")
        segment_count = int(
            obj["skate3_retail_grind_segment_count"]
        )
        actual_segment_count = (
            len(spline.bezier_points)
            if spline.use_cyclic_u
            else len(spline.bezier_points) - 1
        )
        if actual_segment_count != segment_count:
            raise RuntimeError(
                f"{obj.name!r} has {actual_segment_count} Blender segments "
                f"but {segment_count} retail segments"
            )
        payload = bytes.fromhex(
            str(obj["skate3_retail_grind_segment_payload"])
        )
        if len(payload) != segment_count * 120:
            raise RuntimeError(
                f"{obj.name!r} has an invalid native segment payload"
            )
        if int(
            str(obj["skate3_retail_grind_spline_id"]),
            16,
        ) == 0 or int(
            str(obj["skate3_retail_grind_type_signature"]),
            16,
        ) == 0:
            raise RuntimeError(
                f"{obj.name!r} lost its retail spline identity"
            )

        points = spline.bezier_points
        for segment_index in range(segment_count):
            values = struct.unpack_from(
                ">30f",
                payload,
                segment_index * 120,
            )
            coefficient_a = values[0:3]
            coefficient_b = values[4:7]
            coefficient_c = values[8:11]
            coefficient_d = values[12:15]
            runtime_controls = (
                coefficient_d,
                tuple(
                    coefficient_d[axis] + coefficient_c[axis] / 3.0
                    for axis in range(3)
                ),
                tuple(
                    coefficient_d[axis]
                    + (
                        2.0 * coefficient_c[axis]
                        + coefficient_b[axis]
                    )
                    / 3.0
                    for axis in range(3)
                ),
                tuple(
                    coefficient_d[axis]
                    + coefficient_c[axis]
                    + coefficient_b[axis]
                    + coefficient_a[axis]
                    for axis in range(3)
                ),
            )
            expected_controls = tuple(
                Vector((point[0], -point[2], point[1]))
                for point in runtime_controls
            )
            current = points[segment_index]
            following = points[
                (segment_index + 1) % len(points)
            ]
            actual_controls = (
                obj.matrix_world @ current.co,
                obj.matrix_world @ current.handle_right,
                obj.matrix_world @ following.handle_left,
                obj.matrix_world @ following.co,
            )
            control_errors = [
                (actual - expected).length
                for actual, expected in zip(
                    actual_controls,
                    expected_controls,
                )
            ]
            # Blender stores curve points as float32. Retail cubic endpoints
            # are independently evaluated coefficients, so cancellation at
            # University-scale coordinates can add sub-millimetre rounding.
            if any(error > 2.0e-3 for error in control_errors):
                raise RuntimeError(
                    f"{obj.name!r} segment {segment_index} no longer "
                    "matches its exact retail cubic: "
                    f"control errors={control_errors}"
                )
        grind_segments += segment_count
        closed_grinds += bool(spline.use_cyclic_u)

    if (
        grind_segments != EXPECTED_GRIND_SEGMENTS
        or closed_grinds != EXPECTED_CLOSED_GRIND_RAILS
    ):
        raise RuntimeError(
            "University retail grind totals changed: "
            f"segments={grind_segments}, closed={closed_grinds}"
        )

    collision_objects = [
        obj
        for obj in bpy.data.objects
        if obj.type == "MESH"
        and bool(obj.get("skate3_retail_collision", False))
    ]
    if len(collision_objects) != EXPECTED_COLLISION_SURFACES:
        raise RuntimeError(
            f"University has {len(collision_objects)} retail collision "
            f"surfaces, expected {EXPECTED_COLLISION_SURFACES}"
        )
    collision_triangles = 0
    for obj in collision_objects:
        if len(obj.data.materials) != 1 or obj.data.materials[0] is None:
            raise RuntimeError(
                f"{obj.name!r} does not have one retail collision material"
            )
        surface = int(str(obj["skate3_retail_surface_id"]), 16)
        material = obj.data.materials[0]
        encoded = (
            int(material["ow_audio_surface"])
            | (int(material["ow_physics_surface"]) << 7)
            | (int(material["ow_surface_pattern"]) << 12)
        )
        if encoded != surface:
            raise RuntimeError(
                f"{obj.name!r} packed surface changed: "
                f"0x{encoded:04X} != 0x{surface:04X}"
            )
        triangle_count = len(obj.data.polygons)
        if triangle_count != int(obj["skate3_retail_triangle_count"]):
            raise RuntimeError(
                f"{obj.name!r} retail triangle count changed"
            )
        collision_triangles += triangle_count
    if collision_triangles != EXPECTED_COLLISION_TRIANGLES:
        raise RuntimeError(
            f"University has {collision_triangles} retail collision triangles, "
            f"expected {EXPECTED_COLLISION_TRIANGLES}"
        )

    print(
        json.dumps(
            {
                "status": "UNIVERSITY_BLEND_MATERIALS_OK",
                "mesh_parts": len(objects),
                "alpha_modes": dict(sorted(modes.items())),
                "unresolved_by_alpha_mode": dict(sorted(unresolved.items())),
                "regression_bindings": len(REGRESSION_BINDINGS),
                "grind_rails": len(grind_objects),
                "grind_segments": grind_segments,
                "closed_grind_rails": closed_grinds,
                "collision_surfaces": len(collision_objects),
                "collision_triangles": collision_triangles,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
