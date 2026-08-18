"""Original Blender -> SKATE v8 exporter.

This module intentionally targets the narrow project-owned scene contract
documented beside it. It has no ArenaBuilder imports or runtime dependency.
"""

from __future__ import annotations

from array import array
from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import math
import os
import struct
import sys
import time
from typing import BinaryIO

import bpy
from mathutils import Vector
try:
    import numpy
except ImportError:
    numpy = None


MAGIC = b"SKATE08\0"
ENDIAN_MARKER = 0x12345678
VISUAL_COLLECTION = "OW_VISUAL"
COLLISION_COLLECTION = "OW_COLLISION"
GRIND_COLLECTION = "OW_GRIND"
NPC_PATH_COLLECTION = "OW_NPC_PATHS"
SPAWN_OBJECT = "OW_SPAWN"
CACHE_SCHEMA = 8
METADATA_FLOAT_COUNT = 49
METADATA_BYTE_COUNT = METADATA_FLOAT_COUNT * 4


@dataclass
class ExportMaterial:
    blender_material: bpy.types.Material
    material_id: int
    albedo_texture: int
    lightmap_texture: int
    normal_texture: int
    orm_texture: int
    emissive_texture: int


@dataclass
class ExportHingedDoor:
    name: str
    hinge_position: tuple[float, float, float]
    hinge_axis: tuple[float, float, float]
    closed_width_axis: tuple[float, float, float]
    closed_depth_axis: tuple[float, float, float]
    local_min: tuple[float, float, float]
    local_max: tuple[float, float, float]
    minimum_angle_radians: float
    maximum_angle_radians: float
    initial_angle_radians: float
    mass: float
    angular_damping: float
    return_spring_strength: float
    maximum_angular_speed: float
    contact_impulse_scale: float
    friction: float
    restitution: float
    surface_id: int
    vertices: list[tuple]
    indices: list[int]
    collision: list[tuple]


@dataclass
class ExportLocalLight:
    name: str
    light_type: int
    position: tuple[float, float, float]
    direction: tuple[float, float, float]
    color: tuple[float, float, float]
    intensity: float
    influence_radius: float
    source_radius: float
    spot_inner_cosine: float
    spot_outer_cosine: float


@dataclass
class SceneContentFingerprint:
    digest: str
    visual_vertices: int
    visual_indices: int
    collision_triangles: int
    grind_rails: int
    npc_routes: int
    hinged_doors: int
    local_lights: int


def _write_u32(stream: BinaryIO, value: int) -> None:
    stream.write(struct.pack("<I", value))


def _write_f32(stream: BinaryIO, value: float) -> None:
    if not math.isfinite(value):
        raise ValueError("SKATE does not permit non-finite floats")
    stream.write(struct.pack("<f", value))


def _write_vec(stream: BinaryIO, value) -> None:
    for component in value:
        _write_f32(stream, float(component))


def _write_string(stream: BinaryIO, value: str) -> None:
    encoded = value.encode("utf-8")
    _write_u32(stream, len(encoded))
    stream.write(encoded)


def _cache_manifest_path(output: Path) -> Path:
    return output.with_name(output.name + ".export-cache.json")


def _hash_text(digest, value: object) -> None:
    encoded = str(value).encode("utf-8")
    digest.update(struct.pack("<I", len(encoded)))
    digest.update(encoded)


def _hash_floats(digest, values) -> None:
    for value in values:
        digest.update(struct.pack("<f", float(value)))


def _hash_foreach(
    digest,
    collection,
    property_name: str,
    component_count: int,
    typecode: str,
) -> None:
    count = len(collection) * component_count
    if typecode == "f":
        values = array("f", [0.0]) * count
    else:
        values = array(typecode, [0]) * count
    if count:
        collection.foreach_get(property_name, values)
    if sys.byteorder != "little":
        values.byteswap()
    digest.update(values.tobytes())


def _hash_matrix(digest, matrix) -> None:
    for row in matrix:
        _hash_floats(digest, row)


def _hash_image_source(digest, image: bpy.types.Image) -> None:
    _hash_text(digest, image.name)
    _hash_text(digest, image.source)
    _hash_text(digest, image.colorspace_settings.name)
    digest.update(struct.pack("<II", int(image.size[0]), int(image.size[1])))
    raw_path = image.filepath_raw or image.filepath
    resolved = Path(bpy.path.abspath(raw_path)).resolve() if raw_path else None
    if resolved is not None and resolved.is_file() and image.packed_file is None:
        stat = resolved.stat()
        _hash_text(digest, os.path.normcase(str(resolved)))
        digest.update(struct.pack("<QQ", stat.st_size, stat.st_mtime_ns))
        return

    # Generated, baked, and packed images have no trustworthy external file
    # fingerprint. Hash their actual pixels so cache reuse remains correct.
    expected = int(image.size[0]) * int(image.size[1]) * 4
    values = array("f", [0.0]) * expected
    if expected:
        image.pixels.foreach_get(values)
    if sys.byteorder != "little":
        values.byteswap()
    digest.update(values.tobytes())


def _hash_mesh(
    digest,
    source_object: bpy.types.Object,
    *,
    visual: bool,
    depsgraph,
) -> tuple[int, int]:
    evaluated = source_object.evaluated_get(depsgraph)
    mesh = (
        evaluated.to_mesh(
            preserve_all_data_layers=True, depsgraph=depsgraph
        )
        if visual
        else evaluated.to_mesh()
    )
    try:
        mesh.calc_loop_triangles()
        _hash_text(digest, source_object.name_full)
        _hash_matrix(digest, source_object.matrix_world)
        digest.update(
            struct.pack(
                "<IIII",
                len(mesh.vertices),
                len(mesh.loops),
                len(mesh.polygons),
                len(mesh.loop_triangles),
            )
        )
        _hash_foreach(digest, mesh.vertices, "co", 3, "f")
        _hash_foreach(digest, mesh.loops, "vertex_index", 1, "I")
        _hash_foreach(digest, mesh.polygons, "loop_start", 1, "I")
        _hash_foreach(digest, mesh.polygons, "loop_total", 1, "I")
        _hash_foreach(digest, mesh.polygons, "material_index", 1, "I")
        for material in mesh.materials:
            _hash_text(digest, material.name if material else "")

        if visual:
            for property_name in (
                "ow_export_visual",
                "ow_physics_type",
                "ow_hinge_position",
                "ow_hinge_axis",
                "ow_door_min_angle_degrees",
                "ow_door_max_angle_degrees",
                "ow_door_initial_angle_degrees",
                "ow_door_mass",
                "ow_door_angular_damping",
                "ow_door_return_spring_strength",
                "ow_door_maximum_angular_speed",
                "ow_door_contact_impulse_scale",
                "ow_door_friction",
                "ow_door_restitution",
                "ow_door_collision_material",
            ):
                _hash_text(
                    digest,
                    repr(source_object.get(property_name, None)),
                )
            uv0 = mesh.uv_layers.get("UVMap")
            uv1 = mesh.uv_layers.get("Lightmap")
            if uv0 is None or uv1 is None:
                raise ValueError(
                    f"visual mesh {source_object.name!r} requires UVMap and "
                    "Lightmap UV layers"
                )
            _hash_foreach(digest, mesh.loops, "normal", 3, "f")
            _hash_foreach(digest, uv0.data, "uv", 2, "f")
            _hash_foreach(digest, uv1.data, "uv", 2, "f")
        else:
            _hash_text(digest, source_object.get("ow_material", ""))
            _hash_text(
                digest,
                int(bool(source_object.get("ow_upward_surface", False))),
            )
        triangle_count = len(mesh.loop_triangles)
        return triangle_count * 3, triangle_count
    finally:
        evaluated.to_mesh_clear()


def _scene_content_fingerprint(
    visual_objects: list[bpy.types.Object],
    collision_objects: list[bpy.types.Object],
    grind_objects: list[bpy.types.Object],
    npc_path_objects: list[bpy.types.Object],
    materials: list[bpy.types.Material],
    images: list[bpy.types.Image],
) -> SceneContentFingerprint:
    started = time.perf_counter()
    print(
        "SKATE cache: fingerprinting scene content",
        f"visual_objects={len(visual_objects)}",
        f"collision_objects={len(collision_objects)}",
        flush=True,
    )
    digest = hashlib.sha256()
    digest.update(f"SKATE_EXPORT_CACHE_{CACHE_SCHEMA}".encode("ascii"))

    material_properties = (
        "ow_flags",
        "ow_friction",
        "ow_restitution",
        "ow_display_color",
        "ow_roughness",
        "ow_emissive",
        "ow_albedo_image",
        "ow_lightmap_image",
        "ow_baked_strength",
        "ow_normal_image",
        "ow_orm_image",
        "ow_emissive_image",
        "ow_alpha_mode",
        "ow_alpha_cutoff",
        "ow_audio_surface",
        "ow_physics_surface",
        "ow_surface_pattern",
        "ow_collision_enabled",
    )
    for material in materials:
        _hash_text(digest, material.name)
        _hash_floats(digest, material.diffuse_color)
        for property_name in material_properties:
            _hash_text(digest, property_name)
            _hash_text(digest, repr(material.get(property_name, None)))
    for image in images:
        _hash_image_source(digest, image)

    depsgraph = bpy.context.evaluated_depsgraph_get()
    visual_vertices = 0
    visual_indices = 0
    hinged_doors = 0
    for obj in visual_objects:
        if obj.type != "MESH":
            continue
        object_vertices, _ = _hash_mesh(
            digest, obj, visual=True, depsgraph=depsgraph
        )
        if str(obj.get("ow_physics_type", "STATIC")) == "HINGED_DOOR":
            hinged_doors += 1
        else:
            visual_vertices += object_vertices
            visual_indices += object_vertices

    collision_triangles = 0
    for obj in collision_objects:
        if obj.type != "MESH":
            continue
        material = bpy.data.materials.get(str(obj.get("ow_material", "")))
        if material is not None and not bool(
            material.get("ow_collision_enabled", True)
        ):
            continue
        _, object_triangles = _hash_mesh(
            digest, obj, visual=False, depsgraph=depsgraph
        )
        collision_triangles += object_triangles

    grind_rails = 0
    for obj in grind_objects:
        if obj.type != "CURVE":
            continue
        _hash_text(digest, obj.name_full)
        _hash_matrix(digest, obj.matrix_world)
        for spline in obj.data.splines:
            _hash_text(digest, spline.type)
            _hash_text(digest, int(bool(spline.use_cyclic_u)))
            if spline.type == "POLY":
                _hash_foreach(digest, spline.points, "co", 4, "f")
                if len(spline.points) >= 2:
                    grind_rails += 1
            elif spline.type == "BEZIER":
                _hash_foreach(digest, spline.bezier_points, "co", 3, "f")
                if len(spline.bezier_points) >= 2:
                    grind_rails += 1

    npc_routes = 0
    for obj in npc_path_objects:
        if obj.type != "CURVE":
            continue
        _hash_text(digest, obj.name_full)
        _hash_matrix(digest, obj.matrix_world)
        for property_name, default in (
            ("ow_npc_skater_count", 1),
            ("ow_npc_speed", 5.5),
            ("ow_npc_spawn_spacing", 3.0),
        ):
            _hash_text(digest, repr(obj.get(property_name, default)))
        for spline in obj.data.splines:
            _hash_text(digest, spline.type)
            _hash_text(digest, int(bool(spline.use_cyclic_u)))
            if spline.type == "POLY":
                _hash_foreach(digest, spline.points, "co", 4, "f")
                if len(spline.points) >= 2:
                    npc_routes += 1
            elif spline.type == "BEZIER":
                _hash_foreach(digest, spline.bezier_points, "co", 3, "f")
                if len(spline.bezier_points) >= 2:
                    npc_routes += 1

    local_lights = 0
    for obj in _visible_local_light_objects():
        light = obj.data
        local_lights += 1
        _hash_text(digest, obj.name_full)
        _hash_matrix(digest, obj.matrix_world)
        for value in (
            light.type,
            tuple(light.color),
            light.energy,
            light.cutoff_distance,
            light.shadow_soft_size,
            getattr(light, "size", 0.0),
            getattr(light, "size_y", 0.0),
            getattr(light, "shape", ""),
            getattr(light, "spot_size", 0.0),
            getattr(light, "spot_blend", 0.0),
        ):
            _hash_text(digest, repr(value))

    result = SceneContentFingerprint(
        digest=digest.hexdigest(),
        visual_vertices=visual_vertices,
        visual_indices=visual_indices,
        collision_triangles=collision_triangles,
        grind_rails=grind_rails,
        npc_routes=npc_routes,
        hinged_doors=hinged_doors,
        local_lights=local_lights,
    )
    print(
        "SKATE cache: fingerprint complete",
        f"seconds={time.perf_counter() - started:.3f}",
        f"digest={result.digest[:16]}",
        flush=True,
    )
    return result


def _visible_light_objects() -> list[bpy.types.Object]:
    return sorted(
        (
            obj
            for obj in bpy.data.objects
            if obj.type == "LIGHT"
            and not obj.hide_render
            and math.isfinite(float(obj.data.energy))
            and float(obj.data.energy) > 0.0
        ),
        key=lambda item: item.name_full,
    )


def _visible_local_light_objects() -> list[bpy.types.Object]:
    return [
        obj
        for obj in _visible_light_objects()
        if obj.data.type in {"POINT", "SPOT", "AREA"}
    ]


def _sun_metadata() -> tuple[tuple[float, float, float], float, float]:
    scene = bpy.context.scene
    sun_color = tuple(scene.get("ow_sun_color", (1.0, 0.92, 0.78)))
    sun_intensity = float(scene.get("ow_sun_intensity", 1.25))
    orbit_azimuth = float(scene.get("ow_orbit_azimuth", 0.62))
    suns = [
        obj for obj in _visible_light_objects() if obj.data.type == "SUN"
    ]
    if not suns:
        return sun_color, sun_intensity, orbit_azimuth

    sun = suns[0]
    emitted = sun.matrix_world.to_quaternion() @ Vector((0.0, 0.0, -1.0))
    runtime_emitted = _to_runtime(emitted)
    direction_to_light = (
        -runtime_emitted[0],
        -runtime_emitted[1],
        -runtime_emitted[2],
    )
    horizontal_length = math.hypot(
        direction_to_light[0], direction_to_light[2]
    )
    if horizontal_length > 1.0e-5:
        orbit_azimuth = math.atan2(
            direction_to_light[2], direction_to_light[0]
        )
    return (
        tuple(float(value) for value in sun.data.color),
        max(0.0, float(sun.data.energy) * 0.42),
        orbit_azimuth,
    )


def _read_package_header(output: Path) -> tuple[str, int, tuple[int, ...]]:
    with output.open("rb") as stream:
        if stream.read(len(MAGIC)) != MAGIC:
            raise ValueError(f"{output} is not an SKATE v8 package")
        marker = struct.unpack("<I", stream.read(4))[0]
        if marker != ENDIAN_MARKER:
            raise ValueError(f"{output} has an invalid endian marker")
        name_length = struct.unpack("<I", stream.read(4))[0]
        package_name = stream.read(name_length).decode("utf-8")
        metadata_offset = stream.tell()
        stream.seek(METADATA_BYTE_COUNT, os.SEEK_CUR)
        counts = struct.unpack("<9I", stream.read(36))
    return package_name, metadata_offset, counts


def _package_static_digest(output: Path, metadata_offset: int) -> str:
    digest = hashlib.sha256()
    with output.open("rb") as stream:
        remaining = metadata_offset
        while remaining:
            data = stream.read(min(8 * 1024 * 1024, remaining))
            if not data:
                raise ValueError(f"{output} ended before its metadata")
            digest.update(data)
            remaining -= len(data)
        stream.seek(METADATA_BYTE_COUNT, os.SEEK_CUR)
        while True:
            data = stream.read(8 * 1024 * 1024)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def _scene_metadata(output: Path) -> tuple[str, bytes]:
    spawn = bpy.data.objects.get(SPAWN_OBJECT)
    if spawn is None:
        raise ValueError(f"required spawn object is missing: {SPAWN_OBJECT}")
    scene = bpy.context.scene
    map_name = str(scene.get("ow_map_name", output.stem))
    sun_color, sun_intensity, orbit_azimuth = _sun_metadata()
    values = (
        *_to_runtime(spawn.matrix_world.translation),
        float(spawn.get("ow_heading_radians", 0.0)),
        *tuple(scene.get("ow_sky_zenith", (0.09, 0.34, 0.72))),
        *tuple(scene.get("ow_sky_horizon", (0.58, 0.78, 0.98))),
        *tuple(scene.get("ow_sky_nadir", (0.18, 0.25, 0.34))),
        float(scene.get("ow_cycle_seconds", 96.0)),
        float(scene.get("ow_start_hour", 9.0)),
        orbit_azimuth,
        float(scene.get("ow_end_hour", 17.0)),
        1.0 if bool(scene.get("ow_cycle_ping_pong", False)) else 0.0,
        *tuple(scene.get("ow_twilight_zenith", (0.045, 0.10, 0.26))),
        *tuple(scene.get("ow_twilight_horizon", (1.0, 0.32, 0.10))),
        *tuple(scene.get("ow_twilight_nadir", (0.05, 0.035, 0.06))),
        *tuple(scene.get("ow_night_zenith", (0.007, 0.015, 0.045))),
        *tuple(scene.get("ow_night_horizon", (0.045, 0.085, 0.17))),
        *tuple(scene.get("ow_night_nadir", (0.008, 0.014, 0.032))),
        *sun_color,
        *tuple(scene.get("ow_moon_color", (0.42, 0.56, 0.92))),
        sun_intensity,
        float(scene.get("ow_moon_intensity", 0.18)),
        float(scene.get("ow_day_ambient", 0.32)),
        float(scene.get("ow_night_ambient", 0.11)),
        *tuple(scene.get("ow_sky_tint", (1.0, 1.0, 1.0))),
    )
    if len(values) != METADATA_FLOAT_COUNT or not all(
        math.isfinite(float(value)) for value in values
    ):
        raise ValueError(
            f"SKATE metadata must contain {METADATA_FLOAT_COUNT} finite floats"
        )
    return map_name, struct.pack("<49f", *(float(value) for value in values))


def _patch_package_metadata(output: Path) -> None:
    map_name, metadata = _scene_metadata(output)
    package_name, metadata_offset, _ = _read_package_header(output)
    if package_name != map_name:
        raise ValueError(
            "metadata-only export cannot change the map name; use a full "
            f"export ({package_name!r} != {map_name!r})"
        )
    with output.open("r+b") as stream:
        stream.seek(metadata_offset)
        stream.write(metadata)
        stream.flush()
        os.fsync(stream.fileno())


def _load_cache_manifest(output: Path) -> dict | None:
    path = _cache_manifest_path(output)
    if not path.is_file():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def _write_cache_manifest(
    output: Path,
    fingerprint: SceneContentFingerprint,
    material_count: int,
    texture_count: int,
) -> None:
    package_name, metadata_offset, counts = _read_package_header(output)
    output_stat = output.stat()
    manifest = {
        "schema": CACHE_SCHEMA,
        "package_name": package_name,
        "output_length": output_stat.st_size,
        "output_mtime_ns": output_stat.st_mtime_ns,
        "static_sha256": _package_static_digest(output, metadata_offset),
        "content_sha256": fingerprint.digest,
        "material_count": material_count,
        "texture_count": texture_count,
        "visual_vertex_count": fingerprint.visual_vertices,
        "visual_index_count": fingerprint.visual_indices,
        "collision_triangle_count": fingerprint.collision_triangles,
        "grind_rail_count": fingerprint.grind_rails,
        "npc_route_count": fingerprint.npc_routes,
        "hinged_door_count": fingerprint.hinged_doors,
        "local_light_count": fingerprint.local_lights,
        "package_counts": list(counts),
    }
    path = _cache_manifest_path(output)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def _refresh_manifest_file_state(output: Path, manifest: dict) -> None:
    output_stat = output.stat()
    manifest["output_length"] = output_stat.st_size
    manifest["output_mtime_ns"] = output_stat.st_mtime_ns
    path = _cache_manifest_path(output)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def _manifest_matches_package(output: Path, manifest: dict) -> bool:
    if (
        manifest.get("schema") != CACHE_SCHEMA
        or not output.is_file()
        or output.stat().st_size != manifest.get("output_length")
    ):
        return False
    try:
        package_name, metadata_offset, counts = _read_package_header(output)
    except (OSError, UnicodeError, ValueError, struct.error):
        return False
    if (
        package_name != manifest.get("package_name")
        or list(counts) != manifest.get("package_counts")
    ):
        return False
    # The exporter records file identity after every metadata patch. An
    # unchanged size/mtime pair means the already-verified static payload has
    # not been touched, so rereading a 1.2 GiB package would add no value.
    if output.stat().st_mtime_ns == manifest.get("output_mtime_ns"):
        return True
    return (
        _package_static_digest(output, metadata_offset)
        == manifest.get("static_sha256")
    )


def _to_runtime(value) -> tuple[float, float, float]:
    return float(value.x), float(value.z), float(-value.y)


def _export_local_lights() -> list[ExportLocalLight]:
    result: list[ExportLocalLight] = []
    type_ids = {"POINT": 0, "SPOT": 1, "AREA": 2}
    for obj in _visible_local_light_objects():
        light = obj.data
        forward = obj.matrix_world.to_quaternion() @ Vector((0.0, 0.0, -1.0))
        runtime_forward = Vector(_to_runtime(forward))
        if runtime_forward.length <= 1.0e-6:
            raise ValueError(f"light {obj.name!r} has an invalid direction")
        runtime_forward.normalize()

        if light.type == "AREA":
            source_radius = 0.5 * max(
                float(getattr(light, "size", 0.0)),
                float(getattr(light, "size_y", 0.0)),
                0.02,
            )
        else:
            source_radius = max(float(light.shadow_soft_size), 0.01)
        influence_radius = max(
            float(light.cutoff_distance), source_radius + 0.01
        )

        inner_cosine = 1.0
        outer_cosine = 1.0
        if light.type == "SPOT":
            outer_half_angle = max(
                0.001, min(math.pi * 0.5, float(light.spot_size) * 0.5)
            )
            inner_half_angle = outer_half_angle * (
                1.0 - max(0.0, min(1.0, float(light.spot_blend)))
            )
            inner_cosine = math.cos(inner_half_angle)
            outer_cosine = math.cos(outer_half_angle)

        result.append(
            ExportLocalLight(
                name=obj.name_full,
                light_type=type_ids[light.type],
                position=_to_runtime(obj.matrix_world.translation),
                direction=tuple(float(value) for value in runtime_forward),
                color=tuple(float(value) for value in light.color),
                # Blender's local lights use Watts. This maps the familiar
                # default 1000 W light to the renderer's established 10-unit
                # local-light scale.
                intensity=max(0.001, float(light.energy) * 0.01),
                influence_radius=influence_radius,
                source_radius=source_radius,
                spot_inner_cosine=inner_cosine,
                spot_outer_cosine=outer_cosine,
            )
        )
    return result


def _image_rgba8(
    image: bpy.types.Image, *, lightmap: bool = False
) -> bytes:
    expected = image.size[0] * image.size[1] * 4
    if numpy is not None:
        # Blender bundles NumPy and foreach_get writes directly into its
        # contiguous storage. Vectorizing clamp/transfer/quantization avoids
        # billions of Python scalar operations on large imported maps.
        values = numpy.empty(expected, dtype=numpy.float32)
        image.pixels.foreach_get(values)
        values = numpy.nan_to_num(
            values, nan=0.0, posinf=1.0, neginf=0.0, copy=False
        )
        pixels = values.reshape((-1, 4))
        if lightmap:
            pixels[:, :3] = numpy.sqrt(
                numpy.clip(pixels[:, :3], 0.0, 4.0) * 0.25
            )
            pixels[:, 3] = numpy.clip(pixels[:, 3], 0.0, 1.0)
        else:
            numpy.clip(pixels, 0.0, 1.0, out=pixels)
        # Python round() and numpy.rint() both use ties-to-even, preserving
        # the byte-exact exporter contract.
        return numpy.rint(pixels * 255.0).astype(numpy.uint8).tobytes()
    values = array("f", [0.0]) * expected
    image.pixels.foreach_get(values)
    result = bytearray(expected)
    for index, value in enumerate(values):
        channel = index & 3
        linear = max(0.0, float(value))
        if lightmap and channel != 3:
            # SKATE v1 lightmaps use sqrt(linear / 4). This preserves dark
            # indirect energy that direct linear UNORM8 quantization erased,
            # while retaining headroom up to 4.0 for bright colour bounce.
            encoded = math.sqrt(min(linear, 4.0) * 0.25)
        else:
            encoded = min(linear, 1.0)
        result[index] = max(0, min(255, round(encoded * 255.0)))
    return bytes(result)


def _require_nonblank_rgb(name: str, rgba8: bytes) -> None:
    if not any(
        rgba8[index] or rgba8[index + 1] or rgba8[index + 2]
        for index in range(0, len(rgba8), 4)
    ):
        raise ValueError(
            f"referenced texture {name!r} contains no RGB data; refusing "
            "to export a black SKATE package"
        )


def _collection(name: str) -> bpy.types.Collection:
    collection = bpy.data.collections.get(name)
    if collection is None:
        raise ValueError(f"required Blender collection is missing: {name}")
    return collection


def _used_visual_materials(
    visual_objects: list[bpy.types.Object],
) -> list[bpy.types.Material]:
    result: list[bpy.types.Material] = []
    seen: set[int] = set()
    for obj in visual_objects:
        if obj.type != "MESH":
            continue
        for slot in obj.material_slots:
            material = slot.material
            if material is None:
                continue
            identity = material.as_pointer()
            if identity not in seen:
                result.append(material)
                seen.add(identity)
    if not result:
        raise ValueError("OW_VISUAL does not reference any materials")
    return result


def _referenced_images(
    materials: list[bpy.types.Material],
) -> tuple[list[bpy.types.Image], dict[int, int]]:
    images: list[bpy.types.Image] = []
    ids: dict[int, int] = {}
    for material in materials:
        for property_name in (
            "ow_albedo_image",
            "ow_lightmap_image",
            "ow_normal_image",
            "ow_orm_image",
            "ow_emissive_image",
        ):
            image_name = str(material.get(property_name, ""))
            if not image_name:
                continue
            image = bpy.data.images.get(image_name)
            if image is None:
                raise ValueError(
                    f"material {material.name!r} references missing image "
                    f"{image_name!r}"
                )
            identity = image.as_pointer()
            if identity not in ids:
                ids[identity] = len(images) + 1
                images.append(image)
    return images, ids


def _texture_id(
    material: bpy.types.Material,
    property_name: str,
    image_ids: dict[int, int],
) -> int:
    image = bpy.data.images.get(str(material.get(property_name, "")))
    return (
        image_ids.get(image.as_pointer(), 0)
        if image is not None
        else 0
    )


def _bounded_int(
    material: bpy.types.Material,
    property_name: str,
    default: int,
    maximum: int,
) -> int:
    value = int(material.get(property_name, default))
    if value < 0 or value > maximum:
        raise ValueError(
            f"material {material.name!r} has invalid {property_name}={value}"
        )
    return value


def _material_color(material: bpy.types.Material) -> tuple[float, float, float]:
    authored = material.get("ow_display_color")
    if authored is not None and len(authored) >= 3:
        return tuple(float(authored[index]) for index in range(3))
    return tuple(float(material.diffuse_color[index]) for index in range(3))


def _export_visual_geometry(
    visual_objects: list[bpy.types.Object],
    material_name_ids: dict[str, int],
) -> tuple[list[tuple], list[int]]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    vertices: list[tuple] = []
    indices: list[int] = []
    for source_object in visual_objects:
        if source_object.type != "MESH":
            continue
        evaluated = source_object.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh(
            preserve_all_data_layers=True, depsgraph=depsgraph
        )
        try:
            mesh.calc_loop_triangles()
            uv0 = mesh.uv_layers.get("UVMap")
            uv1 = mesh.uv_layers.get("Lightmap")
            if uv0 is None or uv1 is None:
                raise ValueError(
                    f"visual mesh {source_object.name!r} requires UVMap and "
                    "Lightmap UV layers"
                )
            normal_matrix = source_object.matrix_world.to_3x3().inverted().transposed()
            for triangle in mesh.loop_triangles:
                polygon = mesh.polygons[triangle.polygon_index]
                if polygon.material_index >= len(mesh.materials):
                    raise ValueError(
                        f"mesh {source_object.name!r} has an invalid material slot"
                    )
                material = mesh.materials[polygon.material_index]
                if material is None or material.name not in material_name_ids:
                    raise ValueError(
                        f"mesh {source_object.name!r} has an unexported material"
                    )
                material_id = material_name_ids[material.name]
                for loop_index in triangle.loops:
                    loop = mesh.loops[loop_index]
                    point = source_object.matrix_world @ mesh.vertices[loop.vertex_index].co
                    normal = (normal_matrix @ loop.normal).normalized()
                    base_uv = uv0.data[loop_index].uv
                    light_uv = uv1.data[loop_index].uv
                    vertices.append(
                        (
                            _to_runtime(point),
                            _to_runtime(normal),
                            (float(base_uv.x), float(base_uv.y)),
                            (float(light_uv.x), float(light_uv.y)),
                            material_id,
                        )
                    )
                    indices.append(len(vertices) - 1)
        finally:
            evaluated.to_mesh_clear()
    if not vertices:
        raise ValueError("OW_VISUAL contains no exportable triangles")
    return vertices, indices


def _export_collision(
    collision_objects: list[bpy.types.Object],
    material_name_ids: dict[str, int],
) -> list[tuple]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    triangles: list[tuple] = []
    triangle_owners: dict[tuple, str] = {}
    surface_id = 1
    for source_object in collision_objects:
        if source_object.type != "MESH":
            continue
        material_name = str(source_object.get("ow_material", ""))
        if material_name not in material_name_ids:
            raise ValueError(
                f"collision object {source_object.name!r} references unknown "
                f"material {material_name!r}"
            )
        blender_material = bpy.data.materials.get(material_name)
        if (
            blender_material is not None
            and not bool(
                blender_material.get("ow_collision_enabled", True)
            )
        ):
            continue
        material_id = material_name_ids[material_name]
        evaluated = source_object.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        try:
            mesh.calc_loop_triangles()
            for triangle in mesh.loop_triangles:
                points = [
                    source_object.matrix_world @ mesh.vertices[index].co
                    for index in triangle.vertices
                ]
                cross = (points[1] - points[0]).cross(
                    points[2] - points[0]
                )
                # Keep this exactly aligned with the C++ SKATE loader. Very
                # small source triangles can survive Blender's mesh cleanup
                # but collapse after float32 package serialization.
                if cross.length_squared <= 1.0e-10:
                    raise ValueError(
                        f"collision object {source_object.name!r} contains "
                        "a degenerate triangle"
                    )
                if (
                    bool(source_object.get("ow_upward_surface", False))
                    and cross.z <= 1.0e-6
                ):
                    raise ValueError(
                        f"rideable collision object {source_object.name!r} "
                        "contains a downward or vertical triangle"
                    )
                # Position-only, orientation-independent key catches both
                # exact duplicates and opposite-wound copies. Those surfaces
                # create contradictory native contacts and broken adjacency.
                key = tuple(
                    sorted(
                        tuple(round(float(component), 6) for component in point)
                        for point in points
                    )
                )
                if key in triangle_owners:
                    raise ValueError(
                        f"collision triangle in {source_object.name!r} "
                        f"duplicates geometry from {triangle_owners[key]!r}"
                    )
                triangle_owners[key] = source_object.name
                triangles.append(
                    (
                        _to_runtime(points[0]),
                        _to_runtime(points[1]),
                        _to_runtime(points[2]),
                        surface_id,
                        material_id,
                    )
                )
        finally:
            evaluated.to_mesh_clear()
        surface_id += 1
    if not triangles:
        raise ValueError("OW_COLLISION contains no exportable triangles")
    return triangles


def _export_grinds(grind_objects: list[bpy.types.Object]) -> list[tuple]:
    rails: list[tuple] = []
    for obj in grind_objects:
        if obj.type != "CURVE":
            continue
        for spline_index, spline in enumerate(obj.data.splines):
            points: list[tuple[float, float, float]] = []
            if spline.type == "POLY":
                points = [
                    _to_runtime(obj.matrix_world @ Vector(point.co[:3]))
                    for point in spline.points
                ]
            elif spline.type == "BEZIER":
                points = [
                    _to_runtime(obj.matrix_world @ point.co)
                    for point in spline.bezier_points
                ]
            if len(points) < 2:
                continue
            name = obj.name if len(obj.data.splines) == 1 else (
                f"{obj.name}_{spline_index}"
            )
            rails.append((name, bool(spline.use_cyclic_u), points))
    return rails


def _export_npc_routes(
    npc_path_objects: list[bpy.types.Object],
) -> list[tuple]:
    routes: list[tuple] = []
    for obj in npc_path_objects:
        if obj.type != "CURVE":
            continue
        skater_count = int(obj.get("ow_npc_skater_count", 1))
        speed = float(obj.get("ow_npc_speed", 5.5))
        spawn_spacing = float(obj.get("ow_npc_spawn_spacing", 3.0))
        if not 1 <= skater_count <= 32:
            raise ValueError(
                f"NPC path {obj.name!r} skater count must be 1-32"
            )
        if not math.isfinite(speed) or not 0.0 < speed <= 30.0:
            raise ValueError(
                f"NPC path {obj.name!r} speed must be above 0 and at most 30"
            )
        if (
            not math.isfinite(spawn_spacing)
            or not 0.0 <= spawn_spacing <= 100.0
        ):
            raise ValueError(
                f"NPC path {obj.name!r} spacing must be 0-100 metres"
            )
        for spline_index, spline in enumerate(obj.data.splines):
            points: list[tuple[float, float, float]] = []
            if spline.type == "POLY":
                points = [
                    _to_runtime(obj.matrix_world @ Vector(point.co[:3]))
                    for point in spline.points
                ]
            elif spline.type == "BEZIER":
                points = [
                    _to_runtime(obj.matrix_world @ point.co)
                    for point in spline.bezier_points
                ]
            if len(points) < 2:
                continue
            name = obj.name if len(obj.data.splines) == 1 else (
                f"{obj.name}_{spline_index}"
            )
            routes.append(
                (
                    name,
                    bool(spline.use_cyclic_u),
                    skater_count,
                    speed,
                    spawn_spacing,
                    points,
                )
            )
    return routes


def _door_objects(
    visual_objects: list[bpy.types.Object],
) -> list[bpy.types.Object]:
    return [
        obj
        for obj in visual_objects
        if obj.type == "MESH"
        and str(obj.get("ow_physics_type", "STATIC")) == "HINGED_DOOR"
    ]


def _static_visual_objects(
    visual_objects: list[bpy.types.Object],
) -> list[bpy.types.Object]:
    door_ids = {obj.as_pointer() for obj in _door_objects(visual_objects)}
    return [
        obj
        for obj in visual_objects
        if obj.as_pointer() not in door_ids
    ]


def _normalized_vector(value, label: str) -> Vector:
    vector = Vector(tuple(float(component) for component in value))
    if vector.length <= 1.0e-6:
        raise ValueError(f"{label} must be a non-zero vector")
    return vector.normalized()


def _door_box_collision(
    minimum: Vector,
    maximum: Vector,
    surface_id: int,
    material_id: int,
) -> list[tuple]:
    corners = [
        (
            maximum.x if index & 1 else minimum.x,
            maximum.y if index & 2 else minimum.y,
            maximum.z if index & 4 else minimum.z,
        )
        for index in range(8)
    ]
    faces = (
        (0, 2, 3), (0, 3, 1),
        (4, 5, 7), (4, 7, 6),
        (0, 4, 6), (0, 6, 2),
        (1, 3, 7), (1, 7, 5),
        (0, 1, 5), (0, 5, 4),
        (2, 6, 7), (2, 7, 3),
    )
    return [
        (
            corners[a],
            corners[b],
            corners[c],
            surface_id,
            material_id,
        )
        for a, b, c in faces
    ]


def _export_hinged_doors(
    visual_objects: list[bpy.types.Object],
    material_name_ids: dict[str, int],
) -> list[ExportHingedDoor]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    result: list[ExportHingedDoor] = []
    for source_object in _door_objects(visual_objects):
        evaluated = source_object.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh(
            preserve_all_data_layers=True, depsgraph=depsgraph
        )
        try:
            mesh.calc_loop_triangles()
            uv0 = mesh.uv_layers.get("UVMap")
            uv1 = mesh.uv_layers.get("Lightmap")
            if uv0 is None or uv1 is None:
                raise ValueError(
                    f"hinged door {source_object.name!r} requires UVMap "
                    "and Lightmap UV layers"
                )
            if not mesh.loop_triangles:
                raise ValueError(
                    f"hinged door {source_object.name!r} has no triangles"
                )

            hinge_source = source_object.get("ow_hinge_position")
            hinge = (
                Vector(tuple(float(value) for value in hinge_source))
                if hinge_source is not None
                else source_object.matrix_world.translation.copy()
            )
            axis = _normalized_vector(
                source_object.get("ow_hinge_axis", (0.0, 0.0, 1.0)),
                f"hinged door {source_object.name!r} axis",
            )
            world_corners = [
                source_object.matrix_world @ Vector(corner)
                for corner in source_object.bound_box
            ]
            center = sum(world_corners, Vector()) / len(world_corners)
            width = center - hinge
            width -= axis * width.dot(axis)
            if width.length <= 0.05:
                raise ValueError(
                    f"hinged door {source_object.name!r} origin/hinge must "
                    "lie on one side of the door leaf"
                )
            width.normalize()
            depth = width.cross(axis).normalized()
            # Re-orthogonalize width after the cross product so slightly
            # imprecise authored axes cannot introduce render/collision drift.
            width = axis.cross(depth).normalized()

            def local_point(point: Vector) -> Vector:
                delta = point - hinge
                return Vector(
                    (delta.dot(width), delta.dot(axis), delta.dot(depth))
                )

            def local_direction(direction: Vector) -> Vector:
                return Vector(
                    (
                        direction.dot(width),
                        direction.dot(axis),
                        direction.dot(depth),
                    )
                )

            normal_matrix = (
                source_object.matrix_world.to_3x3().inverted().transposed()
            )
            vertices: list[tuple] = []
            indices: list[int] = []
            local_points: list[Vector] = []
            for triangle in mesh.loop_triangles:
                polygon = mesh.polygons[triangle.polygon_index]
                if polygon.material_index >= len(mesh.materials):
                    raise ValueError(
                        f"hinged door {source_object.name!r} has an invalid "
                        "material slot"
                    )
                material = mesh.materials[polygon.material_index]
                if material is None or material.name not in material_name_ids:
                    raise ValueError(
                        f"hinged door {source_object.name!r} has an "
                        "unexported material"
                    )
                material_id = material_name_ids[material.name]
                for loop_index in triangle.loops:
                    loop = mesh.loops[loop_index]
                    world_point = (
                        source_object.matrix_world
                        @ mesh.vertices[loop.vertex_index].co
                    )
                    point = local_point(world_point)
                    world_normal = (
                        normal_matrix @ loop.normal
                    ).normalized()
                    normal = local_direction(world_normal).normalized()
                    base_uv = uv0.data[loop_index].uv
                    light_uv = uv1.data[loop_index].uv
                    local_points.append(point)
                    vertices.append(
                        (
                            tuple(float(value) for value in point),
                            tuple(float(value) for value in normal),
                            (float(base_uv.x), float(base_uv.y)),
                            (float(light_uv.x), float(light_uv.y)),
                            material_id,
                        )
                    )
                    indices.append(len(vertices) - 1)

            minimum = Vector(
                tuple(min(point[axis_index] for point in local_points)
                      for axis_index in range(3))
            )
            maximum = Vector(
                tuple(max(point[axis_index] for point in local_points)
                      for axis_index in range(3))
            )
            # Very thin visual planes still need a stable physical leaf.
            if maximum.z - minimum.z < 0.08:
                middle = (minimum.z + maximum.z) * 0.5
                minimum.z = middle - 0.04
                maximum.z = middle + 0.04

            material_name = str(
                source_object.get("ow_door_collision_material", "")
            )
            if not material_name and len(mesh.materials):
                material = mesh.materials[0]
                material_name = material.name if material else ""
            if material_name not in material_name_ids:
                raise ValueError(
                    f"hinged door {source_object.name!r} collision material "
                    f"{material_name!r} is not exported"
                )
            material_id = material_name_ids[material_name]
            surface_id = len(result) + 1

            minimum_angle = math.radians(
                float(source_object.get(
                    "ow_door_min_angle_degrees", -105.0
                ))
            )
            maximum_angle = math.radians(
                float(source_object.get(
                    "ow_door_max_angle_degrees", 105.0
                ))
            )
            initial_angle = math.radians(
                float(source_object.get(
                    "ow_door_initial_angle_degrees", 0.0
                ))
            )
            if not minimum_angle < maximum_angle:
                raise ValueError(
                    f"hinged door {source_object.name!r} has invalid limits"
                )
            if not minimum_angle <= initial_angle <= maximum_angle:
                raise ValueError(
                    f"hinged door {source_object.name!r} initial angle lies "
                    "outside its limits"
                )
            result.append(
                ExportHingedDoor(
                    name=str(source_object.get(
                        "ow_door_name", source_object.name
                    )),
                    hinge_position=_to_runtime(hinge),
                    hinge_axis=_to_runtime(axis),
                    closed_width_axis=_to_runtime(width),
                    closed_depth_axis=_to_runtime(depth),
                    local_min=tuple(float(value) for value in minimum),
                    local_max=tuple(float(value) for value in maximum),
                    minimum_angle_radians=minimum_angle,
                    maximum_angle_radians=maximum_angle,
                    initial_angle_radians=initial_angle,
                    mass=float(source_object.get("ow_door_mass", 32.0)),
                    angular_damping=float(source_object.get(
                        "ow_door_angular_damping", 2.2
                    )),
                    return_spring_strength=float(source_object.get(
                        "ow_door_return_spring_strength", 0.0
                    )),
                    maximum_angular_speed=float(source_object.get(
                        "ow_door_maximum_angular_speed", 8.0
                    )),
                    contact_impulse_scale=float(source_object.get(
                        "ow_door_contact_impulse_scale", 1.0
                    )),
                    friction=float(source_object.get(
                        "ow_door_friction", 0.55
                    )),
                    restitution=float(source_object.get(
                        "ow_door_restitution", 0.02
                    )),
                    surface_id=surface_id,
                    vertices=vertices,
                    indices=indices,
                    collision=_door_box_collision(
                        minimum, maximum, surface_id, material_id
                    ),
                )
            )
        finally:
            evaluated.to_mesh_clear()
    return result


def export_scene(
    output_path: str | Path,
    *,
    force_rebuild: bool = False,
    metadata_only: bool = False,
    adopt_existing_cache: bool = False,
) -> Path:
    started = time.perf_counter()
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    if metadata_only:
        manifest = _load_cache_manifest(output)
        if manifest is None or not _manifest_matches_package(output, manifest):
            raise ValueError(
                "metadata-only export requires a valid incremental cache; "
                "run a normal export once or use --adopt-existing-cache"
            )
        _patch_package_metadata(output)
        _refresh_manifest_file_state(output, manifest)
        print(
            "SKATE incremental export:",
            output,
            "mode=metadata_only",
            f"seconds={time.perf_counter() - started:.3f}",
            flush=True,
        )
        return output

    visual_objects = [
        obj
        for obj in _collection(VISUAL_COLLECTION).all_objects
        if bool(obj.get("ow_export_visual", True))
    ]
    static_visual_objects = _static_visual_objects(visual_objects)
    collision_objects = list(_collection(COLLISION_COLLECTION).all_objects)
    grind_objects = list(_collection(GRIND_COLLECTION).all_objects)
    npc_path_objects = list(_collection(NPC_PATH_COLLECTION).all_objects)
    materials = _used_visual_materials(visual_objects)
    images, image_ids = _referenced_images(materials)
    material_ids = {
        material.as_pointer(): index + 1
        for index, material in enumerate(materials)
    }
    material_name_ids = {
        material.name: material_ids[material.as_pointer()]
        for material in materials
    }

    export_materials: list[ExportMaterial] = []
    for material in materials:
        export_materials.append(
            ExportMaterial(
                blender_material=material,
                material_id=material_ids[material.as_pointer()],
                albedo_texture=_texture_id(
                    material, "ow_albedo_image", image_ids
                ),
                lightmap_texture=_texture_id(
                    material, "ow_lightmap_image", image_ids
                ),
                normal_texture=_texture_id(
                    material, "ow_normal_image", image_ids
                ),
                orm_texture=_texture_id(
                    material, "ow_orm_image", image_ids
                ),
                emissive_texture=_texture_id(
                    material, "ow_emissive_image", image_ids
                ),
            )
        )

    fingerprint: SceneContentFingerprint | None = None
    manifest = _load_cache_manifest(output)
    if adopt_existing_cache:
        if not output.is_file():
            raise ValueError(
                "--adopt-existing-cache requires an existing SKATE package"
            )
        fingerprint = _scene_content_fingerprint(
            visual_objects,
            collision_objects,
            grind_objects,
            npc_path_objects,
            materials,
            images,
        )
        package_name, _, package_counts = _read_package_header(output)
        expected_name, _ = _scene_metadata(output)
        expected_counts = (
            len(export_materials),
            len(images),
            fingerprint.visual_vertices,
            fingerprint.visual_indices,
            fingerprint.collision_triangles,
            fingerprint.grind_rails,
            fingerprint.hinged_doors,
            fingerprint.local_lights,
            fingerprint.npc_routes,
        )
        if package_name != expected_name or package_counts != expected_counts:
            raise ValueError(
                "existing package counts do not match the Blender scene; "
                "a full export is required"
            )
        _patch_package_metadata(output)
        _write_cache_manifest(
            output, fingerprint, len(export_materials), len(images)
        )
        print(
            "SKATE incremental cache adopted:",
            output,
            f"seconds={time.perf_counter() - started:.3f}",
            flush=True,
        )
        return output

    if not force_rebuild and manifest is not None:
        fingerprint = _scene_content_fingerprint(
            visual_objects,
            collision_objects,
            grind_objects,
            npc_path_objects,
            materials,
            images,
        )
        if (
            fingerprint.digest == manifest.get("content_sha256")
            and _manifest_matches_package(output, manifest)
        ):
            _patch_package_metadata(output)
            _refresh_manifest_file_state(output, manifest)
            print(
                "SKATE incremental export:",
                output,
                "mode=content_cache_hit",
                f"seconds={time.perf_counter() - started:.3f}",
                flush=True,
            )
            return output
        print(
            "SKATE cache: content changed; rebuilding package",
            flush=True,
        )

    vertices, indices = _export_visual_geometry(
        static_visual_objects, material_name_ids
    )
    collision = _export_collision(
        collision_objects, material_name_ids
    )
    rails = _export_grinds(grind_objects)
    npc_routes = _export_npc_routes(npc_path_objects)
    doors = _export_hinged_doors(visual_objects, material_name_ids)
    lights = _export_local_lights()

    spawn = bpy.data.objects.get(SPAWN_OBJECT)
    if spawn is None:
        raise ValueError(f"required spawn object is missing: {SPAWN_OBJECT}")
    spawn_position = _to_runtime(spawn.matrix_world.translation)
    heading = float(spawn.get("ow_heading_radians", 0.0))
    scene = bpy.context.scene
    sun_color, sun_intensity, orbit_azimuth = _sun_metadata()

    with output.open("wb") as stream:
        stream.write(MAGIC)
        _write_u32(stream, ENDIAN_MARKER)
        _write_string(stream, str(scene.get("ow_map_name", output.stem)))
        _write_vec(stream, spawn_position)
        _write_f32(stream, heading)
        _write_vec(stream, scene.get("ow_sky_zenith", (0.09, 0.34, 0.72)))
        _write_vec(stream, scene.get("ow_sky_horizon", (0.58, 0.78, 0.98)))
        _write_vec(stream, scene.get("ow_sky_nadir", (0.18, 0.25, 0.34)))
        _write_f32(stream, float(scene.get("ow_cycle_seconds", 96.0)))
        _write_f32(stream, float(scene.get("ow_start_hour", 9.0)))
        _write_f32(stream, orbit_azimuth)
        _write_f32(stream, float(scene.get("ow_end_hour", 17.0)))
        _write_f32(
            stream,
            1.0 if bool(scene.get("ow_cycle_ping_pong", False)) else 0.0,
        )
        _write_vec(
            stream,
            scene.get("ow_twilight_zenith", (0.045, 0.10, 0.26)),
        )
        _write_vec(
            stream,
            scene.get("ow_twilight_horizon", (1.0, 0.32, 0.10)),
        )
        _write_vec(
            stream,
            scene.get("ow_twilight_nadir", (0.05, 0.035, 0.06)),
        )
        _write_vec(
            stream,
            scene.get("ow_night_zenith", (0.007, 0.015, 0.045)),
        )
        _write_vec(
            stream,
            scene.get("ow_night_horizon", (0.045, 0.085, 0.17)),
        )
        _write_vec(
            stream,
            scene.get("ow_night_nadir", (0.008, 0.014, 0.032)),
        )
        _write_vec(stream, sun_color)
        _write_vec(
            stream, scene.get("ow_moon_color", (0.42, 0.56, 0.92))
        )
        _write_f32(stream, sun_intensity)
        _write_f32(stream, float(scene.get("ow_moon_intensity", 0.18)))
        _write_f32(stream, float(scene.get("ow_day_ambient", 0.32)))
        _write_f32(stream, float(scene.get("ow_night_ambient", 0.11)))
        _write_vec(stream, scene.get("ow_sky_tint", (1.0, 1.0, 1.0)))
        for count in (
            len(export_materials),
            len(images),
            len(vertices),
            len(indices),
            len(collision),
            len(rails),
            len(doors),
            len(lights),
            len(npc_routes),
        ):
            _write_u32(stream, count)

        for exported in export_materials:
            material = exported.blender_material
            _write_string(stream, material.name)
            _write_u32(stream, int(material.get("ow_flags", 1)))
            _write_f32(stream, float(material.get("ow_friction", 0.82)))
            _write_f32(stream, float(material.get("ow_restitution", 0.0)))
            _write_vec(stream, _material_color(material))
            _write_f32(stream, float(material.get("ow_roughness", 0.78)))
            _write_f32(stream, float(material.get("ow_emissive", 0.0)))
            _write_u32(stream, exported.albedo_texture)
            _write_u32(stream, exported.lightmap_texture)
            _write_f32(stream, float(material.get("ow_baked_strength", 1.0)))
            _write_u32(stream, exported.normal_texture)
            _write_u32(stream, exported.orm_texture)
            _write_u32(stream, exported.emissive_texture)
            _write_u32(
                stream,
                _bounded_int(material, "ow_alpha_mode", 0, 2),
            )
            alpha_cutoff = float(material.get("ow_alpha_cutoff", 0.5))
            if not 0.0 <= alpha_cutoff <= 1.0:
                raise ValueError(
                    f"material {material.name!r} has invalid alpha cutoff"
                )
            _write_f32(stream, alpha_cutoff)
            _write_u32(
                stream,
                _bounded_int(material, "ow_audio_surface", 3, 93),
            )
            _write_u32(
                stream,
                _bounded_int(material, "ow_physics_surface", 1, 12),
            )
            _write_u32(
                stream,
                _bounded_int(material, "ow_surface_pattern", 0, 15),
            )

        lightmap_names = {
            str(material.get("ow_lightmap_image", ""))
            for material in materials
        }
        for image in images:
            rgba8 = _image_rgba8(
                image, lightmap=image.name in lightmap_names
            )
            _require_nonblank_rgb(image.name, rgba8)
            _write_string(stream, image.name)
            _write_u32(stream, int(image.size[0]))
            _write_u32(stream, int(image.size[1]))
            # Blender exposes generated/baked image pixels in scene-linear
            # space. Preserve those values directly in v2; a future
            # compressed texture stage may encode albedo as sRGB.
            _write_u32(stream, 0)
            _write_u32(stream, len(rgba8))
            stream.write(rgba8)

        for position, normal, uv0, uv1, material_id in vertices:
            _write_vec(stream, position)
            _write_vec(stream, normal)
            _write_vec(stream, uv0)
            _write_vec(stream, uv1)
            _write_u32(stream, material_id)
        for index in indices:
            _write_u32(stream, index)
        for a, b, c, surface_id, material_id in collision:
            _write_vec(stream, a)
            _write_vec(stream, b)
            _write_vec(stream, c)
            _write_u32(stream, surface_id)
            _write_u32(stream, material_id)
        for name, closed, points in rails:
            _write_string(stream, name)
            _write_u32(stream, 1 if closed else 0)
            _write_u32(stream, len(points))
            for point in points:
                _write_vec(stream, point)
        for door in doors:
            _write_string(stream, door.name)
            _write_vec(stream, door.hinge_position)
            _write_vec(stream, door.hinge_axis)
            _write_vec(stream, door.closed_width_axis)
            _write_vec(stream, door.closed_depth_axis)
            _write_vec(stream, door.local_min)
            _write_vec(stream, door.local_max)
            _write_f32(stream, door.minimum_angle_radians)
            _write_f32(stream, door.maximum_angle_radians)
            _write_f32(stream, door.initial_angle_radians)
            _write_f32(stream, door.mass)
            _write_f32(stream, door.angular_damping)
            _write_f32(stream, door.return_spring_strength)
            _write_f32(stream, door.maximum_angular_speed)
            _write_f32(stream, door.contact_impulse_scale)
            _write_f32(stream, door.friction)
            _write_f32(stream, door.restitution)
            _write_u32(stream, door.surface_id)
            _write_u32(stream, len(door.vertices))
            _write_u32(stream, len(door.indices))
            _write_u32(stream, len(door.collision))
            for position, normal, uv0, uv1, material_id in door.vertices:
                _write_vec(stream, position)
                _write_vec(stream, normal)
                _write_vec(stream, uv0)
                _write_vec(stream, uv1)
                _write_u32(stream, material_id)
            for index in door.indices:
                _write_u32(stream, index)
            for a, b, c, surface_id, material_id in door.collision:
                _write_vec(stream, a)
                _write_vec(stream, b)
                _write_vec(stream, c)
                _write_u32(stream, surface_id)
                _write_u32(stream, material_id)
        for light in lights:
            _write_string(stream, light.name)
            _write_u32(stream, light.light_type)
            _write_vec(stream, light.position)
            _write_vec(stream, light.direction)
            _write_vec(stream, light.color)
            _write_f32(stream, light.intensity)
            _write_f32(stream, light.influence_radius)
            _write_f32(stream, light.source_radius)
            _write_f32(stream, light.spot_inner_cosine)
            _write_f32(stream, light.spot_outer_cosine)
        for (
            name,
            closed,
            skater_count,
            speed,
            spawn_spacing,
            points,
        ) in npc_routes:
            _write_string(stream, name)
            _write_u32(stream, 1 if closed else 0)
            _write_u32(stream, skater_count)
            _write_f32(stream, speed)
            _write_f32(stream, spawn_spacing)
            _write_u32(stream, len(points))
            for point in points:
                _write_vec(stream, point)

    print(
        "SKATE export:",
        output,
        f"materials={len(export_materials)}",
        f"textures={len(images)}",
        f"triangles={len(indices) // 3}",
        f"collision={len(collision)}",
        f"rails={len(rails)}",
        f"doors={len(doors)}",
        f"lights={len(lights)}",
        f"npc_routes={len(npc_routes)}",
        f"seconds={time.perf_counter() - started:.3f}",
        flush=True,
    )
    if fingerprint is None:
        fingerprint = _scene_content_fingerprint(
            visual_objects,
            collision_objects,
            grind_objects,
            npc_path_objects,
            materials,
            images,
        )
    _write_cache_manifest(
        output, fingerprint, len(export_materials), len(images)
    )
    print(
        "SKATE cache: manifest updated",
        _cache_manifest_path(output),
        flush=True,
    )
    return output


def main(arguments: list[str] | None = None) -> Path:
    if arguments is None:
        arguments = (
            sys.argv[sys.argv.index("--") + 1 :]
            if "--" in sys.argv
            else []
        )
    if not arguments:
        raise SystemExit("usage: blender --background file.blend --python "
                         "export_skate.py -- output.skate "
                         "[--force|--metadata-only|--adopt-existing-cache]")
    flags = set(arguments[1:])
    allowed_flags = {
        "--force",
        "--metadata-only",
        "--adopt-existing-cache",
    }
    unknown_flags = flags - allowed_flags
    if unknown_flags:
        raise SystemExit(
            "unknown SKATE export arguments: "
            + ", ".join(sorted(unknown_flags))
        )
    selected_modes = sum(
        flag in flags
        for flag in ("--force", "--metadata-only", "--adopt-existing-cache")
    )
    if selected_modes > 1:
        raise SystemExit(
            "--force, --metadata-only, and --adopt-existing-cache are "
            "mutually exclusive"
        )
    return export_scene(
        arguments[0],
        force_rebuild="--force" in flags,
        metadata_only="--metadata-only" in flags,
        adopt_existing_cache="--adopt-existing-cache" in flags,
    )


if __name__ == "__main__":
    main()
