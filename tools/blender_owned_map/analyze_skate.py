#!/usr/bin/env python3
"""Report the byte layout and integrity-relevant counts of a SKATE package."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path


VERTEX_BYTES = 44
VERTEX_BYTES_V12 = 56
COLLISION_TRIANGLE_BYTES_V10 = 44
COLLISION_TRIANGLE_BYTES_V11 = 48


class PackageError(ValueError):
    """Raised when a package cannot be parsed without guessing."""


@dataclass
class Reader:
    data: bytes
    offset: int = 0

    def take(self, size: int, label: str) -> bytes:
        if size < 0 or self.offset + size > len(self.data):
            raise PackageError(f"{label} extends past the end of the package")
        start = self.offset
        self.offset += size
        return self.data[start : self.offset]

    def u32(self, label: str) -> int:
        return struct.unpack("<I", self.take(4, label))[0]

    def u64(self, label: str) -> int:
        return struct.unpack("<Q", self.take(8, label))[0]

    def string(self, label: str) -> str:
        size = self.u32(f"{label} length")
        try:
            return self.take(size, label).decode("utf-8")
        except UnicodeDecodeError as error:
            raise PackageError(f"{label} is not valid UTF-8") from error

    def skip(self, size: int, label: str) -> None:
        self.take(size, label)

    def stored(self, expected_size: int, label: str) -> bytes:
        method = self.u32(f"{label} storage method")
        stored_size = self.u32(f"{label} stored size")
        stored = self.take(stored_size, f"{label} payload")
        if method == 0:
            if stored_size != expected_size:
                raise PackageError(
                    f"{label} raw size is {stored_size}; "
                    f"expected {expected_size}"
                )
            return stored
        if method != 1:
            raise PackageError(f"{label} uses unsupported method {method}")
        try:
            decoded = zlib.decompress(stored)
        except zlib.error as error:
            raise PackageError(f"{label} DEFLATE payload is invalid") from error
        if len(decoded) != expected_size:
            raise PackageError(
                f"{label} decoded to {len(decoded)} bytes; "
                f"expected {expected_size}"
            )
        return decoded


def _section(
    sections: dict[str, int],
    reader: Reader,
    name: str,
    start: int,
) -> None:
    sections[name] = reader.offset - start


def analyze_package(
    path: Path,
    *,
    include_payloads: bool = False,
) -> dict[str, object]:
    reader = Reader(path.read_bytes())
    sections: dict[str, int] = {}

    start = reader.offset
    magic = reader.take(8, "magic")
    if magic not in (
        b"SKATE08\0",
        b"SKATE09\0",
        b"SKATE10\0",
        b"SKATE11\0",
        b"SKATE12\0",
        b"SKATE13\0",
    ):
        raise PackageError(
            f"unsupported magic {magic!r}; expected SKATE v8-v13"
        )
    version = int(magic[5:7])
    if reader.u32("endian marker") != 0x12345678:
        raise PackageError("invalid endian marker")
    semantic_metadata_start = reader.offset
    map_name = reader.string("map name")
    # Spawn/environment metadata between the map name and count table.
    reader.skip(49 * 4, "map metadata")
    counts = struct.unpack("<9I", reader.take(9 * 4, "count table"))
    (
        material_count,
        texture_count,
        vertex_count,
        index_count,
        collision_count,
        rail_count,
        door_count,
        light_count,
        route_count,
    ) = counts
    semantic_metadata = reader.data[semantic_metadata_start : reader.offset - 36]
    _section(sections, reader, "header_and_map_metadata", start)

    start = reader.offset
    material_alpha_modes = {0: 0, 1: 0, 2: 0}
    material_depth_layers = {0: 0, 1: 0, 2: 0, 3: 0}
    retail_family_counts: dict[int, int] = {}
    retail_binding_count = 0
    retail_parameter_count = 0
    material_records: list[dict[str, object]] = []
    for index in range(material_count):
        name = reader.string(f"material {index} name")
        fields = reader.take(76, f"material {index} fields")
        alpha_mode = struct.unpack_from("<I", fields, 56)[0]
        if alpha_mode not in material_alpha_modes:
            raise PackageError(
                f"material {index} uses invalid alpha mode {alpha_mode}"
            )
        material_alpha_modes[alpha_mode] += 1
        record = {
                "id": index + 1,
                "name": name,
                "roughness": struct.unpack_from("<f", fields, 24)[0],
                "emissive_intensity": struct.unpack_from(
                    "<f", fields, 28
                )[0],
                "albedo_texture": struct.unpack_from("<I", fields, 32)[0],
                "lightmap_texture": struct.unpack_from("<I", fields, 36)[0],
                "baked_indirect_strength": struct.unpack_from(
                    "<f", fields, 40
                )[0],
                "normal_texture": struct.unpack_from("<I", fields, 44)[0],
                "orm_texture": struct.unpack_from("<I", fields, 48)[0],
                "emissive_texture": struct.unpack_from("<I", fields, 52)[0],
                "alpha_mode": alpha_mode,
                "alpha_cutoff": struct.unpack_from("<f", fields, 60)[0],
                "audio_surface": struct.unpack_from("<I", fields, 64)[0],
                "physics_surface": struct.unpack_from("<I", fields, 68)[0],
                "surface_pattern": struct.unpack_from("<I", fields, 72)[0],
        }
        depth_layer = (
            reader.u32(f"material {index} presentation depth layer")
            if version >= 13
            else 0
        )
        if depth_layer not in material_depth_layers:
            raise PackageError(
                f"material {index} uses invalid depth layer {depth_layer}"
            )
        material_depth_layers[depth_layer] += 1
        record["presentation_depth_layer"] = depth_layer
        if version >= 12:
            retail_enabled = reader.u32(
                f"material {index} retail enabled"
            ) != 0
            record["retail_enabled"] = retail_enabled
            if retail_enabled:
                record["retail_material_guid"] = reader.u64(
                    f"material {index} retail guid"
                )
                record["retail_material_handle"] = reader.u32(
                    f"material {index} retail handle"
                )
                record["retail_material_group_index"] = struct.unpack(
                    "<i",
                    reader.take(
                        4, f"material {index} retail group index"
                    ),
                )[0]
                record["retail_shader_name"] = reader.string(
                    f"material {index} retail shader"
                )
                family = reader.u32(
                    f"material {index} retail family"
                )
                record["retail_shader_family"] = family
                retail_family_counts[family] = (
                    retail_family_counts.get(family, 0) + 1
                )
                record["retail_render_flags"] = reader.u32(
                    f"material {index} retail flags"
                )
                binding_count = reader.u32(
                    f"material {index} retail binding count"
                )
                bindings = []
                for binding_index in range(binding_count):
                    bindings.append(
                        {
                            "semantic": reader.string(
                                f"material {index} binding "
                                f"{binding_index} semantic"
                            ),
                            "texture": reader.u32(
                                f"material {index} binding "
                                f"{binding_index} texture"
                            ),
                            "uv_set": reader.u32(
                                f"material {index} binding "
                                f"{binding_index} uv set"
                            ),
                            "address_u": reader.u32(
                                f"material {index} binding "
                                f"{binding_index} address u"
                            ),
                            "address_v": reader.u32(
                                f"material {index} binding "
                                f"{binding_index} address v"
                            ),
                        }
                    )
                record["retail_texture_bindings"] = bindings
                retail_binding_count += binding_count
                parameter_count = reader.u32(
                    f"material {index} retail parameter count"
                )
                parameters = {}
                for parameter_index in range(parameter_count):
                    parameter_name = reader.string(
                        f"material {index} parameter "
                        f"{parameter_index} name"
                    )
                    value_count = reader.u32(
                        f"material {index} parameter "
                        f"{parameter_index} value count"
                    )
                    parameters[parameter_name] = [
                        reader.string(
                            f"material {index} parameter "
                            f"{parameter_index} value {value_index}"
                        )
                        for value_index in range(value_count)
                    ]
                record["retail_parameters"] = parameters
                retail_parameter_count += parameter_count
                record["retail_source_metadata"] = reader.string(
                    f"material {index} retail source metadata"
                )
        material_records.append(record)
    material_bytes = reader.data[start : reader.offset]
    _section(sections, reader, "materials", start)

    start = reader.offset
    texture_decoded_bytes = 0
    texture_dimensions: list[dict[str, object]] = []
    texture_digest = hashlib.sha256()
    for index in range(texture_count):
        name = reader.string(f"texture {index} name")
        width = reader.u32(f"texture {index} width")
        height = reader.u32(f"texture {index} height")
        color_space = reader.u32(f"texture {index} color space")
        expected = width * height * 4
        if version >= 9:
            rgba8 = reader.stored(expected, f"texture {index}")
        else:
            byte_count = reader.u32(f"texture {index} byte count")
            if byte_count != expected:
                raise PackageError(
                    f"texture {index} has {byte_count} bytes; expected {expected}"
                )
            rgba8 = reader.take(byte_count, f"texture {index} RGBA8")
        encoded_name = name.encode("utf-8")
        texture_digest.update(struct.pack("<I", len(encoded_name)))
        texture_digest.update(encoded_name)
        texture_digest.update(struct.pack("<3I", width, height, color_space))
        texture_digest.update(rgba8)
        texture_decoded_bytes += len(rgba8)
        alpha = rgba8[3::4]
        texture_dimensions.append(
            {
                "id": index + 1,
                "name": name,
                "width": width,
                "height": height,
                "color_space": color_space,
                "decoded_bytes": len(rgba8),
                "alpha_min": min(alpha) if alpha else None,
                "alpha_max": max(alpha) if alpha else None,
                "transparent_texels": alpha.count(0),
                "opaque_texels": alpha.count(255),
            }
        )
    _section(sections, reader, "textures", start)

    start = reader.offset
    vertex_stride = VERTEX_BYTES_V12 if version >= 12 else VERTEX_BYTES
    vertex_byte_count = vertex_count * vertex_stride
    if version >= 9:
        vertex_bytes = reader.stored(
            vertex_byte_count, "visual vertex block"
        )
    else:
        vertex_bytes = reader.take(vertex_byte_count, "visual vertices")
    _section(sections, reader, "visual_vertices", start)

    start = reader.offset
    index_byte_count = index_count * 4
    if version >= 9:
        index_bytes = reader.stored(index_byte_count, "visual index block")
    else:
        index_bytes = reader.take(index_byte_count, "visual indices")
    if index_count:
        indices = struct.unpack(f"<{index_count}I", index_bytes)
        maximum_index = max(indices)
        if maximum_index >= vertex_count:
            raise PackageError(
                f"visual index {maximum_index} exceeds vertex count {vertex_count}"
            )
    else:
        indices = ()
        maximum_index = None
    _section(sections, reader, "visual_indices", start)

    start = reader.offset
    collision_triangle_bytes = (
        COLLISION_TRIANGLE_BYTES_V11
        if version >= 11
        else COLLISION_TRIANGLE_BYTES_V10
    )
    collision_byte_count = collision_count * collision_triangle_bytes
    if version >= 9:
        collision_bytes = reader.stored(
            collision_byte_count, "collision block"
        )
    else:
        collision_bytes = reader.take(
            collision_byte_count, "collision triangles"
        )
    _section(sections, reader, "collision", start)

    authored_features_start = reader.offset
    start = reader.offset
    grind_section_start = reader.offset
    native_grind_segments = 0
    for index in range(rail_count):
        reader.string(f"rail {index} name")
        reader.u32(f"rail {index} closed")
        representation = (
            reader.u32(f"rail {index} representation")
            if version >= 10
            else 0
        )
        if representation == 0:
            point_count = reader.u32(f"rail {index} point count")
            reader.skip(point_count * 12, f"rail {index} points")
        elif representation == 1 and version >= 10:
            reader.u64(f"rail {index} retail spline id")
            reader.u64(f"rail {index} retail type signature")
            reader.u32(f"rail {index} retail flags")
            reader.u32(f"rail {index} retail trailing word")
            segment_count = reader.u32(f"rail {index} segment count")
            reader.skip(
                segment_count * 120,
                f"rail {index} native segments",
            )
            native_grind_segments += segment_count
        else:
            raise PackageError(
                f"rail {index} uses unsupported representation "
                f"{representation}"
            )
    _section(sections, reader, "grind_rails", start)
    grind_section_bytes = reader.data[grind_section_start : reader.offset]

    start = reader.offset
    for index in range(door_count):
        reader.string(f"door {index} name")
        reader.skip(28 * 4, f"door {index} float fields")
        reader.u32(f"door {index} surface")
        door_vertex_count = reader.u32(f"door {index} vertex count")
        door_index_count = reader.u32(f"door {index} index count")
        door_collision_count = reader.u32(f"door {index} collision count")
        reader.skip(
            door_vertex_count * vertex_stride,
            f"door {index} vertices",
        )
        door_indices = reader.take(
            door_index_count * 4,
            f"door {index} indices",
        )
        if door_index_count:
            maximum_door_index = max(
                struct.unpack(f"<{door_index_count}I", door_indices)
            )
            if maximum_door_index >= door_vertex_count:
                raise PackageError(
                    f"door {index} index {maximum_door_index} exceeds "
                    f"vertex count {door_vertex_count}"
                )
        reader.skip(
            door_collision_count * collision_triangle_bytes,
            f"door {index} collision",
        )
    _section(sections, reader, "hinged_doors", start)

    start = reader.offset
    for index in range(light_count):
        reader.string(f"light {index} name")
        reader.skip(4 + 14 * 4, f"light {index} fields")
    _section(sections, reader, "local_lights", start)

    start = reader.offset
    for index in range(route_count):
        reader.string(f"route {index} name")
        reader.skip(4 + 4 + 4 + 4, f"route {index} fields")
        point_count = reader.u32(f"route {index} point count")
        reader.skip(point_count * 12, f"route {index} points")
    _section(sections, reader, "npc_routes", start)

    extension_tags: list[str] = []
    map_objects: list[dict[str, object]] = []
    if version >= 12:
        start = reader.offset
        extension_count = reader.u32("extension count")
        for extension_index in range(extension_count):
            tag = reader.take(
                4, f"extension {extension_index} tag"
            ).decode("ascii", errors="replace")
            schema = reader.u32(f"extension {extension_index} schema")
            decoded_size = reader.u32(
                f"extension {extension_index} decoded size"
            )
            payload = reader.stored(
                decoded_size, f"extension {extension_index}"
            )
            extension_tags.append(tag)
            if tag == "MOBJ" and schema in (1, 2):
                object_reader = Reader(payload)
                object_count = object_reader.u32("MOBJ object count")
                for object_index in range(object_count):
                    object_id = object_reader.u32(
                        f"MOBJ object {object_index} id"
                    )
                    name = object_reader.string(
                        f"MOBJ object {object_index} name"
                    )
                    origin = struct.unpack(
                        "<3f",
                        object_reader.take(
                            12, f"MOBJ object {object_index} origin"
                        ),
                    )
                    first_index = object_reader.u32(
                        f"MOBJ object {object_index} first index"
                    )
                    index_count = object_reader.u32(
                        f"MOBJ object {object_index} index count"
                    )
                    first_collision = object_reader.u32(
                        f"MOBJ object {object_index} first collision"
                    )
                    collision_count = object_reader.u32(
                        f"MOBJ object {object_index} collision count"
                    )
                    grind_indices = []
                    if schema >= 2:
                        grind_count = object_reader.u32(
                            f"MOBJ object {object_index} grind count"
                        )
                        grind_indices = [
                            object_reader.u32(
                                f"MOBJ object {object_index} grind {grind}"
                            )
                            for grind in range(grind_count)
                        ]
                    map_objects.append(
                        {
                            "id": object_id,
                            "name": name,
                            "origin": origin,
                            "first_index": first_index,
                            "index_count": index_count,
                            "first_collision": first_collision,
                            "collision_count": collision_count,
                            "grind_indices": grind_indices,
                        }
                    )
                if object_reader.offset != len(payload):
                    raise PackageError("MOBJ has trailing bytes")
        _section(sections, reader, "extensions", start)

    if reader.offset != len(reader.data):
        raise PackageError(
            f"package has {len(reader.data) - reader.offset} trailing bytes"
        )

    triangle_digest = hashlib.sha256()
    quantized_triangle_digest = hashlib.sha256()
    for index in indices:
        offset = index * vertex_stride
        record = vertex_bytes[offset : offset + vertex_stride]
        triangle_digest.update(record)
        if version >= 12:
            base_values = struct.unpack("<10fI2f4b", record)
            floats = (
                *base_values[:10],
                *base_values[11:13],
                *(value / 127.0 for value in base_values[13:17]),
            )
            material_id = base_values[10]
        else:
            base_values = struct.unpack("<10fI", record)
            floats = base_values[:10]
            material_id = base_values[10]
        quantized = [
            round(value * 1_000_000.0)
            for value in floats
        ]
        try:
            quantized_triangle_digest.update(
                struct.pack(
                    f"<{len(floats)}qI",
                    *quantized,
                    material_id,
                )
            )
        except struct.error as error:
            raise PackageError(
                f"vertex {index} contains values outside the analyzer's "
                f"quantized range: {floats!r}"
            ) from error
    positions = [
        struct.unpack_from("<3f", vertex_bytes, offset)
        for offset in range(0, len(vertex_bytes), vertex_stride)
    ]
    bounds_min = [
        min(position[axis] for position in positions) for axis in range(3)
    ]
    bounds_max = [
        max(position[axis] for position in positions) for axis in range(3)
    ]

    result = {
        "path": str(path.resolve()),
        "version": version,
        "map_name": map_name,
        "file_bytes": len(reader.data),
        "counts": {
            "materials": material_count,
            "textures": texture_count,
            "vertices": vertex_count,
            "indices": index_count,
            "collision_triangles": collision_count,
            "grind_rails": rail_count,
            "native_grind_segments": native_grind_segments,
            "hinged_doors": door_count,
            "local_lights": light_count,
            "npc_routes": route_count,
            "retail_texture_bindings": retail_binding_count,
            "retail_material_parameters": retail_parameter_count,
        },
        "sections": sections,
        "material_alpha_modes": {
            "opaque": material_alpha_modes[0],
            "mask": material_alpha_modes[1],
            "blend": material_alpha_modes[2],
        },
        "material_depth_layers": material_depth_layers,
        "retail_shader_families": retail_family_counts,
        "extension_tags": extension_tags,
        "map_objects": map_objects,
        "texture_decoded_bytes": texture_decoded_bytes,
        "texture_dimensions": texture_dimensions,
        "maximum_visual_index": maximum_index,
        "integrity": {
            "semantic_metadata_sha256": hashlib.sha256(
                semantic_metadata
            ).hexdigest(),
            "materials_sha256": hashlib.sha256(material_bytes).hexdigest(),
            "decoded_textures_sha256": texture_digest.hexdigest(),
            "expanded_visual_triangles_sha256": (
                triangle_digest.hexdigest()
            ),
            "expanded_visual_triangles_1e6_sha256": (
                quantized_triangle_digest.hexdigest()
            ),
            "collision_sha256": hashlib.sha256(
                collision_bytes
            ).hexdigest(),
            "grind_rails_sha256": hashlib.sha256(
                grind_section_bytes
            ).hexdigest(),
            "authored_features_sha256": hashlib.sha256(
                reader.data[authored_features_start : reader.offset]
            ).hexdigest(),
            "bounds_min": bounds_min,
            "bounds_max": bounds_max,
        },
    }
    if include_payloads:
        result["_materials"] = material_records
        result["_vertex_bytes"] = vertex_bytes
        result["_indices"] = indices
        result["_collision_bytes"] = collision_bytes
    return result


def _format_bytes(value: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(value)
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            return f"{amount:.2f} {unit}"
        amount /= 1024.0
    raise AssertionError("unreachable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument(
        "--json",
        action="store_true",
        help="write machine-readable JSON instead of a table",
    )
    args = parser.parse_args()
    result = analyze_package(args.package)
    if args.json:
        print(json.dumps(result, indent=2))
        return 0

    total = int(result["file_bytes"])
    print(f"{result['map_name']} — SKATE v{result['version']}")
    print(f"File: {_format_bytes(total)} ({total:,} bytes)")
    print()
    print(f"{'Section':<26} {'Bytes':>14} {'Share':>9}")
    for name, size_value in result["sections"].items():
        size = int(size_value)
        share = 100.0 * size / max(1, total)
        print(f"{name:<26} {size:>14,} {share:>8.2f}%")
    print()
    print("Counts:", json.dumps(result["counts"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
