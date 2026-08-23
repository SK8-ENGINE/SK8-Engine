"""Original Blender -> SKATE v11 exporter.

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
import zlib
from typing import BinaryIO, Callable

import bpy
from mathutils import Vector
try:
    import numpy
except ImportError:
    numpy = None


MAGIC = b"SKATE11\0"
ENDIAN_MARKER = 0x12345678
STORAGE_RAW = 0
STORAGE_DEFLATE = 1
PRESENTATION_COLLISION_COLLECTION = "OW_GROUP_1_PRESENTATION_COLLISION"
NO_PRESENTATION_COLLECTION = "OW_GROUP_2_NO_PRESENTATION"
NO_COLLISION_COLLECTION = "OW_GROUP_3_NO_COLLISION"
GRIND_COLLECTION = "OW_GROUP_4_GRINDS"
NPC_PATH_COLLECTION = "OW_GROUP_5_PATHING"
# Public aliases retained for scripts written against add-on 1.x. New scenes
# use the five exclusive collections above.
VISUAL_COLLECTION = PRESENTATION_COLLISION_COLLECTION
COLLISION_COLLECTION = NO_PRESENTATION_COLLECTION
LEGACY_VISUAL_COLLECTION = "OW_VISUAL"
LEGACY_COLLISION_COLLECTION = "OW_COLLISION"
LEGACY_GRIND_COLLECTION = "OW_GRIND"
LEGACY_NPC_PATH_COLLECTION = "OW_NPC_PATHS"
GROUP_COLLECTIONS = (
    PRESENTATION_COLLISION_COLLECTION,
    NO_PRESENTATION_COLLECTION,
    NO_COLLISION_COLLECTION,
    GRIND_COLLECTION,
    NPC_PATH_COLLECTION,
)
SPAWN_OBJECT = "OW_SPAWN"
_HELPER_OBJECT_MARKERS = (
    "character_size",
    "character size",
    "player_size",
    "player size",
    "scale_reference",
    "scale reference",
)
CACHE_SCHEMA = 13
METADATA_FLOAT_COUNT = 49
METADATA_BYTE_COUNT = METADATA_FLOAT_COUNT * 4
RETAIL_EDGE_CODE_ATTRIBUTES = tuple(
    f"skate3_retail_edge_code_{corner}" for corner in range(3)
)


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
class PackedVisualGeometry:
    vertex_chunks: list[bytes]
    index_chunks: list[bytes]
    vertex_count: int
    index_count: int


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
class ExportGrindRail:
    name: str
    closed: bool
    points: list[tuple[float, float, float]]
    retail_spline_id: int = 0
    retail_type_signature: int = 0
    retail_flags: int = 0
    retail_trailing_word: int = 0
    native_segment_payload: bytes = b""


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


@dataclass
class CollisionGeometryAudit:
    issues: list[str]
    warnings: list[str]
    source_triangles: int
    exported_triangles: int
    skipped_degenerate: int
    skipped_duplicates: int


class CollisionGeometryError(ValueError):
    def __init__(self, issues: list[str], warnings: list[str]) -> None:
        self.issues = list(issues)
        self.warnings = list(warnings)
        summary = "Collision validation failed:\n" + "\n".join(
            f"- {issue}" for issue in self.issues
        )
        super().__init__(summary)


LAST_COLLISION_AUDIT: CollisionGeometryAudit | None = None
ProgressCallback = Callable[[float, str], None]


def _report_progress(
    callback: ProgressCallback | None,
    fraction: float,
    stage: str,
) -> None:
    if callback is not None:
        callback(max(0.0, min(1.0, float(fraction))), stage)


def _write_u32(stream: BinaryIO, value: int) -> None:
    stream.write(struct.pack("<I", value))


def _write_u64(stream: BinaryIO, value: int) -> None:
    stream.write(struct.pack("<Q", value))


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


def _write_stored_bytes(
    stream: BinaryIO,
    decoded: bytes,
) -> tuple[int, int]:
    compressed = zlib.compress(decoded, level=6)
    if len(compressed) >= len(decoded):
        _write_u32(stream, STORAGE_RAW)
        _write_u32(stream, len(decoded))
        stream.write(decoded)
        return STORAGE_RAW, len(decoded)
    _write_u32(stream, STORAGE_DEFLATE)
    _write_u32(stream, len(compressed))
    stream.write(compressed)
    return STORAGE_DEFLATE, len(compressed)


def _write_stored_chunks(
    stream: BinaryIO,
    chunks: list[bytes],
) -> tuple[int, int]:
    header_offset = stream.tell()
    _write_u32(stream, STORAGE_DEFLATE)
    _write_u32(stream, 0)
    compressor = zlib.compressobj(level=6)
    stored_size = 0
    for chunk in chunks:
        compressed = compressor.compress(chunk)
        stream.write(compressed)
        stored_size += len(compressed)
    compressed = compressor.flush()
    stream.write(compressed)
    stored_size += len(compressed)
    if stored_size > 0xFFFFFFFF:
        raise ValueError("compressed SKATE block exceeds the u32 size limit")
    end = stream.tell()
    stream.seek(header_offset + 4)
    _write_u32(stream, stored_size)
    stream.seek(end)
    return STORAGE_DEFLATE, stored_size


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

    packed = image.packed_file
    if packed is not None and not image.is_dirty:
        # Clean packed images decode deterministically from these source
        # bytes. Hashing the packed payload avoids expanding large texture
        # sets to float RGBA solely for an incremental-cache check.
        _hash_text(digest, "PACKED_SOURCE")
        digest.update(struct.pack("<Q", int(packed.size)))
        digest.update(packed.data)
        return

    # Generated, baked, and dirty packed images have no trustworthy immutable
    # source fingerprint. Hash their actual pixels so cache reuse is correct.
    expected = int(image.size[0]) * int(image.size[1]) * 4
    values = array("f", [0.0]) * expected
    if expected:
        image.pixels.foreach_get(values)
    if sys.byteorder != "little":
        values.byteswap()
    digest.update(values.tobytes())


def _mesh_for_export(
    source_object: bpy.types.Object,
    depsgraph,
    *,
    preserve_all_data_layers: bool = False,
) -> tuple[bpy.types.Mesh, bpy.types.Object | None]:
    # Blender 5.1 can keep newly-created UV layers off an already-built
    # evaluated mesh even after the source datablock is tagged and the view
    # layer is refreshed. An unmodified mesh does not need evaluation, so use
    # its authoritative source data directly. Modified or shape-keyed meshes
    # still use Blender's evaluated result.
    source_mesh = source_object.data
    if not source_object.modifiers and source_mesh.shape_keys is None:
        return source_mesh, None

    evaluated = source_object.evaluated_get(depsgraph)
    mesh = (
        evaluated.to_mesh(
            preserve_all_data_layers=True, depsgraph=depsgraph
        )
        if preserve_all_data_layers
        else evaluated.to_mesh()
    )
    if mesh is None:
        raise ValueError(
            f"Blender could not evaluate mesh {source_object.name!r}"
        )
    return mesh, evaluated


def _hash_mesh(
    digest,
    source_object: bpy.types.Object,
    *,
    visual: bool,
    depsgraph,
) -> tuple[int, int]:
    mesh, evaluated = _mesh_for_export(
        source_object,
        depsgraph,
        preserve_all_data_layers=visual,
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
        if evaluated is not None:
            evaluated.to_mesh_clear()


def _scene_content_fingerprint(
    visual_objects: list[bpy.types.Object],
    collision_objects: list[bpy.types.Object],
    grind_objects: list[bpy.types.Object],
    npc_path_objects: list[bpy.types.Object],
    materials: list[bpy.types.Material],
    images: list[bpy.types.Image],
    collision_triangle_count: int,
    progress: ProgressCallback | None = None,
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
        "ow_lightmap_encoding",
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
    for image_index, image in enumerate(images, start=1):
        _hash_image_source(digest, image)
        _report_progress(
            progress,
            0.15 * image_index / max(1, len(images)),
            f"Hashing textures ({image_index}/{len(images)}): "
            f"{image.name}",
        )

    depsgraph = bpy.context.evaluated_depsgraph_get()
    total_objects = max(
        1,
        len(visual_objects)
        + len(collision_objects)
        + len(grind_objects)
        + len(npc_path_objects)
        + len(_visible_local_light_objects()),
    )
    processed_objects = 0

    def object_complete(label: str) -> None:
        nonlocal processed_objects
        processed_objects += 1
        _report_progress(
            progress,
            0.15 + 0.85 * processed_objects / total_objects,
            label,
        )

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
        object_complete(f"Hashing visuals: {obj.name}")

    for obj in collision_objects:
        if obj.type != "MESH":
            continue
        material = bpy.data.materials.get(str(obj.get("ow_material", "")))
        if material is not None and not bool(
            material.get("ow_collision_enabled", True)
        ):
            continue
        _hash_text(
            digest,
            int(
                bool(
                    obj.get(
                        "ow_preserve_opposite_wound_collision",
                        False,
                    )
                )
            ),
        )
        preserve_retail_codes = bool(
            obj.get("ow_preserve_retail_edge_codes", False)
        )
        _hash_text(digest, int(preserve_retail_codes))
        if preserve_retail_codes:
            for attribute_name in RETAIL_EDGE_CODE_ATTRIBUTES:
                attribute = obj.data.attributes.get(attribute_name)
                _hash_text(digest, attribute_name)
                if attribute is None:
                    _hash_text(digest, "MISSING")
                else:
                    _hash_text(digest, attribute.domain)
                    _hash_text(digest, attribute.data_type)
                    _hash_foreach(
                        digest,
                        attribute.data,
                        "value",
                        1,
                        "i",
                    )
        _hash_mesh(
            digest, obj, visual=False, depsgraph=depsgraph
        )
        object_complete(f"Hashing collision: {obj.name}")

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
                _hash_foreach(
                    digest, spline.bezier_points, "handle_left", 3, "f"
                )
                _hash_foreach(
                    digest, spline.bezier_points, "handle_right", 3, "f"
                )
                if len(spline.bezier_points) >= 2:
                    grind_rails += 1
        for property_name in (
            "skate3_retail_grind",
            "skate3_retail_grind_spline_id",
            "skate3_retail_grind_type_signature",
            "skate3_retail_grind_flags",
            "skate3_retail_grind_trailing_word",
            "skate3_retail_grind_segment_count",
            "skate3_retail_grind_segment_payload",
        ):
            _hash_text(digest, repr(obj.get(property_name, None)))
        object_complete(f"Hashing grind paths: {obj.name}")

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
        object_complete(f"Hashing NPC paths: {obj.name}")

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
        object_complete(f"Hashing lights: {obj.name}")

    result = SceneContentFingerprint(
        digest=digest.hexdigest(),
        visual_vertices=visual_vertices,
        visual_indices=visual_indices,
        collision_triangles=collision_triangle_count,
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
            raise ValueError(f"{output} is not an SKATE v11 package")
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
        _spawn_heading(spawn),
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
        "visual_vertex_count": counts[2],
        "visual_index_count": counts[3],
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
    image: bpy.types.Image,
    *,
    lightmap: bool = False,
    lightmap_encoding: str = "",
) -> bytes:
    retail_encoded = (
        lightmap_encoding == "skate3_retail_sqrt_linear_over_4"
    )
    if lightmap_encoding and not retail_encoded:
        raise ValueError(
            f"image {image.name!r} uses unsupported lightmap encoding "
            f"{lightmap_encoding!r}"
        )
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
        if lightmap and not retail_encoded:
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
        if lightmap and not retail_encoded and channel != 3:
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


def _objects_from_collections(*names: str) -> list[bpy.types.Object]:
    result: list[bpy.types.Object] = []
    seen: set[int] = set()
    for name in names:
        collection = bpy.data.collections.get(name)
        if collection is None:
            continue
        for obj in collection.all_objects:
            identity = obj.as_pointer()
            if identity not in seen:
                seen.add(identity)
                result.append(obj)
    return result


def _spawn_heading(spawn: bpy.types.Object) -> float:
    if spawn.type == "MESH":
        return float(spawn.matrix_world.to_euler("XYZ").z)
    return float(spawn.get("ow_heading_radians", 0.0))


def _is_helper_object(obj: bpy.types.Object) -> bool:
    """Reject authoring references even when an old add-on linked them."""
    identity = f"{obj.name} {getattr(obj.data, 'name', '')}".lower()
    return any(marker in identity for marker in _HELPER_OBJECT_MARKERS)


def _used_export_materials(
    visual_objects: list[bpy.types.Object],
    collision_objects: list[bpy.types.Object],
) -> list[bpy.types.Material]:
    result: list[bpy.types.Material] = []
    seen: set[int] = set()
    for obj in [*visual_objects, *collision_objects]:
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
        raise ValueError("export groups do not reference any materials")
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
    progress: ProgressCallback | None = None,
) -> PackedVisualGeometry:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    vertex_chunks: list[bytes] = []
    index_chunks: list[bytes] = []
    vertex_count = 0
    index_count = 0
    mesh_objects = [
        obj for obj in visual_objects if obj.type == "MESH"
    ]
    weights = [
        max(1, len(obj.data.polygons)) for obj in mesh_objects
    ]
    total_weight = max(1, sum(weights))
    completed_weight = 0
    packed_vertex = struct.Struct("<3f3f2f2fI")

    for object_index, (source_object, object_weight) in enumerate(
        zip(mesh_objects, weights, strict=True),
        start=1,
    ):
        if source_object.type != "MESH":
            continue
        mesh, evaluated = _mesh_for_export(
            source_object,
            depsgraph,
            preserve_all_data_layers=True,
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
            loop_count = len(mesh.loops)
            if len(uv0.data) != loop_count or len(uv1.data) != loop_count:
                raise ValueError(
                    f"visual mesh {source_object.name!r} has malformed UV "
                    "layer data"
                )

            if numpy is not None:
                triangle_count = len(mesh.loop_triangles)
                corner_count = triangle_count * 3
                triangle_loops = numpy.empty(
                    corner_count, dtype=numpy.int32
                )
                triangle_polygons = numpy.empty(
                    triangle_count, dtype=numpy.int32
                )
                mesh.loop_triangles.foreach_get(
                    "loops", triangle_loops
                )
                mesh.loop_triangles.foreach_get(
                    "polygon_index", triangle_polygons
                )

                loop_vertices = numpy.empty(
                    loop_count, dtype=numpy.int32
                )
                loop_normals = numpy.empty(
                    loop_count * 3, dtype=numpy.float32
                )
                mesh.loops.foreach_get(
                    "vertex_index", loop_vertices
                )
                mesh.loops.foreach_get("normal", loop_normals)
                loop_normals = loop_normals.reshape((-1, 3))

                source_positions = numpy.empty(
                    len(mesh.vertices) * 3, dtype=numpy.float32
                )
                mesh.vertices.foreach_get("co", source_positions)
                source_positions = source_positions.reshape((-1, 3))

                base_uvs = numpy.empty(
                    loop_count * 2, dtype=numpy.float32
                )
                light_uvs = numpy.empty(
                    loop_count * 2, dtype=numpy.float32
                )
                uv0.data.foreach_get("uv", base_uvs)
                uv1.data.foreach_get("uv", light_uvs)
                base_uvs = base_uvs.reshape((-1, 2))
                light_uvs = light_uvs.reshape((-1, 2))

                polygon_materials = numpy.empty(
                    len(mesh.polygons), dtype=numpy.int32
                )
                mesh.polygons.foreach_get(
                    "material_index", polygon_materials
                )
                if (
                    polygon_materials.size
                    and (
                        int(polygon_materials.min()) < 0
                        or int(polygon_materials.max())
                        >= len(mesh.materials)
                    )
                ):
                    raise ValueError(
                        f"mesh {source_object.name!r} has an invalid "
                        "material slot"
                    )
                mesh_material_ids = numpy.empty(
                    len(mesh.materials), dtype=numpy.uint32
                )
                for material_index, material in enumerate(mesh.materials):
                    if (
                        material is None
                        or material.name not in material_name_ids
                    ):
                        raise ValueError(
                            f"mesh {source_object.name!r} has an "
                            "unexported material"
                        )
                    mesh_material_ids[material_index] = (
                        material_name_ids[material.name]
                    )

                corner_vertices = loop_vertices[triangle_loops]
                positions = source_positions[corner_vertices].astype(
                    numpy.float64
                )
                world_matrix = numpy.asarray(
                    source_object.matrix_world, dtype=numpy.float64
                )
                positions = (
                    positions @ world_matrix[:3, :3].T
                    + world_matrix[:3, 3]
                )

                normal_matrix = numpy.asarray(
                    source_object.matrix_world
                    .to_3x3()
                    .inverted()
                    .transposed(),
                    dtype=numpy.float64,
                )
                normals = (
                    loop_normals[triangle_loops].astype(numpy.float64)
                    @ normal_matrix.T
                )
                lengths = numpy.linalg.norm(normals, axis=1)
                nonzero_normals = lengths > 1.0e-12
                normals[nonzero_normals] /= lengths[
                    nonzero_normals, None
                ]

                runtime_positions = positions[:, (0, 2, 1)].copy()
                runtime_positions[:, 2] *= -1.0
                runtime_normals = normals[:, (0, 2, 1)].copy()
                runtime_normals[:, 2] *= -1.0
                corner_materials = numpy.repeat(
                    mesh_material_ids[
                        polygon_materials[triangle_polygons]
                    ],
                    3,
                )

                if not all(
                    numpy.isfinite(values).all()
                    for values in (
                        runtime_positions,
                        runtime_normals,
                        base_uvs[triangle_loops],
                        light_uvs[triangle_loops],
                    )
                ):
                    raise ValueError(
                        f"visual mesh {source_object.name!r} contains "
                        "non-finite geometry or UV values"
                    )

                record_dtype = numpy.dtype(
                    [
                        ("position", "<f4", (3,)),
                        ("normal", "<f4", (3,)),
                        ("uv0", "<f4", (2,)),
                        ("uv1", "<f4", (2,)),
                        ("material", "<u4"),
                    ],
                    align=False,
                )
                records = numpy.empty(corner_count, dtype=record_dtype)
                records["position"] = runtime_positions
                records["normal"] = runtime_normals
                records["uv0"] = base_uvs[triangle_loops]
                records["uv1"] = light_uvs[triangle_loops]
                records["material"] = corner_materials
                unique_records, inverse = numpy.unique(
                    records,
                    return_inverse=True,
                )
                if vertex_count + len(unique_records) > 0xFFFFFFFF:
                    raise ValueError(
                        "visual geometry exceeds the SKATE u32 index limit"
                    )
                indices = inverse.astype(
                    numpy.dtype("<u4"), copy=False
                )
                if vertex_count:
                    indices += vertex_count
                vertex_chunks.append(unique_records.tobytes())
                index_chunks.append(indices.tobytes())
                vertex_count += len(unique_records)
                index_count += corner_count
            else:
                normal_matrix = (
                    source_object.matrix_world
                    .to_3x3()
                    .inverted()
                    .transposed()
                )
                unique_vertices: dict[bytes, int] = {}
                vertex_chunk = bytearray()
                index_values = array("I")
                for triangle in mesh.loop_triangles:
                    polygon = mesh.polygons[triangle.polygon_index]
                    if polygon.material_index >= len(mesh.materials):
                        raise ValueError(
                            f"mesh {source_object.name!r} has an invalid "
                            "material slot"
                        )
                    material = mesh.materials[polygon.material_index]
                    if (
                        material is None
                        or material.name not in material_name_ids
                    ):
                        raise ValueError(
                            f"mesh {source_object.name!r} has an "
                            "unexported material"
                        )
                    material_id = material_name_ids[material.name]
                    for loop_index in triangle.loops:
                        loop = mesh.loops[loop_index]
                        point = (
                            source_object.matrix_world
                            @ mesh.vertices[loop.vertex_index].co
                        )
                        normal = (
                            normal_matrix @ loop.normal
                        ).normalized()
                        base_uv = uv0.data[loop_index].uv
                        light_uv = uv1.data[loop_index].uv
                        values = (
                            *_to_runtime(point),
                            *_to_runtime(normal),
                            float(base_uv.x),
                            float(base_uv.y),
                            float(light_uv.x),
                            float(light_uv.y),
                        )
                        if not all(math.isfinite(value) for value in values):
                            raise ValueError(
                                f"visual mesh {source_object.name!r} "
                                "contains non-finite geometry or UV values"
                            )
                        record = packed_vertex.pack(*values, material_id)
                        local_index = unique_vertices.get(record)
                        if local_index is None:
                            local_index = len(unique_vertices)
                            unique_vertices[record] = local_index
                            vertex_chunk.extend(record)
                        index_values.append(vertex_count + local_index)
                        index_count += 1
                if sys.byteorder != "little":
                    index_values.byteswap()
                vertex_chunks.append(bytes(vertex_chunk))
                index_chunks.append(index_values.tobytes())
                vertex_count += len(unique_vertices)
        finally:
            if evaluated is not None:
                evaluated.to_mesh_clear()

        completed_weight += object_weight
        _report_progress(
            progress,
            completed_weight / total_weight,
            f"Packing visuals ({object_index}/{len(mesh_objects)}): "
            f"{source_object.name}",
        )

    if not vertex_count:
        raise ValueError("presentation groups contain no exportable triangles")
    return PackedVisualGeometry(
        vertex_chunks,
        index_chunks,
        vertex_count,
        index_count,
    )


def audit_collision_geometry(
    collision_objects: list[bpy.types.Object],
    material_name_ids: dict[str, int] | None = None,
    progress: ProgressCallback | None = None,
) -> tuple[list[tuple], CollisionGeometryAudit]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    triangles: list[tuple] = []
    triangle_owners: dict[tuple, str] = {}
    issues: list[str] = []
    warnings: list[str] = []
    source_triangle_count = 0
    skipped_degenerate = 0
    skipped_duplicates = 0
    surface_id = 1
    mesh_objects = [
        obj for obj in collision_objects if obj.type == "MESH"
    ]
    for object_index, source_object in enumerate(
        mesh_objects, start=1
    ):
        _report_progress(
            progress,
            (object_index - 1) / max(1, len(mesh_objects)),
            f"Auditing collision ({object_index}/"
            f"{len(mesh_objects)}): {source_object.name}",
        )
        material_name = str(source_object.get("ow_material", ""))
        blender_material = bpy.data.materials.get(material_name)
        if (
            blender_material is not None
            and not bool(
                blender_material.get("ow_collision_enabled", True)
            )
        ):
            continue
        if (
            material_name_ids is not None
            and material_name not in material_name_ids
        ):
            issues.append(
                f"{source_object.name}: collision material "
                f"{material_name!r} is not exported by a presentation group."
            )
            continue
        material_id = (
            material_name_ids[material_name]
            if material_name_ids is not None
            else 0
        )
        preserve_retail_codes = bool(
            source_object.get("ow_preserve_retail_edge_codes", False)
        )
        mesh, evaluated = _mesh_for_export(
            source_object,
            depsgraph,
            preserve_all_data_layers=preserve_retail_codes,
        )
        edge_attributes = []
        if preserve_retail_codes:
            for attribute_name in RETAIL_EDGE_CODE_ATTRIBUTES:
                attribute = mesh.attributes.get(attribute_name)
                if (
                    attribute is None
                    or attribute.domain != "FACE"
                    or attribute.data_type != "INT"
                ):
                    issues.append(
                        f"{source_object.name}: native retail collision "
                        f"attribute {attribute_name!r} is missing or invalid."
                    )
                    edge_attributes = []
                    break
                edge_attributes.append(attribute)
        object_degenerate = 0
        object_duplicates = 0
        object_non_finite = 0
        object_wrong_facing = 0
        try:
            mesh.calc_loop_triangles()
            for triangle in mesh.loop_triangles:
                source_triangle_count += 1
                blender_points = [
                    source_object.matrix_world @ mesh.vertices[index].co
                    for index in triangle.vertices
                ]
                if not all(
                    math.isfinite(float(component))
                    for point in blender_points
                    for component in point
                ):
                    object_non_finite += 1
                    continue
                try:
                    points = [
                        tuple(
                            struct.unpack(
                                "<f", struct.pack("<f", component)
                            )[0]
                            for component in _to_runtime(point)
                        )
                        for point in blender_points
                    ]
                except (OverflowError, struct.error):
                    object_non_finite += 1
                    continue
                runtime_cross = (Vector(points[1]) - Vector(points[0])).cross(
                    Vector(points[2]) - Vector(points[0])
                )
                # Keep this exactly aligned with the C++ SKATE loader. Very
                # small source triangles can survive Blender's mesh cleanup
                # but collapse after float32 package serialization.
                if runtime_cross.length_squared <= 1.0e-10:
                    object_degenerate += 1
                    skipped_degenerate += 1
                    continue
                if (
                    bool(source_object.get("ow_upward_surface", False))
                    and runtime_cross.y <= 1.0e-6
                ):
                    object_wrong_facing += 1
                    continue
                rounded_points = tuple(
                    tuple(
                        round(float(component), 6)
                        for component in point
                    )
                    for point in points
                )
                if bool(
                    source_object.get(
                        "ow_preserve_opposite_wound_collision",
                        False,
                    )
                ):
                    # Cyclic rotation does not change triangle orientation,
                    # but reversing two vertices does. Retail ClusteredMesh
                    # uses reverse-wound partners to provide intentional
                    # two-sided collision, so only same-wound copies are
                    # duplicates for these objects.
                    key = min(
                        rounded_points,
                        (
                            rounded_points[1],
                            rounded_points[2],
                            rounded_points[0],
                        ),
                        (
                            rounded_points[2],
                            rounded_points[0],
                            rounded_points[1],
                        ),
                    )
                else:
                    # Generic authored maps retain the stricter cleanup:
                    # opposite-wound copies can otherwise create
                    # contradictory contacts and broken adjacency.
                    key = tuple(sorted(rounded_points))
                if key in triangle_owners:
                    object_duplicates += 1
                    skipped_duplicates += 1
                    continue
                triangle_owners[key] = source_object.name
                native_edge_codes = None
                if preserve_retail_codes and len(edge_attributes) == 3:
                    polygon_vertices = tuple(
                        mesh.polygons[triangle.polygon_index].vertices
                    )
                    triangle_vertices = tuple(triangle.vertices)
                    if len(polygon_vertices) != 3:
                        issues.append(
                            f"{source_object.name}: native retail collision "
                            "metadata is attached to a non-triangle face."
                        )
                        continue
                    rotation = next(
                        (
                            offset
                            for offset in range(3)
                            if triangle_vertices
                            == polygon_vertices[offset:]
                            + polygon_vertices[:offset]
                        ),
                        None,
                    )
                    if rotation is None:
                        issues.append(
                            f"{source_object.name}: Blender reversed a face "
                            "with native retail collision edge codes."
                        )
                        continue
                    polygon_codes = tuple(
                        int(attribute.data[triangle.polygon_index].value)
                        for attribute in edge_attributes
                    )
                    if any(code < 0 or code > 255 for code in polygon_codes):
                        issues.append(
                            f"{source_object.name}: native retail collision "
                            "edge code is outside the byte range."
                        )
                        continue
                    native_edge_codes = tuple(
                        polygon_codes[(rotation + corner) % 3]
                        for corner in range(3)
                    )
                triangles.append(
                    (
                        points[0],
                        points[1],
                        points[2],
                        surface_id,
                        material_id,
                        native_edge_codes,
                    )
                )
        finally:
            if evaluated is not None:
                evaluated.to_mesh_clear()
        if object_degenerate:
            warnings.append(
                f"{source_object.name}: skipped {object_degenerate} "
                "zero-area collision triangle(s). No mesh dissolve or UV "
                "changes are required."
            )
        if object_duplicates:
            warnings.append(
                f"{source_object.name}: skipped {object_duplicates} "
                + (
                    "same-wound duplicate collision triangle(s); retained "
                    "reverse-wound retail partners."
                    if bool(
                        source_object.get(
                            "ow_preserve_opposite_wound_collision",
                            False,
                        )
                    )
                    else "exact or opposite-wound duplicate collision "
                    "triangle(s)."
                )
            )
        if object_non_finite:
            issues.append(
                f"{source_object.name}: {object_non_finite} collision "
                "triangle(s) contain non-finite or float32-out-of-range "
                "coordinates."
            )
        if object_wrong_facing:
            issues.append(
                f"{source_object.name}: {object_wrong_facing} triangle(s) "
                "face downward or vertically, but this object is marked "
                "Rideable Top Surface."
            )
        surface_id += 1
    if not triangles:
        issues.append("collision groups contain no usable collision triangles.")
    audit = CollisionGeometryAudit(
        issues=issues,
        warnings=warnings,
        source_triangles=source_triangle_count,
        exported_triangles=len(triangles),
        skipped_degenerate=skipped_degenerate,
        skipped_duplicates=skipped_duplicates,
    )
    _report_progress(progress, 1.0, "Collision audit complete")
    return triangles, audit


def _export_collision(
    collision_objects: list[bpy.types.Object],
    material_name_ids: dict[str, int],
    progress: ProgressCallback | None = None,
) -> tuple[list[tuple], CollisionGeometryAudit]:
    global LAST_COLLISION_AUDIT
    triangles, audit = audit_collision_geometry(
        collision_objects, material_name_ids, progress
    )
    LAST_COLLISION_AUDIT = audit
    if audit.issues:
        raise CollisionGeometryError(audit.issues, audit.warnings)
    for warning in audit.warnings:
        print(f"SKATE collision cleanup: {warning}", flush=True)
    return triangles, audit


def _retail_grind_controls(
    payload: bytes,
) -> tuple[tuple[float, float, float], ...]:
    values = struct.unpack(">30f", payload)
    coefficient_a = values[0:3]
    coefficient_b = values[4:7]
    coefficient_c = values[8:11]
    coefficient_d = values[12:15]
    return (
        tuple(coefficient_d),
        tuple(
            coefficient_d[axis] + coefficient_c[axis] / 3.0
            for axis in range(3)
        ),
        tuple(
            coefficient_d[axis]
            + (2.0 * coefficient_c[axis] + coefficient_b[axis]) / 3.0
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


def _close_enough(
    left: tuple[float, float, float],
    right: tuple[float, float, float],
    tolerance: float = 2.0e-3,
) -> bool:
    return all(
        abs(left[axis] - right[axis]) <= tolerance
        for axis in range(3)
    )


def _export_retail_grind(
    obj: bpy.types.Object,
) -> ExportGrindRail:
    if len(obj.data.splines) != 1:
        raise ValueError(
            f"retail grind {obj.name!r} must contain exactly one spline"
        )
    spline = obj.data.splines[0]
    if spline.type != "BEZIER":
        raise ValueError(
            f"retail grind {obj.name!r} must remain a Bezier spline"
        )
    segment_count = int(obj["skate3_retail_grind_segment_count"])
    actual_segments = (
        len(spline.bezier_points)
        if spline.use_cyclic_u
        else len(spline.bezier_points) - 1
    )
    if segment_count <= 0 or actual_segments != segment_count:
        raise ValueError(
            f"retail grind {obj.name!r} segment count changed: "
            f"{actual_segments} versus {segment_count}"
        )
    payload_hex = str(obj["skate3_retail_grind_segment_payload"])
    try:
        payload = bytes.fromhex(payload_hex)
    except ValueError as error:
        raise ValueError(
            f"retail grind {obj.name!r} has invalid native payload"
        ) from error
    if len(payload) != segment_count * 120:
        raise ValueError(
            f"retail grind {obj.name!r} native payload size changed"
        )

    points = spline.bezier_points
    for segment_index in range(segment_count):
        current = points[segment_index]
        following = points[(segment_index + 1) % len(points)]
        actual_controls = (
            _to_runtime(obj.matrix_world @ current.co),
            _to_runtime(obj.matrix_world @ current.handle_right),
            _to_runtime(obj.matrix_world @ following.handle_left),
            _to_runtime(obj.matrix_world @ following.co),
        )
        expected_controls = _retail_grind_controls(
            payload[segment_index * 120 : (segment_index + 1) * 120]
        )
        if not all(
            _close_enough(actual, expected)
            for actual, expected in zip(
                actual_controls,
                expected_controls,
            )
        ):
            raise ValueError(
                f"retail grind {obj.name!r} was edited; exact native "
                f"segment {segment_index} no longer matches its Blender curve"
            )

    return ExportGrindRail(
        name=obj.name,
        closed=bool(spline.use_cyclic_u),
        points=[],
        retail_spline_id=int(
            str(obj["skate3_retail_grind_spline_id"]),
            16,
        ),
        retail_type_signature=int(
            str(obj["skate3_retail_grind_type_signature"]),
            16,
        ),
        retail_flags=int(obj["skate3_retail_grind_flags"]),
        retail_trailing_word=int(
            obj["skate3_retail_grind_trailing_word"]
        ),
        native_segment_payload=payload,
    )


def _export_grinds(
    grind_objects: list[bpy.types.Object],
) -> list[ExportGrindRail]:
    rails: list[ExportGrindRail] = []
    for obj in grind_objects:
        if obj.type != "CURVE":
            continue
        if bool(obj.get("skate3_retail_grind", False)):
            rails.append(_export_retail_grind(obj))
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
            rails.append(
                ExportGrindRail(
                    name=name,
                    closed=bool(spline.use_cyclic_u),
                    points=points,
                )
            )
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
        mesh, evaluated = _mesh_for_export(
            source_object,
            depsgraph,
            preserve_all_data_layers=True,
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
            if evaluated is not None:
                evaluated.to_mesh_clear()
    return result


def export_scene(
    output_path: str | Path,
    *,
    force_rebuild: bool = False,
    metadata_only: bool = False,
    adopt_existing_cache: bool = False,
    progress: ProgressCallback | None = None,
) -> Path:
    started = time.perf_counter()
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    _report_progress(progress, 0.0, "Preparing export")

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
        _report_progress(progress, 1.0, "Metadata export complete")
        return output

    visual_objects = [
        obj
        for obj in _objects_from_collections(
            PRESENTATION_COLLISION_COLLECTION,
            NO_COLLISION_COLLECTION,
            LEGACY_VISUAL_COLLECTION,
        )
        if bool(obj.get("ow_export_visual", True))
        and not _is_helper_object(obj)
    ]
    static_visual_objects = _static_visual_objects(visual_objects)
    collision_objects = [
        obj
        for obj in _objects_from_collections(
            PRESENTATION_COLLISION_COLLECTION,
            NO_PRESENTATION_COLLECTION,
            LEGACY_COLLISION_COLLECTION,
        )
        if not _is_helper_object(obj)
    ]
    grind_objects = _objects_from_collections(
        GRIND_COLLECTION, LEGACY_GRIND_COLLECTION
    )
    npc_path_objects = _objects_from_collections(
        NPC_PATH_COLLECTION, LEGACY_NPC_PATH_COLLECTION
    )
    materials = _used_export_materials(visual_objects, collision_objects)
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

    # Collision is audited before cache checks so a previously cached package
    # cannot conceal newly-invalid source geometry. Harmless zero-area and
    # duplicate triangles are omitted from the package without modifying the
    # Blender mesh (or its visual UVs).
    _report_progress(progress, 0.05, "Auditing collision geometry")
    collision, _collision_audit = _export_collision(
        collision_objects,
        material_name_ids,
        progress=lambda fraction, stage: _report_progress(
            progress,
            0.05 + fraction * 0.20,
            stage,
        ),
    )
    _report_progress(progress, 0.25, "Collision geometry ready")

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
            len(collision),
            progress=lambda fraction, stage: _report_progress(
                progress,
                0.25 + fraction * 0.05,
                stage,
            ),
        )
        package_name, _, package_counts = _read_package_header(output)
        expected_name, _ = _scene_metadata(output)
        expected_counts_except_vertices = (
            len(export_materials),
            len(images),
            fingerprint.visual_indices,
            fingerprint.collision_triangles,
            fingerprint.grind_rails,
            fingerprint.hinged_doors,
            fingerprint.local_lights,
            fingerprint.npc_routes,
        )
        actual_counts_except_vertices = (
            package_counts[0],
            package_counts[1],
            *package_counts[3:],
        )
        if (
            package_name != expected_name
            or actual_counts_except_vertices
            != expected_counts_except_vertices
            or package_counts[2] == 0
            or package_counts[2] > fingerprint.visual_vertices
        ):
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
        _report_progress(progress, 1.0, "Cache adoption complete")
        return output

    if not force_rebuild and manifest is not None:
        fingerprint = _scene_content_fingerprint(
            visual_objects,
            collision_objects,
            grind_objects,
            npc_path_objects,
            materials,
            images,
            len(collision),
            progress=lambda fraction, stage: _report_progress(
                progress,
                0.25 + fraction * 0.05,
                stage,
            ),
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
            _report_progress(progress, 1.0, "Fast export complete")
            return output
        print(
            "SKATE cache: content changed; rebuilding package",
            flush=True,
        )

    geometry = _export_visual_geometry(
        static_visual_objects,
        material_name_ids,
        progress=lambda fraction, stage: _report_progress(
            progress,
            0.30 + fraction * 0.40,
            stage,
        ),
    )
    _report_progress(progress, 0.71, "Packing map metadata")
    rails = _export_grinds(grind_objects)
    npc_routes = _export_npc_routes(npc_path_objects)
    doors = _export_hinged_doors(visual_objects, material_name_ids)
    lights = _export_local_lights()

    spawn = bpy.data.objects.get(SPAWN_OBJECT)
    if spawn is None:
        raise ValueError(f"required spawn object is missing: {SPAWN_OBJECT}")
    spawn_position = _to_runtime(spawn.matrix_world.translation)
    heading = _spawn_heading(spawn)
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
            geometry.vertex_count,
            geometry.index_count,
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
                _bounded_int(material, "ow_physics_surface", 1, 13),
            )
            _write_u32(
                stream,
                _bounded_int(material, "ow_surface_pattern", 0, 15),
            )

        lightmap_encodings: dict[str, str] = {}
        for material in materials:
            lightmap_name = str(material.get("ow_lightmap_image", ""))
            if not lightmap_name:
                continue
            encoding = str(material.get("ow_lightmap_encoding", ""))
            previous = lightmap_encodings.setdefault(lightmap_name, encoding)
            if previous != encoding:
                raise ValueError(
                    f"lightmap image {lightmap_name!r} is referenced with "
                    "conflicting encodings"
                )
        for image_index, image in enumerate(images, start=1):
            lightmap_encoding = lightmap_encodings.get(image.name, "")
            rgba8 = _image_rgba8(
                image,
                lightmap=image.name in lightmap_encodings,
                lightmap_encoding=lightmap_encoding,
            )
            _require_nonblank_rgb(image.name, rgba8)
            _write_string(stream, image.name)
            _write_u32(stream, int(image.size[0]))
            _write_u32(stream, int(image.size[1]))
            # Authored bakes arrive scene-linear and use sqrt(linear / 4).
            # Retail lightmaps already contain that exact encoded quantity,
            # so their Non-Color bytes pass through without a second transfer.
            # Compression is lossless in both cases.
            _write_u32(stream, 0)
            _write_stored_bytes(stream, rgba8)
            _report_progress(
                progress,
                0.72
                + 0.06 * image_index / max(1, len(images)),
                f"Packing textures ({image_index}/{len(images)}): "
                f"{image.name}",
            )

        _write_stored_chunks(stream, geometry.vertex_chunks)
        _report_progress(progress, 0.86, "Compressed visual vertices")
        _write_stored_chunks(stream, geometry.index_chunks)
        _report_progress(progress, 0.89, "Compressed visual indices")
        packed_collision = struct.Struct("<9fII4B")
        collision_chunks: list[bytes] = []
        collision_buffer = bytearray()
        collision_flush_size = 16_384
        for collision_index, (
            a,
            b,
            c,
            surface_id,
            material_id,
            native_edge_codes,
        ) in enumerate(collision, start=1):
            edge_codes = native_edge_codes or (0, 0, 0)
            collision_buffer.extend(
                packed_collision.pack(
                    *a,
                    *b,
                    *c,
                    surface_id,
                    material_id,
                    *edge_codes,
                    1 if native_edge_codes is not None else 0,
                )
            )
            if (
                collision_index % collision_flush_size == 0
                or collision_index == len(collision)
            ):
                collision_chunks.append(bytes(collision_buffer))
                collision_buffer.clear()
                _report_progress(
                    progress,
                    0.89
                    + 0.05
                    * collision_index
                    / max(1, len(collision)),
                    f"Writing collision ({collision_index}/"
                    f"{len(collision)})",
                )
        _write_stored_chunks(stream, collision_chunks)
        for rail in rails:
            _write_string(stream, rail.name)
            _write_u32(stream, 1 if rail.closed else 0)
            if rail.native_segment_payload:
                _write_u32(stream, 1)
                _write_u64(stream, rail.retail_spline_id)
                _write_u64(stream, rail.retail_type_signature)
                _write_u32(stream, rail.retail_flags)
                _write_u32(stream, rail.retail_trailing_word)
                segment_count = len(rail.native_segment_payload) // 120
                _write_u32(stream, segment_count)
                for offset in range(
                    0,
                    len(rail.native_segment_payload),
                    4,
                ):
                    _write_u32(
                        stream,
                        int.from_bytes(
                            rail.native_segment_payload[offset : offset + 4],
                            "big",
                        ),
                    )
            else:
                _write_u32(stream, 0)
                _write_u32(stream, len(rail.points))
                for point in rail.points:
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
                stream.write(b"\0\0\0\0")
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
        f"triangles={geometry.index_count // 3}",
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
            len(collision),
            progress=lambda fraction, stage: _report_progress(
                progress,
                0.94 + fraction * 0.05,
                stage,
            ),
        )
    _write_cache_manifest(
        output, fingerprint, len(export_materials), len(images)
    )
    print(
        "SKATE cache: manifest updated",
        _cache_manifest_path(output),
        flush=True,
    )
    _report_progress(progress, 1.0, "Export complete")
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
