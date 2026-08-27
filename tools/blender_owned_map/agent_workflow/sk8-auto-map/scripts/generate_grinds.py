from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
import math

import bpy
from mathutils import Vector


GRIND_COLLECTION = "OW_GROUP_4_GRINDS"
GENERATED_PROPERTY = "sk8_auto_generated_grinds"
EXCLUDE_PROPERTY = "sk8_auto_grind_exclude"
GRIND_SOURCE_COLLECTIONS = {
    "OW_GROUP_1_PRESENTATION_COLLISION",
    "OW_GROUP_2_NO_PRESENTATION",
    "OW_COLLISION",
}
GRIND_OWNER_COLLECTIONS = {
    "OW_GROUP_1_PRESENTATION_COLLISION",
    "OW_VISUAL",
}
GRIND_COLLECTIONS = {
    GRIND_COLLECTION,
    "OW_GRIND",
}


@dataclass
class GrindSettings:
    minimum_segment_length: float = 0.35
    minimum_chain_length: float = 0.8
    minimum_corner_angle_degrees: float = 8.0
    maximum_slope_degrees: float = 65.0
    deduplicate_distance: float = 0.08
    join_distance: float = 0.04
    density_cell_size: float = 2.0
    maximum_splines_per_cell: int = 4
    maximum_splines_per_source: int = 128


@dataclass
class Segment:
    source: str
    start: Vector
    end: Vector

    @property
    def length(self) -> float:
        return (self.end - self.start).length


def _collection() -> bpy.types.Collection:
    collection = bpy.data.collections.get(GRIND_COLLECTION)
    if collection is None:
        collection = bpy.data.collections.new(GRIND_COLLECTION)
        bpy.context.scene.collection.children.link(collection)
    return collection


def _remove_previous(collection: bpy.types.Collection) -> None:
    for obj in list(collection.objects):
        if bool(obj.get(GENERATED_PROPERTY, False)):
            bpy.data.objects.remove(obj, do_unlink=True)


def _is_grind_source(obj: bpy.types.Object) -> bool:
    return any(
        collection.name in GRIND_SOURCE_COLLECTIONS
        for collection in obj.users_collection
    )


def _manual_grind_owner_names() -> set[str]:
    result: set[str] = set()
    for collection_name in GRIND_COLLECTIONS:
        collection = bpy.data.collections.get(collection_name)
        if collection is None:
            continue
        for obj in collection.all_objects:
            if (
                obj.type == "CURVE"
                and not bool(obj.get(GENERATED_PROPERTY, False))
                and obj.parent is not None
                and obj.parent.type == "MESH"
            ):
                result.add(obj.parent.name)
    return result


def _grind_owner(source: bpy.types.Object) -> bpy.types.Object | None:
    owner_name = str(source.get("ow_map_object_owner", "")).strip()
    owner = bpy.data.objects.get(owner_name) if owner_name else None
    if owner is not None and owner.type == "MESH":
        return owner
    if any(
        collection.name in GRIND_OWNER_COLLECTIONS
        for collection in source.users_collection
    ):
        return source
    return None


def _candidate_segments(settings: GrindSettings) -> list[Segment]:
    dependency_graph = bpy.context.evaluated_depsgraph_get()
    manual_owner_names = _manual_grind_owner_names()
    result: list[Segment] = []
    for source in sorted(
        (
            obj
            for obj in bpy.context.scene.objects
            if obj.type == "MESH"
            and obj.name != "OW_SPAWN"
            and not obj.hide_get()
            and not bool(obj.get(GENERATED_PROPERTY, False))
            and not bool(obj.get(EXCLUDE_PROPERTY, False))
            and _is_grind_source(obj)
            and (
                (_grind_owner(obj) or obj).name
                not in manual_owner_names
            )
        ),
        key=lambda item: item.name.casefold(),
    ):
        evaluated = source.evaluated_get(dependency_graph)
        mesh = evaluated.to_mesh()
        try:
            matrix = evaluated.matrix_world
            normal_matrix = matrix.to_3x3().inverted_safe().transposed()
            edge_faces: dict[tuple[int, int], list[int]] = defaultdict(list)
            world_normals = []
            for polygon in mesh.polygons:
                normal = (normal_matrix @ polygon.normal).normalized()
                world_normals.append(normal)
                for edge_key in polygon.edge_keys:
                    edge_faces[tuple(sorted(edge_key))].append(polygon.index)

            for edge in mesh.edges:
                start = matrix @ mesh.vertices[edge.vertices[0]].co
                end = matrix @ mesh.vertices[edge.vertices[1]].co
                delta = end - start
                length = delta.length
                if length < settings.minimum_segment_length:
                    continue
                slope = math.degrees(
                    math.asin(min(1.0, abs(delta.z) / max(length, 1.0e-9)))
                )
                if slope > settings.maximum_slope_degrees:
                    continue
                face_indices = edge_faces.get(tuple(sorted(edge.vertices)), [])
                normals = [world_normals[index] for index in face_indices]
                if normals and max(normal.z for normal in normals) < 0.12:
                    continue
                if len(normals) >= 2:
                    angle = math.degrees(normals[0].angle(normals[1]))
                    if angle < settings.minimum_corner_angle_degrees:
                        continue
                result.append(Segment(source.name, start.copy(), end.copy()))
        finally:
            evaluated.to_mesh_clear()
    return result


def _segments_overlap(
    left: Segment,
    right: Segment,
    distance: float,
) -> bool:
    left_delta = left.end - left.start
    right_delta = right.end - right.start
    if left_delta.length == 0.0 or right_delta.length == 0.0:
        return True
    if abs(left_delta.normalized().dot(right_delta.normalized())) < 0.985:
        return False
    direct = max(
        (left.start - right.start).length,
        (left.end - right.end).length,
    )
    reverse = max(
        (left.start - right.end).length,
        (left.end - right.start).length,
    )
    return min(direct, reverse) <= distance


def _deduplicate(
    segments: list[Segment],
    distance: float,
) -> list[Segment]:
    if distance <= 0.0:
        return segments
    cells: dict[tuple[int, int, int], list[Segment]] = defaultdict(list)
    kept: list[Segment] = []
    for segment in sorted(segments, key=lambda item: item.length, reverse=True):
        midpoint = (segment.start + segment.end) * 0.5
        cell = tuple(math.floor(value / distance) for value in midpoint)
        duplicate = False
        for x in range(cell[0] - 1, cell[0] + 2):
            for y in range(cell[1] - 1, cell[1] + 2):
                for z in range(cell[2] - 1, cell[2] + 2):
                    if any(
                        _segments_overlap(segment, other, distance)
                        for other in cells.get((x, y, z), ())
                    ):
                        duplicate = True
                        break
                if duplicate:
                    break
            if duplicate:
                break
        if duplicate:
            continue
        cells[cell].append(segment)
        kept.append(segment)
    return kept


def _point_key(point: Vector, distance: float) -> tuple[int, int, int]:
    return tuple(round(value / distance) for value in point)


def _chain_segments(
    segments: list[Segment],
    settings: GrindSettings,
) -> dict[str, list[list[Vector]]]:
    by_source: dict[str, list[Segment]] = defaultdict(list)
    for segment in segments:
        by_source[segment.source].append(segment)
    output: dict[str, list[list[Vector]]] = defaultdict(list)

    for source, source_segments in by_source.items():
        adjacency: dict[tuple[int, int, int], list[int]] = defaultdict(list)
        for index, segment in enumerate(source_segments):
            adjacency[_point_key(segment.start, settings.join_distance)].append(index)
            adjacency[_point_key(segment.end, settings.join_distance)].append(index)
        unused = set(range(len(source_segments)))

        def grow(start_index: int) -> list[Vector]:
            segment = source_segments[start_index]
            unused.remove(start_index)
            points = [segment.start.copy(), segment.end.copy()]

            def extend(at_end: bool) -> None:
                while True:
                    point = points[-1] if at_end else points[0]
                    key = _point_key(point, settings.join_distance)
                    choices = [index for index in adjacency[key] if index in unused]
                    if not choices:
                        return
                    previous = (
                        points[-1] - points[-2]
                        if at_end
                        else points[0] - points[1]
                    ).normalized()
                    best_index = max(
                        choices,
                        key=lambda index: abs(
                            previous.dot(
                                (
                                    source_segments[index].end
                                    - source_segments[index].start
                                ).normalized()
                            )
                        ),
                    )
                    next_segment = source_segments[best_index]
                    unused.remove(best_index)
                    if (next_segment.start - point).length <= (
                        next_segment.end - point
                    ).length:
                        next_point = next_segment.end
                    else:
                        next_point = next_segment.start
                    if at_end:
                        points.append(next_point.copy())
                    else:
                        points.insert(0, next_point.copy())

            extend(True)
            extend(False)
            return points

        while unused:
            chain = grow(next(iter(unused)))
            length = sum(
                (right - left).length for left, right in zip(chain, chain[1:])
            )
            if length >= settings.minimum_chain_length:
                output[source].append(chain)
    return output


def _chain_length(chain: list[Vector]) -> float:
    return sum(
        (right - left).length for left, right in zip(chain, chain[1:])
    )


def _limit_chain_density(
    chains: dict[str, list[list[Vector]]],
    settings: GrindSettings,
) -> tuple[dict[str, list[list[Vector]]], int]:
    if (
        settings.density_cell_size <= 0.0
        or settings.maximum_splines_per_cell <= 0
    ):
        return chains, 0

    candidates = [
        (source, chain, _chain_length(chain))
        for source, source_chains in chains.items()
        for chain in source_chains
    ]
    candidates.sort(
        key=lambda item: (
            -item[2],
            item[0].casefold(),
            tuple(round(float(value), 5) for value in item[1][0]),
            tuple(round(float(value), 5) for value in item[1][-1]),
        )
    )
    cell_counts: dict[tuple[int, int, int], int] = defaultdict(int)
    source_counts: dict[str, int] = defaultdict(int)
    kept: dict[str, list[list[Vector]]] = defaultdict(list)
    rejected = 0

    for source, chain, _length in candidates:
        midpoint = sum(chain, Vector()) / len(chain)
        cell = tuple(
            math.floor(float(value) / settings.density_cell_size)
            for value in midpoint
        )
        source_limit_reached = (
            settings.maximum_splines_per_source > 0
            and source_counts[source] >= settings.maximum_splines_per_source
        )
        if (
            cell_counts[cell] >= settings.maximum_splines_per_cell
            or source_limit_reached
        ):
            rejected += 1
            continue
        cell_counts[cell] += 1
        source_counts[source] += 1
        kept[source].append(chain)
    return kept, rejected


def generate_grinds(settings: GrindSettings | None = None) -> dict:
    settings = settings or GrindSettings()
    collection = _collection()
    _remove_previous(collection)
    candidates = _candidate_segments(settings)
    deduplicated = _deduplicate(candidates, settings.deduplicate_distance)
    raw_chains = _chain_segments(deduplicated, settings)
    raw_spline_count = sum(
        len(source_chains) for source_chains in raw_chains.values()
    )
    chains, density_rejected = _limit_chain_density(raw_chains, settings)
    spline_count = 0
    point_count = 0
    for source, source_chains in sorted(chains.items()):
        if not source_chains:
            continue
        curve = bpy.data.curves.new(f"SK8_AutoGrinds_{source}", "CURVE")
        curve.dimensions = "3D"
        obj = bpy.data.objects.new(curve.name, curve)
        obj[GENERATED_PROPERTY] = True
        obj["sk8_source_object"] = source
        collection.objects.link(obj)
        source_object = bpy.data.objects.get(source)
        owner = _grind_owner(source_object) if source_object else None
        if owner is not None:
            world_matrix = obj.matrix_world.copy()
            obj.parent = owner
            obj.matrix_world = world_matrix
        for chain in source_chains:
            spline = curve.splines.new("POLY")
            spline.points.add(len(chain) - 1)
            for target, point in zip(spline.points, chain):
                target.co = (*point, 1.0)
            spline_count += 1
            point_count += len(chain)
    return {
        "candidate_segments": len(candidates),
        "deduplicated_segments": len(deduplicated),
        "raw_splines": raw_spline_count,
        "density_rejected_splines": density_rejected,
        "splines": spline_count,
        "points": point_count,
    }
