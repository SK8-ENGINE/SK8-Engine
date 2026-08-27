from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import sys
import traceback

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
TOOL_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TOOL_ROOT))

import owned_world_material_addon as addon
from generate_grinds import GrindSettings, generate_grinds
from scene_groups import (
    ObjectGroup,
    build_auxiliary_groups,
    build_light_groups,
    build_mesh_groups,
)


ROLE_COLLECTIONS = {
    "DEFAULT": "OW_GROUP_1_PRESENTATION_COLLISION",
    "COLLISION_ONLY": "OW_GROUP_2_NO_PRESENTATION",
    "VISUAL_ONLY": "OW_GROUP_3_NO_COLLISION",
    "GRIND": "OW_GROUP_4_GRINDS",
    "PATH": "OW_GROUP_5_PATHING",
}
MESH_ROLES = {"DEFAULT", "COLLISION_ONLY", "VISUAL_ONLY", "IGNORE"}
CURVE_ROLES = {"GRIND", "PATH", "IGNORE"}
GENERATED_LIGHT_PROPERTY = "sk8_auto_generated_light"


def _arguments() -> argparse.Namespace:
    values = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True)
    parser.add_argument("--output-blend", required=True)
    return parser.parse_args(values)


def _vector(values, field: str) -> Vector:
    if not isinstance(values, list) or len(values) != 3:
        raise ValueError(f"{field} must contain three numbers")
    return Vector(float(value) for value in values)


def _world_bounds(obj: bpy.types.Object) -> tuple[Vector, Vector]:
    points = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    minimum = Vector(
        (
            min(point.x for point in points),
            min(point.y for point in points),
            min(point.z for point in points),
        )
    )
    maximum = Vector(
        (
            max(point.x for point in points),
            max(point.y for point in points),
            max(point.z for point in points),
        )
    )
    return minimum, maximum


def _ensure_collection(name: str) -> bpy.types.Collection:
    collection = bpy.data.collections.get(name)
    if collection is None:
        collection = bpy.data.collections.new(name)
        bpy.context.scene.collection.children.link(collection)
    return collection


def _apply_materials(plan: dict) -> int:
    count = 0
    for assignment in plan.get("materials", []):
        name = str(assignment["material"])
        material = bpy.data.materials.get(name)
        if material is None:
            raise ValueError(f"plan references missing material {name!r}")
        audio = int(assignment["audio_surface"])
        physics = int(assignment["physics_surface"])
        pattern = int(assignment["surface_pattern"])
        if not 0 <= audio <= 93:
            raise ValueError(f"{name}: audio_surface must be 0..93")
        if not 0 <= physics <= 13:
            raise ValueError(f"{name}: physics_surface must be 0..13")
        if not 0 <= pattern <= 15:
            raise ValueError(f"{name}: surface_pattern must be 0..15")
        material["ow_audio_surface"] = audio
        material["ow_physics_surface"] = physics
        material["ow_surface_pattern"] = pattern
        material["ow_collision_enabled"] = bool(
            assignment.get("collision_enabled", True)
        )
        material["ow_agent_mapped"] = True
        material["ow_auto_imported"] = False
        count += 1
    return count


def _validate_object_roles(plan: dict, version: int) -> None:
    assignments = plan.get("object_roles", [])
    if not isinstance(assignments, list):
        raise ValueError("object_roles must be a list")
    seen: set[str] = set()
    for assignment in assignments:
        name = str(assignment["object"])
        role = str(assignment["role"]).upper()
        if name in seen:
            raise ValueError(f"duplicate object role for {name!r}")
        seen.add(name)
        obj = bpy.data.objects.get(name)
        if obj is None:
            raise ValueError(f"plan references missing object {name!r}")
        allowed = MESH_ROLES if obj.type == "MESH" else (
            CURVE_ROLES if obj.type == "CURVE" else {"IGNORE"}
        )
        if role not in allowed:
            raise ValueError(
                f"{name}: role {role!r} is invalid for {obj.type}"
            )
    if version < 2:
        return
    required = {
        obj.name
        for obj in bpy.context.scene.objects
        if obj.type == "MESH"
        and len(obj.data.polygons) > 0
        and obj.name != "OW_SPAWN"
    }
    missing = sorted(required - seen, key=str.casefold)
    if missing:
        preview = ", ".join(repr(name) for name in missing[:8])
        suffix = "" if len(missing) <= 8 else f" (+{len(missing) - 8} more)"
        raise ValueError(
            "version 2 plans must classify every mesh; missing "
            f"{preview}{suffix}"
        )


def _group_lookup(groups: list[ObjectGroup]) -> dict[str, ObjectGroup]:
    return {group.group_id: group for group in groups}


def _expand_group_roles(
    plan: dict,
    mesh_groups: list[ObjectGroup],
) -> tuple[dict, dict]:
    assignments = plan.get("object_group_roles", [])
    if not isinstance(assignments, list):
        raise ValueError("object_group_roles must be a list")
    groups = _group_lookup(mesh_groups)
    seen_groups: set[str] = set()
    role_by_object: dict[str, str] = {}

    for assignment in assignments:
        identifier = str(assignment["group"])
        role = str(assignment["role"]).upper()
        if identifier in seen_groups:
            raise ValueError(f"duplicate object group role for {identifier!r}")
        seen_groups.add(identifier)
        group = groups.get(identifier)
        if group is None:
            raise ValueError(
                f"plan references missing mesh group {identifier!r}"
            )
        if role not in MESH_ROLES:
            raise ValueError(
                f"{identifier}: role {role!r} is invalid for mesh groups"
            )
        for member in group.members:
            role_by_object[member.name] = role

    missing_groups = sorted(set(groups) - seen_groups)
    if missing_groups:
        preview = ", ".join(repr(name) for name in missing_groups[:8])
        suffix = (
            ""
            if len(missing_groups) <= 8
            else f" (+{len(missing_groups) - 8} more)"
        )
        raise ValueError(
            "version 3 plans must classify every mesh group; missing "
            f"{preview}{suffix}"
        )

    overrides = plan.get("object_roles", [])
    if not isinstance(overrides, list):
        raise ValueError("object_roles must be a list")
    seen_overrides: set[str] = set()
    non_mesh_assignments = []
    mesh_override_count = 0
    for assignment in overrides:
        name = str(assignment["object"])
        role = str(assignment["role"]).upper()
        if name in seen_overrides:
            raise ValueError(f"duplicate object role override for {name!r}")
        seen_overrides.add(name)
        obj = bpy.data.objects.get(name)
        if obj is None:
            raise ValueError(f"plan references missing object {name!r}")
        if obj.type == "MESH" and len(obj.data.polygons) > 0:
            if role not in MESH_ROLES:
                raise ValueError(
                    f"{name}: role {role!r} is invalid for meshes"
                )
            role_by_object[name] = role
            mesh_override_count += 1
        else:
            non_mesh_assignments.append(
                {"object": name, "role": role}
            )

    expanded = dict(plan)
    expanded["object_roles"] = [
        {"object": name, "role": role_by_object[name]}
        for name in sorted(role_by_object, key=str.casefold)
    ]
    expanded["object_roles"].extend(non_mesh_assignments)
    return expanded, {
        "mesh_groups": len(mesh_groups),
        "group_role_assignments": len(assignments),
        "mesh_object_expansions": len(role_by_object),
        "exact_object_overrides": mesh_override_count,
    }


def _expand_group_sources(
    plan: dict,
    groups: list[ObjectGroup],
) -> dict:
    lookup = _group_lookup(groups)
    expanded = dict(plan)
    expanded_lights = []
    for entry in plan.get("lights", []):
        identifier = entry.get("source_group")
        if identifier is None:
            expanded_lights.append(entry)
            continue
        if "source_object" in entry:
            raise ValueError(
                "a light cannot contain both source_group and source_object"
            )
        group = lookup.get(str(identifier))
        if group is None:
            raise ValueError(
                f"light references missing object group {identifier!r}"
            )
        for member in group.members:
            instance = dict(entry)
            instance.pop("source_group", None)
            instance["source_object"] = member.name
            expanded_lights.append(instance)
    expanded["lights"] = expanded_lights

    spawn = plan.get("spawn")
    if isinstance(spawn, dict) and "source_group" in spawn:
        if "source_object" in spawn:
            raise ValueError(
                "spawn cannot contain both source_group and source_object"
            )
        identifier = str(spawn["source_group"])
        group = lookup.get(identifier)
        if group is None:
            raise ValueError(
                f"spawn references missing object group {identifier!r}"
            )
        expanded_spawn = dict(spawn)
        expanded_spawn.pop("source_group", None)
        expanded_spawn["source_object"] = group.representative.name
        expanded["spawn"] = expanded_spawn
    return expanded


def _remove_from_skate_groups(obj: bpy.types.Object) -> None:
    group_names = set(addon._group_collections())
    non_skate = [
        collection
        for collection in obj.users_collection
        if collection.name not in group_names
    ]
    if not non_skate and obj.name not in bpy.context.scene.collection.objects:
        bpy.context.scene.collection.objects.link(obj)
    for collection in tuple(obj.users_collection):
        if collection.name in group_names:
            collection.objects.unlink(obj)


def _apply_roles(plan: dict) -> int:
    count = 0
    for assignment in plan.get("object_roles", []):
        name = str(assignment["object"])
        role = str(assignment["role"]).upper()
        obj = bpy.data.objects.get(name)
        if role == "IGNORE":
            obj["sk8_explicitly_ignored"] = True
            _remove_from_skate_groups(obj)
        else:
            obj.pop("sk8_explicitly_ignored", None)
            addon._move_to_group(
                bpy.context.scene, obj, ROLE_COLLECTIONS[role]
            )
        count += 1
    return count


def _sync_collision_materials() -> int:
    visual_objects = addon.exporter._objects_from_collections(
        addon.exporter.PRESENTATION_COLLISION_COLLECTION,
        addon.exporter.NO_COLLISION_COLLECTION,
    )
    collision_objects = addon.exporter._objects_from_collections(
        addon.exporter.PRESENTATION_COLLISION_COLLECTION,
        addon.exporter.NO_PRESENTATION_COLLECTION,
    )
    used_export_materials = {
        slot.material.name
        for obj in [*visual_objects, *collision_objects]
        if obj.type == "MESH"
        for slot in obj.material_slots
        if slot.material is not None
    }
    fallback = next(
        (
            bpy.data.materials.get(name)
            for name in sorted(used_export_materials, key=str.casefold)
            if bpy.data.materials.get(name) is not None
            and bool(
                bpy.data.materials[name].get("ow_collision_enabled", True)
            )
        ),
        addon._default_material(),
    )
    count = 0
    for obj in collision_objects:
        if obj.type != "MESH":
            continue
        material = addon._first_collision_material(
            obj, used_export_materials, fallback
        )
        obj["ow_material"] = material.name
        if "ow_upward_surface" not in obj:
            obj["ow_upward_surface"] = False
        addon._hydrate_physics(obj)
        count += 1
    return count


def _remove_generated_lights() -> None:
    for obj in list(bpy.data.objects):
        if obj.type == "LIGHT" and bool(
            obj.get(GENERATED_LIGHT_PROPERTY, False)
        ):
            bpy.data.objects.remove(obj, do_unlink=True)


def _light_location(entry: dict) -> Vector:
    if "location" in entry:
        location = _vector(entry["location"], "light.location")
    else:
        source_name = str(entry.get("source_object", ""))
        source = bpy.data.objects.get(source_name)
        if source is None:
            raise ValueError(
                "light requires a valid source_object or location"
            )
        minimum, maximum = _world_bounds(source)
        location = (minimum + maximum) * 0.5
    if "offset" in entry:
        location += _vector(entry["offset"], "light.offset")
    return location


def _apply_lights(plan: dict) -> int:
    _remove_generated_lights()
    count = 0
    for index, entry in enumerate(plan.get("lights", []), 1):
        light_type = str(entry.get("type", "POINT")).upper()
        if light_type not in {"POINT", "SPOT", "AREA"}:
            raise ValueError(f"unsupported light type {light_type!r}")
        source_name = str(entry.get("source_object", f"Light_{index}"))
        data = bpy.data.lights.new(
            f"SK8_AutoLight_{source_name}_{index:04d}_Data",
            type=light_type,
        )
        data.color = _vector(
            entry.get("color", [1.0, 0.82, 0.62]), "light.color"
        )
        data.energy = max(0.0, float(entry.get("energy", 800.0)))
        data.cutoff_distance = max(0.01, float(entry.get("range", 12.0)))
        data.shadow_soft_size = max(
            0.01, float(entry.get("softness", 0.25))
        )
        if light_type == "SPOT":
            data.spot_size = math.radians(
                float(entry.get("spot_degrees", 55.0))
            )
            data.spot_blend = float(entry.get("spot_blend", 0.45))
        if light_type == "AREA":
            data.shape = "DISK"
            data.size = max(0.01, float(entry.get("size", 1.0)))
        obj = bpy.data.objects.new(data.name.removesuffix("_Data"), data)
        obj[GENERATED_LIGHT_PROPERTY] = True
        obj["sk8_source_object"] = source_name
        bpy.context.scene.collection.objects.link(obj)
        obj.location = _light_location(entry)
        if "target" in entry:
            direction = _vector(entry["target"], "light.target") - obj.location
            if direction.length > 1.0e-6:
                obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
        count += 1
    return count


def _apply_spawn(plan: dict) -> bool:
    entry = plan.get("spawn")
    if not entry:
        return False
    if "location" in entry:
        location = _vector(entry["location"], "spawn.location")
    else:
        source_name = str(entry.get("source_object", ""))
        source = bpy.data.objects.get(source_name)
        if source is None:
            raise ValueError(
                "spawn requires a valid source_object or location"
            )
        minimum, maximum = _world_bounds(source)
        location = Vector(
            (
                (minimum.x + maximum.x) * 0.5,
                (minimum.y + maximum.y) * 0.5,
                maximum.z,
            )
        )
    if "offset" in entry:
        location += _vector(entry["offset"], "spawn.offset")
    spawn = bpy.data.objects.get("OW_SPAWN")
    if spawn is None:
        spawn = bpy.data.objects.new("OW_SPAWN", None)
        bpy.context.scene.collection.objects.link(spawn)
        spawn.empty_display_type = "ARROWS"
        spawn.empty_display_size = 2.0
    spawn.location = location
    heading = math.radians(float(entry.get("heading_degrees", 0.0)))
    spawn.rotation_euler = (0.0, 0.0, heading)
    spawn["ow_heading_radians"] = heading
    spawn["sk8_auto_placed_spawn"] = True
    return True


def _apply_grinds(plan: dict) -> dict:
    entry = plan.get("grinds", {})
    if entry.get("enabled", True) is False:
        return {"disabled": True}
    settings = GrindSettings(
        minimum_segment_length=float(
            entry.get("minimum_segment_length", 0.35)
        ),
        minimum_chain_length=float(entry.get("minimum_chain_length", 0.8)),
        minimum_corner_angle_degrees=float(
            entry.get("minimum_corner_angle_degrees", 8.0)
        ),
        maximum_slope_degrees=float(
            entry.get("maximum_slope_degrees", 65.0)
        ),
        deduplicate_distance=float(
            entry.get("deduplicate_distance", 0.08)
        ),
        join_distance=float(entry.get("join_distance", 0.04)),
        density_cell_size=float(entry.get("density_cell_size", 2.0)),
        maximum_splines_per_cell=int(
            entry.get("maximum_splines_per_cell", 4)
        ),
        maximum_splines_per_source=int(
            entry.get("maximum_splines_per_source", 128)
        ),
    )
    return generate_grinds(settings)


def apply_plan(plan: dict) -> dict:
    version = int(plan.get("version", 0))
    if version not in {1, 2, 3}:
        raise ValueError("map plan version must be 1, 2, or 3")
    group_stats = None
    expanded_plan = plan
    if version == 3:
        objects = list(bpy.context.scene.objects)
        mesh_groups = build_mesh_groups(objects)
        auxiliary_groups = build_auxiliary_groups(objects)
        light_groups = build_light_groups(objects)
        expanded_plan, group_stats = _expand_group_roles(
            expanded_plan, mesh_groups
        )
        expanded_plan = _expand_group_sources(
            expanded_plan,
            [*mesh_groups, *auxiliary_groups, *light_groups],
        )
    _validate_object_roles(expanded_plan, version)
    addon.register()
    auto_prepare_stats, auto_prepare_warnings = addon._auto_prepare_scene(
        bpy.context
    )
    if "map_name" in expanded_plan:
        bpy.context.scene["ow_map_name"] = str(expanded_plan["map_name"])
    materials = _apply_materials(expanded_plan)
    object_roles = _apply_roles(expanded_plan)
    collision_objects = _sync_collision_materials()
    return {
        "auto_prepare": auto_prepare_stats,
        "auto_prepare_warnings": auto_prepare_warnings,
        "materials": materials,
        "object_groups": group_stats,
        "object_roles": object_roles,
        "collision_objects": collision_objects,
        "lights": _apply_lights(expanded_plan),
        "spawn": _apply_spawn(expanded_plan),
        "grinds": _apply_grinds(expanded_plan),
    }


def main() -> None:
    args = _arguments()
    plan_path = Path(args.plan).resolve()
    output_path = Path(args.output_blend).resolve()
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    result = apply_plan(plan)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(output_path), compress=True)
    print("SK8_AUTO_MAP_APPLIED", output_path, json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(1)
