from __future__ import annotations

from array import array
from dataclasses import dataclass
import hashlib
import json
import re
from typing import Iterable

import bpy
from mathutils import Vector


SKATE_ROLES = {
    "OW_GROUP_1_PRESENTATION_COLLISION": "DEFAULT",
    "OW_GROUP_2_NO_PRESENTATION": "COLLISION_ONLY",
    "OW_GROUP_3_NO_COLLISION": "VISUAL_ONLY",
    "OW_GROUP_4_GRINDS": "GRIND",
    "OW_GROUP_5_PATHING": "PATH",
}
_BLENDER_SUFFIX = re.compile(r"\.\d{3}$")
_INSTANCE_SUFFIX = re.compile(r"\s+\(\d+\)$")


@dataclass
class ObjectGroup:
    group_id: str
    name_family: str
    signature: dict
    members: list[bpy.types.Object]

    @property
    def representative(self) -> bpy.types.Object:
        return self.members[0]


def normalized_name_family(name: str) -> str:
    result = str(name).strip()
    while result:
        previous = result
        result = _BLENDER_SUFFIX.sub("", result)
        result = _INSTANCE_SUFFIX.sub("", result).strip()
        if result == previous:
            break
    return result or str(name)


def current_skate_roles(obj: bpy.types.Object) -> list[str]:
    return sorted(
        {
            SKATE_ROLES[collection.name]
            for collection in obj.users_collection
            if collection.name in SKATE_ROLES
        }
    )


def material_names(obj: bpy.types.Object) -> list[str]:
    return [
        slot.material.name
        for slot in obj.material_slots
        if slot.material is not None
    ]


def world_bounds(obj: bpy.types.Object) -> dict | None:
    if not getattr(obj, "bound_box", None):
        return None
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
    return {
        "minimum": minimum,
        "maximum": maximum,
        "center": (minimum + maximum) * 0.5,
        "size": maximum - minimum,
    }


def _array_bytes(values: array) -> bytes:
    return values.tobytes()


def mesh_geometry_digest(
    mesh: bpy.types.Mesh,
    cache: dict[int, str] | None = None,
) -> str:
    cache_key = int(mesh.as_pointer())
    if cache is not None and cache_key in cache:
        return cache[cache_key]

    digest = hashlib.sha256()
    digest.update(
        (
            f"{len(mesh.vertices)}:{len(mesh.edges)}:"
            f"{len(mesh.polygons)}:{len(mesh.loops)}"
        ).encode("ascii")
    )
    coordinates = array("f", [0.0]) * (len(mesh.vertices) * 3)
    mesh.vertices.foreach_get("co", coordinates)
    digest.update(_array_bytes(coordinates))
    edge_vertices = array("i", [0]) * (len(mesh.edges) * 2)
    mesh.edges.foreach_get("vertices", edge_vertices)
    digest.update(_array_bytes(edge_vertices))
    loop_vertices = array("i", [0]) * len(mesh.loops)
    mesh.loops.foreach_get("vertex_index", loop_vertices)
    digest.update(_array_bytes(loop_vertices))
    for field in ("loop_start", "loop_total", "material_index"):
        values = array("i", [0]) * len(mesh.polygons)
        mesh.polygons.foreach_get(field, values)
        digest.update(_array_bytes(values))
    result = digest.hexdigest()
    if cache is not None:
        cache[cache_key] = result
    return result


def _collection_families(obj: bpy.types.Object) -> list[str]:
    return sorted(
        {
            normalized_name_family(collection.name).casefold()
            for collection in obj.users_collection
        }
    )


def _rounded_sorted_size(obj: bpy.types.Object) -> list[float] | None:
    bounds = world_bounds(obj)
    if bounds is None:
        return None
    return sorted(round(abs(float(value)), 3) for value in bounds["size"])


def _rigid_body_signature(obj: bpy.types.Object) -> dict | None:
    rigid_body = obj.rigid_body
    if rigid_body is None:
        return None
    return {
        "type": rigid_body.type,
        "collision_shape": rigid_body.collision_shape,
        "kinematic": bool(rigid_body.kinematic),
        "enabled": bool(rigid_body.enabled),
    }


def _modifier_signature(obj: bpy.types.Object) -> list[dict]:
    return [
        {
            "type": modifier.type,
            "show_viewport": bool(modifier.show_viewport),
            "show_render": bool(modifier.show_render),
        }
        for modifier in obj.modifiers
    ]


def _mesh_signature(
    obj: bpy.types.Object,
    geometry_cache: dict[int, str],
) -> dict:
    return {
        "type": "MESH",
        "name_family": normalized_name_family(obj.name).casefold(),
        "geometry": mesh_geometry_digest(obj.data, geometry_cache),
        "materials": material_names(obj),
        "world_size": _rounded_sorted_size(obj),
        "collection_families": _collection_families(obj),
        "hide_render": bool(obj.hide_render),
        "hidden_viewport": bool(obj.hide_get()),
        "current_skate_roles": current_skate_roles(obj),
        "rigid_body": _rigid_body_signature(obj),
        "modifiers": _modifier_signature(obj),
    }


def _auxiliary_data_signature(obj: bpy.types.Object) -> dict:
    if obj.type == "CURVE":
        return {
            "dimensions": obj.data.dimensions,
            "splines": len(obj.data.splines),
            "points": sum(
                len(spline.points)
                if spline.type == "POLY"
                else len(spline.bezier_points)
                for spline in obj.data.splines
            ),
        }
    if obj.type == "EMPTY":
        return {"display_type": obj.empty_display_type}
    if obj.type == "CAMERA":
        return {"camera_type": obj.data.type}
    if obj.type == "MESH":
        return {
            "vertices": len(obj.data.vertices),
            "polygons": len(obj.data.polygons),
        }
    return {}


def _auxiliary_signature(obj: bpy.types.Object) -> dict:
    return {
        "type": obj.type,
        "name_family": normalized_name_family(obj.name).casefold(),
        "world_size": _rounded_sorted_size(obj),
        "collection_families": _collection_families(obj),
        "hide_render": bool(obj.hide_render),
        "hidden_viewport": bool(obj.hide_get()),
        "current_skate_roles": current_skate_roles(obj),
        "rigid_body": _rigid_body_signature(obj),
        "data": _auxiliary_data_signature(obj),
    }


def _light_signature(obj: bpy.types.Object) -> dict:
    light = obj.data
    return {
        "type": light.type,
        "name_family": normalized_name_family(obj.name).casefold(),
        "collection_families": _collection_families(obj),
        "color": [round(float(value), 5) for value in light.color],
        "energy": round(float(light.energy), 4),
        "range": round(float(light.cutoff_distance), 4),
        "hide_render": bool(obj.hide_render),
        "hidden_viewport": bool(obj.hide_get()),
    }


def _group_id(prefix: str, signature_json: str) -> str:
    return f"{prefix}_{hashlib.sha256(signature_json.encode('utf-8')).hexdigest()[:20]}"


def _build_groups(
    objects: Iterable[bpy.types.Object],
    prefix: str,
    signature_builder,
) -> list[ObjectGroup]:
    grouped: dict[str, tuple[dict, list[bpy.types.Object]]] = {}
    for obj in sorted(objects, key=lambda item: item.name.casefold()):
        signature = signature_builder(obj)
        signature_json = json.dumps(
            signature,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        )
        if signature_json not in grouped:
            grouped[signature_json] = (signature, [])
        grouped[signature_json][1].append(obj)

    result = []
    seen_ids: dict[str, str] = {}
    for signature_json, (signature, members) in grouped.items():
        identifier = _group_id(prefix, signature_json)
        previous = seen_ids.get(identifier)
        if previous is not None and previous != signature_json:
            raise RuntimeError(f"scene group hash collision for {identifier}")
        seen_ids[identifier] = signature_json
        result.append(
            ObjectGroup(
                group_id=identifier,
                name_family=normalized_name_family(members[0].name),
                signature=signature,
                members=members,
            )
        )
    return sorted(result, key=lambda item: item.group_id)


def build_mesh_groups(
    objects: Iterable[bpy.types.Object],
) -> list[ObjectGroup]:
    geometry_cache: dict[int, str] = {}
    return _build_groups(
        (
            obj
            for obj in objects
            if obj.type == "MESH" and len(obj.data.polygons) > 0
        ),
        "mesh",
        lambda obj: _mesh_signature(obj, geometry_cache),
    )


def build_auxiliary_groups(
    objects: Iterable[bpy.types.Object],
) -> list[ObjectGroup]:
    return _build_groups(
        (
            obj
            for obj in objects
            if obj.type != "LIGHT"
            and not (
                obj.type == "MESH"
                and len(obj.data.polygons) > 0
            )
        ),
        "aux",
        _auxiliary_signature,
    )


def build_light_groups(
    objects: Iterable[bpy.types.Object],
) -> list[ObjectGroup]:
    return _build_groups(
        (obj for obj in objects if obj.type == "LIGHT"),
        "light",
        _light_signature,
    )
