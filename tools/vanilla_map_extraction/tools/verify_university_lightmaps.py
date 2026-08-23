#!/usr/bin/env python3
"""Verify exact retail University lightmaps through the SKATE package."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "blender_owned_map"))

from analyze_skate import Reader, VERTEX_BYTES  # noqa: E402


def _selected_lightmaps(
    manifest: dict[str, object],
) -> tuple[list[dict[str, object]], set[str], int, int]:
    selected: list[dict[str, object]] = []
    excluded_no_uv = 0
    excluded_shader = 0
    for model in manifest["models"]:
        for mesh in model["meshes"]:
            lightmap_id = str(
                mesh.get("retail_texture_ids", {}).get("lightmap", "")
            )
            if not lightmap_id:
                continue
            if not mesh.get("lightmap_uv"):
                excluded_no_uv += 1
                continue
            shader = str(mesh.get("shader_name") or "")
            if shader.startswith(("water.", "ocean.")):
                excluded_shader += 1
                continue
            if not mesh.get("texture_id"):
                raise RuntimeError(
                    "a selected lightmapped mesh has no base texture/material"
                )
            selected.append(mesh)
    return (
        selected,
        {
            str(mesh["retail_texture_ids"]["lightmap"])
            for mesh in selected
        },
        excluded_no_uv,
        excluded_shader,
    )


def _png_rgba_blender_order(path: Path) -> bytes:
    with Image.open(path) as image:
        rgba = image.convert("RGBA")
        width, height = rgba.size
        top_to_bottom = rgba.tobytes()
        row_bytes = width * 4
        # Blender exposes image.pixels bottom row first. The importer flips V
        # when it converts retail UVs, so this storage-order flip preserves
        # the exact sampled texel rather than altering the map orientation.
        return b"".join(
            top_to_bottom[row * row_bytes : (row + 1) * row_bytes]
            for row in range(height - 1, -1, -1)
        )


def _verify_package(
    package_path: Path,
    manifest: dict[str, object],
    selected_ids: set[str],
) -> dict[str, int]:
    reader = Reader(package_path.read_bytes())
    magic = reader.take(8, "magic")
    if magic != b"SKATE11\0":
        raise RuntimeError(f"expected SKATE11 package, found {magic!r}")
    if reader.u32("endian marker") != 0x12345678:
        raise RuntimeError("invalid SKATE endian marker")
    reader.string("map name")
    reader.skip(49 * 4, "map metadata")
    counts = struct.unpack("<9I", reader.take(9 * 4, "count table"))
    material_count, texture_count, vertex_count, index_count = counts[:4]

    materials: list[tuple[int, int]] = []
    for index in range(material_count):
        reader.string(f"material {index} name")
        fields = reader.take(76, f"material {index} fields")
        materials.append(
            (
                struct.unpack_from("<I", fields, 32)[0],
                struct.unpack_from("<I", fields, 36)[0],
            )
        )

    texture_names: dict[int, str] = {}
    exact_payloads = 0
    exact_bytes = 0
    for index in range(texture_count):
        texture_id = index + 1
        name = reader.string(f"texture {index} name")
        width = reader.u32(f"texture {index} width")
        height = reader.u32(f"texture {index} height")
        color_space = reader.u32(f"texture {index} color space")
        rgba8 = reader.stored(width * height * 4, f"texture {index}")
        texture_names[texture_id] = name
        if name not in selected_ids:
            continue
        if color_space != 0:
            raise RuntimeError(f"retail lightmap {name!r} is not linear data")
        entry = manifest["textures"][name]
        if (width, height) != (int(entry["width"]), int(entry["height"])):
            raise RuntimeError(f"retail lightmap {name!r} dimensions changed")
        source = _png_rgba_blender_order(
            Path(manifest["_manifest_root"]) / str(entry["png"])
        )
        if rgba8 != source:
            raise RuntimeError(
                f"retail lightmap {name!r} changed during Blender export"
            )
        exact_payloads += 1
        exact_bytes += len(rgba8)

    lightmap_material_ids = {
        material_id
        for material_id, (_albedo, lightmap) in enumerate(
            materials,
            start=1,
        )
        if lightmap != 0
    }
    package_lightmap_names = {
        texture_names[lightmap]
        for _albedo, lightmap in materials
        if lightmap != 0
    }
    if package_lightmap_names != selected_ids:
        missing = sorted(selected_ids - package_lightmap_names)
        extra = sorted(package_lightmap_names - selected_ids)
        raise RuntimeError(
            "package retail lightmap set differs from extraction: "
            f"missing={missing}, extra={extra}"
        )
    if exact_payloads != len(selected_ids):
        raise RuntimeError("not every retail lightmap payload was verified")

    vertex_bytes = reader.stored(
        vertex_count * VERTEX_BYTES,
        "visual vertex block",
    )
    index_bytes = reader.stored(index_count * 4, "visual index block")
    indices = struct.unpack(f"<{index_count}I", index_bytes)
    lightmap_vertices = 0
    distinct_uv_vertices = 0
    used_lightmap_material_ids: set[int] = set()
    for offset in range(0, len(vertex_bytes), VERTEX_BYTES):
        uv0 = struct.unpack_from("<2f", vertex_bytes, offset + 24)
        uv1 = struct.unpack_from("<2f", vertex_bytes, offset + 32)
        material_id = struct.unpack_from("<I", vertex_bytes, offset + 40)[0]
        if material_id not in lightmap_material_ids:
            continue
        if not all(math.isfinite(value) for value in (*uv0, *uv1)):
            raise RuntimeError("package contains non-finite lightmap UVs")
        if any(value < -0.0001 or value > 1.0001 for value in uv1):
            raise RuntimeError(
                f"package lightmap UV lies outside the retail range: {uv1}"
            )
        lightmap_vertices += 1
        distinct_uv_vertices += uv0 != uv1
        used_lightmap_material_ids.add(material_id)
    if used_lightmap_material_ids != lightmap_material_ids:
        raise RuntimeError("package contains an unused lightmapped material")

    lightmap_corners = 0
    for index in indices:
        material_id = struct.unpack_from(
            "<I",
            vertex_bytes,
            index * VERTEX_BYTES + 40,
        )[0]
        lightmap_corners += material_id in lightmap_material_ids
    if not lightmap_vertices or not distinct_uv_vertices or not lightmap_corners:
        raise RuntimeError("package did not transport usable lightmap UVs")

    return {
        "package_lightmap_texture_ids": len(package_lightmap_names),
        "package_lightmap_materials": len(lightmap_material_ids),
        "exact_payloads": exact_payloads,
        "exact_payload_bytes": exact_bytes,
        "package_lightmap_vertices": lightmap_vertices,
        "package_distinct_uv_vertices": distinct_uv_vertices,
        "package_lightmap_triangle_corners": lightmap_corners,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("--expected", type=Path)
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["_manifest_root"] = str(manifest_path.parent)
    selected, selected_ids, excluded_no_uv, excluded_shader = (
        _selected_lightmaps(manifest)
    )
    source_references = sum(
        bool(mesh.get("retail_texture_ids", {}).get("lightmap"))
        for model in manifest["models"]
        for mesh in model["meshes"]
    )
    result = {
        "status": "UNIVERSITY_RETAIL_LIGHTMAPS_VERIFIED",
        "source_lightmap_references": source_references,
        "source_lightmap_texture_ids": len(
            {
                str(mesh["retail_texture_ids"]["lightmap"])
                for model in manifest["models"]
                for mesh in model["meshes"]
                if mesh.get("retail_texture_ids", {}).get("lightmap")
            }
        ),
        "selected_mesh_parts": len(selected),
        "excluded_no_secondary_uv": excluded_no_uv,
        "excluded_shader_mesh_parts": excluded_shader,
        **_verify_package(
            args.package.resolve(),
            manifest,
            selected_ids,
        ),
    }
    if args.expected is not None:
        expected = json.loads(
            args.expected.resolve().read_text(encoding="utf-8")
        )["retail_lightmaps"]
        for name, value in result.items():
            if name == "status":
                continue
            if expected[name] != value:
                raise RuntimeError(
                    f"retail lightmap contract {name} changed: "
                    f"{value!r} != {expected[name]!r}"
                )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
