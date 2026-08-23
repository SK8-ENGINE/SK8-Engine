"""Prepare the extracted University scene for the owned-world exporter."""

from __future__ import annotations

import json
import math
from pathlib import Path
import sys

import bpy


GROUP_1 = "OW_GROUP_1_PRESENTATION_COLLISION"
GROUP_2 = "OW_GROUP_2_NO_PRESENTATION"
GROUP_3 = "OW_GROUP_3_NO_COLLISION"
GROUP_4 = "OW_GROUP_4_GRINDS"
GROUP_5 = "OW_GROUP_5_PATHING"
SPAWN = "OW_SPAWN"
NO_COLLISION_MARKERS = (
    "billboard",
    "cloud",
    "decal",
    "foliage",
    "leaf",
    "leaves",
    "particle",
    "reflection",
    "shadow",
    "shrub",
    "sky",
    "tree",
    "water",
)
# Observatory skatepath beside the large brass orbital sculpture.  The height
# target prevents the ray cast from selecting terrain below the dam.
SPAWN_RUNTIME_XZ = (-430.0, -880.0)
SPAWN_TARGET_HEIGHT = 223.0
SOLID_COLOR_TEXTURES = {
    # The retail resource is an intentional 16x16 opaque black swatch.
    # Store it as a material constant so the general exporter can continue
    # rejecting accidentally blank texture decodes.
    "0x0000119903e3870a": (0.0, 0.0, 0.0),
}


def _new_group(name: str) -> bpy.types.Collection:
    existing = bpy.data.collections.get(name)
    if existing is not None:
        bpy.data.collections.remove(existing)
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _default_material() -> bpy.types.Material:
    material = bpy.data.materials.get("MAT_UNIVERSITY_FALLBACK")
    if material is None:
        material = bpy.data.materials.new("MAT_UNIVERSITY_FALLBACK")
        material.diffuse_color = (0.62, 0.62, 0.62, 1.0)
    return material


def _configure_material(material: bpy.types.Material) -> None:
    texture_id = str(material.get("skate3_texture_id", ""))
    solid_color = SOLID_COLOR_TEXTURES.get(texture_id)
    image = (
        bpy.data.images.get(texture_id)
        if texture_id and solid_color is None
        else None
    )
    material["ow_flags"] = 1
    material["ow_friction"] = 0.82
    material["ow_restitution"] = 0.0
    material["ow_display_color"] = (
        solid_color
        if solid_color is not None
        else tuple(material.diffuse_color[:3])
    )
    material["ow_roughness"] = 0.68
    material["ow_emissive"] = 0.0
    material["ow_baked_strength"] = 0.0
    material["ow_albedo_image"] = image.name if image is not None else ""
    material["ow_lightmap_image"] = ""
    material["ow_normal_image"] = ""
    material["ow_orm_image"] = ""
    material["ow_emissive_image"] = ""
    material["ow_alpha_mode"] = int(material.get("skate3_alpha_mode", 0))
    material["ow_alpha_cutoff"] = float(
        material.get("skate3_alpha_cutoff", 0.5)
    )
    material["ow_audio_surface"] = 3
    material["ow_physics_surface"] = 1
    material["ow_surface_pattern"] = 0
    material["ow_collision_enabled"] = True


def _ensure_export_uvs(mesh: bpy.types.Mesh) -> None:
    source = mesh.uv_layers.get("UVMap")
    if source is None:
        source = mesh.uv_layers.new(name="UVMap")
    lightmap = mesh.uv_layers.get("Lightmap")
    if lightmap is None:
        lightmap = mesh.uv_layers.new(name="Lightmap")
        for index in range(len(source.data)):
            lightmap.data[index].uv = source.data[index].uv


def _presentation_only(obj: bpy.types.Object) -> bool:
    identity = " ".join(
        (
            obj.name,
            str(obj.get("skate3_material_name", "")),
        )
    ).lower()
    return any(marker in identity for marker in NO_COLLISION_MARKERS)


def _surface_hits(
    x: float,
    runtime_z: float,
) -> list[tuple[float, float, str]]:
    scene = bpy.context.scene
    depsgraph = bpy.context.evaluated_depsgraph_get()
    origin = (x, -runtime_z, 400.0)
    hits: list[tuple[float, float, str]] = []
    for _ in range(64):
        hit, location, normal, _face, obj, _matrix = scene.ray_cast(
            depsgraph,
            origin,
            (0.0, 0.0, -1.0),
            distance=800.0,
        )
        if not hit:
            break
        hits.append((float(location.z), float(normal.z), obj.name))
        origin = (x, -runtime_z, float(location.z) - 0.02)
    return hits


def _create_spawn() -> tuple[float, float, float]:
    old = bpy.data.objects.get(SPAWN)
    if old is not None:
        bpy.data.objects.remove(old, do_unlink=True)
    x, runtime_z = SPAWN_RUNTIME_XZ
    candidates = [
        hit
        for hit in _surface_hits(x, runtime_z)
        if hit[1] >= 0.65
        and not any(marker in hit[2].lower() for marker in NO_COLLISION_MARKERS)
    ]
    if not candidates:
        raise RuntimeError(
            f"no upward University surface found at runtime XZ {(x, runtime_z)}"
        )
    surface_height, _normal_z, owner = min(
        candidates,
        key=lambda hit: abs(hit[0] - SPAWN_TARGET_HEIGHT),
    )
    vertices = [
        (-2.0, -2.0, 0.0),
        (2.0, -2.0, 0.0),
        (2.0, 2.0, 0.0),
        (-2.0, 2.0, 0.0),
        (0.0, -3.0, 0.0),
    ]
    mesh = bpy.data.meshes.new(f"{SPAWN}_MESH")
    mesh.from_pydata(vertices, [], [(0, 1, 2, 3), (0, 4, 1)])
    spawn = bpy.data.objects.new(SPAWN, mesh)
    bpy.context.scene.collection.objects.link(spawn)
    spawn.location = (x, -runtime_z, surface_height + 1.0)
    spawn.rotation_euler.z = math.radians(-90.0)
    spawn.hide_render = True
    spawn["university_surface_object"] = owner
    # Exporter converts Blender (x, y, z) to runtime (x, z, -y).
    return (x, surface_height + 1.0, runtime_z)


def main() -> int:
    arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    if len(arguments) != 1:
        raise SystemExit(
            "usage: blender --background DIST_University.blend --python "
            "prepare_university_owned.py -- OUTPUT_BLEND"
        )
    output = Path(arguments[0]).resolve()
    group_1 = _new_group(GROUP_1)
    _new_group(GROUP_2)
    group_3 = _new_group(GROUP_3)
    group_4 = _new_group(GROUP_4)
    _new_group(GROUP_5)
    fallback = _default_material()
    _configure_material(fallback)

    mesh_objects = sorted(
        (obj for obj in bpy.data.objects if obj.type == "MESH"),
        key=lambda obj: obj.name_full,
    )
    material_ids: set[int] = set()
    collidable = 0
    presentation_only = 0
    triangle_count = 0
    for obj in mesh_objects:
        if obj.name == SPAWN:
            continue
        _ensure_export_uvs(obj.data)
        if not obj.data.materials:
            obj.data.materials.append(fallback)
        for material in obj.data.materials:
            if material is not None and material.as_pointer() not in material_ids:
                material_ids.add(material.as_pointer())
                _configure_material(material)
        obj["ow_material"] = obj.data.materials[0].name
        triangle_count += len(obj.data.polygons)
        if _presentation_only(obj):
            group_3.objects.link(obj)
            presentation_only += 1
        else:
            group_1.objects.link(obj)
            collidable += 1

    grind_objects = sorted(
        (
            obj
            for obj in bpy.data.objects
            if obj.type == "CURVE"
            and bool(obj.get("skate3_retail_grind", False))
        ),
        key=lambda obj: obj.name_full,
    )
    grind_segments = 0
    for obj in grind_objects:
        group_4.objects.link(obj)
        grind_segments += int(obj["skate3_retail_grind_segment_count"])

    spawn_runtime = _create_spawn()
    scene = bpy.context.scene
    scene["ow_map_name"] = "University District"
    scene["ow_cycle_seconds"] = 0.0
    scene["ow_start_hour"] = 11.0
    scene["ow_end_hour"] = 18.0
    scene["ow_cycle_ping_pong"] = False
    scene["ow_sky_zenith"] = (0.10, 0.36, 0.75)
    scene["ow_sky_horizon"] = (0.64, 0.82, 1.0)
    scene["ow_sky_nadir"] = (0.18, 0.24, 0.30)
    scene["ow_day_ambient"] = 0.34
    scene["ow_night_ambient"] = 0.10
    scene["university_collision_source"] = (
        "full-detail presentation geometry; retail simulation RX2 decoder pending"
    )
    scene["university_grind_source"] = (
        "exact retail Pegasus tSplineData cubic segment payloads"
    )
    scene["university_spawn_runtime"] = spawn_runtime
    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(output))
    print(
        json.dumps(
            {
                "output": str(output),
                "mesh_objects": len(mesh_objects),
                "materials": len(material_ids),
                "triangles": triangle_count,
                "collidable_objects": collidable,
                "presentation_only_objects": presentation_only,
                "grind_rails": len(grind_objects),
                "grind_segments": grind_segments,
                "spawn_runtime": spawn_runtime,
                "collision_source": scene["university_collision_source"],
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
