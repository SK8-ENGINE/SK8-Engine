from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import traceback

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scene_groups import (  # noqa: E402
    ObjectGroup,
    build_auxiliary_groups,
    build_light_groups,
    build_mesh_groups,
    current_skate_roles,
    material_names,
    world_bounds,
)
from render_scene_previews import render_scene_previews  # noqa: E402


def _arguments() -> argparse.Namespace:
    values = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--members-output")
    parser.add_argument("--preview-directory")
    return parser.parse_args(values)


def _round_values(values, digits: int = 4) -> list[float]:
    return [round(float(value), digits) for value in values]


def _rounded_bounds(obj: bpy.types.Object) -> dict | None:
    bounds = world_bounds(obj)
    if bounds is None:
        return None
    return {
        key: _round_values(bounds[key])
        for key in ("minimum", "maximum", "center", "size")
    }


def _principled_summary(material: bpy.types.Material) -> dict:
    result = {
        "base_color": _round_values(material.diffuse_color[:3]),
        "roughness": float(material.roughness),
        "metallic": float(material.metallic),
        "alpha": float(material.diffuse_color[3]),
        "emission_color": [0.0, 0.0, 0.0],
        "emission_strength": 0.0,
        "images": [],
    }
    if not material.use_nodes or material.node_tree is None:
        return result
    images = []
    for node in material.node_tree.nodes:
        if node.type == "TEX_IMAGE" and node.image is not None:
            images.append(
                {
                    "name": node.image.name,
                    "path": bpy.path.abspath(node.image.filepath),
                }
            )
        if node.type != "BSDF_PRINCIPLED":
            continue
        for input_name, key in (
            ("Base Color", "base_color"),
            ("Roughness", "roughness"),
            ("Metallic", "metallic"),
            ("Alpha", "alpha"),
            ("Emission Color", "emission_color"),
            ("Emission Strength", "emission_strength"),
        ):
            socket = node.inputs.get(input_name)
            if socket is None or socket.is_linked:
                continue
            value = socket.default_value
            if hasattr(value, "__len__") and not isinstance(value, str):
                result[key] = _round_values(value[:3])
            else:
                result[key] = round(float(value), 4)
    result["images"] = sorted(images, key=lambda item: item["name"].casefold())
    return result


def _rigid_body_summary(obj: bpy.types.Object) -> dict | None:
    rigid_body = obj.rigid_body
    if rigid_body is None:
        return None
    return {
        "type": rigid_body.type,
        "collision_shape": rigid_body.collision_shape,
        "kinematic": bool(rigid_body.kinematic),
        "enabled": bool(rigid_body.enabled),
    }


def _object_entry(obj: bpy.types.Object) -> dict:
    entry = {
        "name": obj.name,
        "type": obj.type,
        "collections": sorted(
            collection.name for collection in obj.users_collection
        ),
        "location": _round_values(obj.matrix_world.translation),
        "rotation_degrees": _round_values(
            value * 57.29577951308232
            for value in obj.matrix_world.to_euler("XYZ")
        ),
        "bounds": _rounded_bounds(obj),
        "materials": material_names(obj),
        "hide_render": bool(obj.hide_render),
        "hidden_viewport": bool(obj.hide_get()),
        "current_skate_roles": current_skate_roles(obj),
        "rigid_body_hint": _rigid_body_summary(obj),
    }
    if obj.type == "MESH":
        entry["vertices"] = len(obj.data.vertices)
        entry["edges"] = len(obj.data.edges)
        entry["polygons"] = len(obj.data.polygons)
    return entry


def _range_summary(vectors) -> dict:
    values = list(vectors)
    minimum = [
        min(float(value[index]) for value in values)
        for index in range(3)
    ]
    maximum = [
        max(float(value[index]) for value in values)
        for index in range(3)
    ]
    return {
        "minimum": _round_values(minimum),
        "maximum": _round_values(maximum),
    }


def _group_bounds(group: ObjectGroup) -> dict | None:
    bounds = [
        world_bounds(member)
        for member in group.members
        if getattr(member, "bound_box", None)
    ]
    bounds = [entry for entry in bounds if entry is not None]
    if not bounds:
        return None
    minimum = [
        min(float(entry["minimum"][index]) for entry in bounds)
        for index in range(3)
    ]
    maximum = [
        max(float(entry["maximum"][index]) for entry in bounds)
        for index in range(3)
    ]
    return {
        "minimum": _round_values(minimum),
        "maximum": _round_values(maximum),
        "size": _round_values(
            maximum[index] - minimum[index] for index in range(3)
        ),
    }


def _group_entry(group: ObjectGroup) -> dict:
    return {
        "group": group.group_id,
        "instance_count": len(group.members),
        "name_family": group.name_family,
        "representative": _object_entry(group.representative),
        "instance_location_range": _range_summary(
            member.matrix_world.translation for member in group.members
        ),
        "combined_bounds": _group_bounds(group),
        "shared_data": group.signature.get("data", {}),
    }


def _auxiliary_group_entry(group: ObjectGroup) -> dict:
    representative = group.representative
    if representative.type != "EMPTY":
        return _group_entry(group)
    return {
        "group": group.group_id,
        "instance_count": len(group.members),
        "name_family": group.name_family,
        "representative": {
            "name": representative.name,
            "type": representative.type,
            "collections": sorted(
                collection.name
                for collection in representative.users_collection
            ),
            "location": _round_values(
                representative.matrix_world.translation
            ),
            "current_skate_roles": current_skate_roles(representative),
        },
        "instance_location_range": _range_summary(
            member.matrix_world.translation for member in group.members
        ),
        "shared_data": group.signature.get("data", {}),
    }


def _light_group_entry(group: ObjectGroup) -> dict:
    representative = group.representative
    light = representative.data
    return {
        "group": group.group_id,
        "instance_count": len(group.members),
        "name_family": group.name_family,
        "representative": {
            "object": representative.name,
            "type": light.type,
            "location": _round_values(
                representative.matrix_world.translation
            ),
            "color": _round_values(light.color),
            "energy": round(float(light.energy), 4),
            "range": round(float(light.cutoff_distance), 4),
            "collections": sorted(
                collection.name
                for collection in representative.users_collection
            ),
        },
        "instance_location_range": _range_summary(
            member.matrix_world.translation for member in group.members
        ),
    }


def _material_usage(
    groups: list[ObjectGroup],
) -> dict[str, dict[str, int]]:
    usage: dict[str, dict[str, int]] = {}
    for group in groups:
        group_material_counts: dict[str, int] = {}
        for member in group.members:
            for material in set(material_names(member)):
                group_material_counts[material] = (
                    group_material_counts.get(material, 0) + 1
                )
        for material, count in group_material_counts.items():
            usage.setdefault(material, {})[group.group_id] = count
    return usage


def build_inventory() -> tuple[dict, dict]:
    objects = list(bpy.context.scene.objects)
    mesh_groups = build_mesh_groups(objects)
    auxiliary_groups = build_auxiliary_groups(objects)
    light_groups = build_light_groups(objects)
    all_non_light_groups = [*mesh_groups, *auxiliary_groups]
    material_usage = _material_usage(all_non_light_groups)

    materials = []
    for material in sorted(bpy.data.materials, key=lambda item: item.name.casefold()):
        usage = material_usage.get(material.name, {})
        if not usage:
            continue
        materials.append(
            {
                "name": material.name,
                "usage": {
                    "object_count": sum(usage.values()),
                    "group_count": len(usage),
                    "groups": [
                        {"group": group_id, "instance_count": count}
                        for group_id, count in sorted(usage.items())
                    ],
                },
                "shader": _principled_summary(material),
                "current_skate": {
                    "audio_surface": material.get("ow_audio_surface"),
                    "physics_surface": material.get("ow_physics_surface"),
                    "surface_pattern": material.get("ow_surface_pattern"),
                    "collision_enabled": material.get("ow_collision_enabled"),
                },
            }
        )

    non_empty_meshes = sum(len(group.members) for group in mesh_groups)
    auxiliary_objects = sum(
        len(group.members) for group in auxiliary_groups
    )
    light_objects = sum(len(group.members) for group in light_groups)
    inventory = {
        "version": 2,
        "format": "grouped-agent-inventory",
        "blend_file": bpy.data.filepath,
        "scene": bpy.context.scene.name,
        "summary": {
            "objects": len(objects),
            "mesh_objects": sum(obj.type == "MESH" for obj in objects),
            "non_empty_mesh_objects": non_empty_meshes,
            "mesh_groups": len(mesh_groups),
            "auxiliary_objects": auxiliary_objects,
            "auxiliary_groups": len(auxiliary_groups),
            "lights": light_objects,
            "light_groups": len(light_groups),
            "agent_object_records": (
                len(mesh_groups)
                + len(auxiliary_groups)
                + len(light_groups)
            ),
            "collapsed_duplicate_instances": (
                len(objects)
                - len(mesh_groups)
                - len(auxiliary_groups)
                - len(light_groups)
            ),
            "materials": len(bpy.data.materials),
            "used_materials": len(materials),
        },
        "used_materials": [
            material["name"]
            for material in materials
            if material["usage"]["object_count"] > 0
        ],
        "materials": materials,
        "mesh_groups": [_group_entry(group) for group in mesh_groups],
        "auxiliary_groups": [
            _auxiliary_group_entry(group) for group in auxiliary_groups
        ],
        "existing_light_groups": [
            _light_group_entry(group) for group in light_groups
        ],
    }
    members = {
        "version": 1,
        "format": "script-only-group-membership",
        "blend_file": bpy.data.filepath,
        "mesh_groups": {
            group.group_id: [member.name for member in group.members]
            for group in mesh_groups
        },
        "auxiliary_groups": {
            group.group_id: [member.name for member in group.members]
            for group in auxiliary_groups
        },
        "light_groups": {
            group.group_id: [member.name for member in group.members]
            for group in light_groups
        },
    }
    return inventory, members


def main() -> None:
    args = _arguments()
    output = Path(args.output).resolve()
    members_output = (
        Path(args.members_output).resolve()
        if args.members_output
        else output.with_name("scene_inventory_members.json")
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    members_output.parent.mkdir(parents=True, exist_ok=True)
    inventory, members = build_inventory()
    if args.preview_directory:
        preview_directory = Path(args.preview_directory).resolve()
        inventory["scene_previews"] = render_scene_previews(
            preview_directory
        )
    output.write_text(
        json.dumps(
            inventory,
            ensure_ascii=False,
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )
    members_output.write_text(
        json.dumps(
            members,
            ensure_ascii=False,
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        "SK8_AUTO_MAP_INVENTORY",
        output,
        f"objects={inventory['summary']['objects']}",
        f"agent_records={inventory['summary']['agent_object_records']}",
        f"collapsed={inventory['summary']['collapsed_duplicate_instances']}",
        f"materials={inventory['summary']['materials']}",
        f"lights={inventory['summary']['lights']}",
        f"members={members_output}",
        f"previews={args.preview_directory or 'disabled'}",
    )


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(1)
