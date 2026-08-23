"""Prepare lossless RX2 assets and a Blender-friendly Hawaiian Dream cache."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

import numpy

from skate3_streams import (
    ASSET_TYPE_MODEL,
    ASSET_TYPE_TEXTURE,
    StreamAsset,
    load_district_stream,
)


DISTRICT_NAME = "DIST_DHS 32221"
TEXTURE_NAME_PATTERN = re.compile(rb"(0x[0-9A-Fa-f]{16})\.Texture")
MATERIAL_TEXTURE_PATTERN = re.compile(r"(0x[0-9A-Fa-f]{16})$")
ALPHA_BLEND_SHADERS = {
    "environment.reflective_trans",
    "environment.transparent",
}


def _default_workspace() -> Path:
    return Path(__file__).resolve().parents[1]


def _default_stream_directory(workspace: Path) -> Path:
    return (
        workspace
        / "raw"
        / "hawaiian_dream"
        / "extracted"
        / "data"
        / "content"
        / "world"
        / "stream"
        / DISTRICT_NAME
    )


def _write_rx2(output_root: Path, category: str, asset: StreamAsset) -> Path:
    target = output_root / "rx2" / category / f"{asset.record.asset_id:016X}.rx2"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(asset.data)
    return target


def _texture_id(data: bytes, asset_id: int) -> str:
    matches = TEXTURE_NAME_PATTERN.findall(data)
    if matches:
        return matches[-1].decode("ascii").lower()
    return f"asset_{asset_id:016x}"


def _material_texture_id(material_name: str | None) -> str | None:
    if not material_name:
        return None
    match = MATERIAL_TEXTURE_PATTERN.search(material_name)
    return match.group(1).lower() if match else None


def _group_material_parameters(
    parameters: object,
) -> list[dict[str, list[str]]]:
    """Recover the per-mesh material groups flattened by mdl_parser.

    A material starts at each ``Name`` parameter. Selecting the Nth value
    from a global list of ``diffuse`` parameters is unsafe because materials
    such as water have no diffuse channel; that shifts every later mesh onto
    the wrong texture.
    """
    groups: list[dict[str, list[str]]] = []
    current: dict[str, list[str]] | None = None
    for parameter in parameters:
        kind = str(parameter.kind)
        value = str(parameter.value)
        if kind == "Name":
            current = {}
            groups.append(current)
        if current is not None:
            current.setdefault(kind, []).append(value)
    return groups


def _first_parameter(
    group: dict[str, list[str]],
    name: str,
) -> str | None:
    values = group.get(name, [])
    return values[0] if values and values[0] else None


def _material_metadata(
    groups: list[dict[str, list[str]]],
    mesh_index: int,
) -> dict[str, object]:
    group = groups[mesh_index] if mesh_index < len(groups) else {}
    diffuse = _first_parameter(group, "diffuse")
    transparent = _first_parameter(group, "transparent")
    shader_name = _first_parameter(group, "AttribulatorMaterialName")
    material_name = diffuse or transparent
    shader_key = (shader_name or "").lower()
    if shader_key in ALPHA_BLEND_SHADERS:
        alpha_mode = 2
    elif (
        transparent is not None
        or "alphatest" in shader_key
        or shader_key in {"animated.tree", "tree.default"}
    ):
        alpha_mode = 1
    else:
        alpha_mode = 0
    return {
        "material_name": material_name,
        "texture_id": _material_texture_id(material_name),
        "texture_channel": (
            "diffuse"
            if diffuse is not None
            else "transparent"
            if transparent is not None
            else None
        ),
        "shader_name": shader_name,
        "alpha_mode": alpha_mode,
        "alpha_cutoff": 0.5,
    }


def prepare(
    stream_directory: Path,
    output_root: Path,
    utt_root: Path,
    *,
    district_name: str = DISTRICT_NAME,
    map_name: str = "Danny Way's Hawaiian Dream",
    package_name: str = "DHS by DH13",
    cache_format: str = "skate3-hawaiian-dream-cache-v1",
    texture_stream_names: tuple[str, ...] = (),
) -> Path:
    sys.path.insert(0, str(utt_root))
    import rx2_parser
    from mdl_parser import parse_rx2 as parse_model

    output_root.mkdir(parents=True, exist_ok=True)
    presentation_assets = load_district_stream(
        stream_directory,
        "Pres",
        district_name,
    )
    simulation_assets = load_district_stream(
        stream_directory,
        "Sim",
        district_name,
    )
    extra_texture_assets = [
        asset
        for stream_name in texture_stream_names
        for asset in load_district_stream(
            stream_directory,
            stream_name,
            district_name,
        )
    ]

    manifest: dict[str, object] = {
        "format": cache_format,
        "map_name": map_name,
        "package_name": package_name,
        "district_name": district_name,
        "source_stream_directory": str(stream_directory),
        "axis_conversion": {
            "runtime_to_blender": ["x", "-z", "y"],
            "uv_v_flipped": True,
        },
        "textures": {},
        "models": [],
        "simulation_assets": [],
        "other_presentation_assets": [],
    }
    textures: dict[str, object] = manifest["textures"]  # type: ignore[assignment]
    models: list[object] = manifest["models"]  # type: ignore[assignment]
    simulation: list[object] = manifest["simulation_assets"]  # type: ignore[assignment]
    other: list[object] = manifest["other_presentation_assets"]  # type: ignore[assignment]
    texture_digests: dict[str, set[bytes]] = {}

    for asset in presentation_assets + extra_texture_assets:
        asset_id = asset.record.asset_id
        source_file = asset.source_path.name
        if asset.record.asset_type == ASSET_TYPE_TEXTURE:
            rx2_path = _write_rx2(output_root, "textures", asset)
            base_texture_id = _texture_id(asset.data, asset_id)
            parsed = rx2_parser.parse_rx2(asset.data)
            if not parsed.textures:
                raise RuntimeError(
                    f"texture asset 0x{asset_id:016X} contains no decoded texture"
                )
            for texture_index, texture in enumerate(parsed.textures):
                texture_id = (
                    base_texture_id
                    if texture_index == 0
                    else f"{base_texture_id}_{texture_index}"
                )
                png_path = (
                    output_root
                    / "textures"
                    / "by_asset"
                    / f"{asset_id:016X}_{texture_index}.png"
                )
                png_path.parent.mkdir(parents=True, exist_ok=True)
                texture.save_png(png_path)
                new_entry = {
                    "stream_asset_id": f"0x{asset_id:016X}",
                    "stream_file": source_file,
                    "rx2": str(rx2_path.relative_to(output_root)),
                    "png": str(png_path.relative_to(output_root)),
                    "texture_index": texture_index,
                    "width": texture.width,
                    "height": texture.height,
                    "format": texture.fmt_name,
                    "warnings": list(parsed.warnings),
                }
                texture_digest = hashlib.sha256(texture.rgba).digest()
                known_digests = texture_digests.setdefault(texture_id, set())
                if texture_id in textures:
                    existing = textures[texture_id]
                    if texture_digest in known_digests:
                        existing.setdefault("alternate_stream_assets", []).append(  # type: ignore[union-attr]
                            new_entry
                        )
                    elif texture.width * texture.height > (  # type: ignore[index]
                        existing["width"] * existing["height"]  # type: ignore[index,operator]
                    ):
                        previous_entry = {
                            key: value
                            for key, value in existing.items()  # type: ignore[union-attr]
                            if key not in {"variants", "alternate_stream_assets"}
                        }
                        new_entry["variants"] = list(  # type: ignore[index]
                            existing.get("variants", [])  # type: ignore[union-attr]
                        ) + [previous_entry]
                        if existing.get("alternate_stream_assets"):  # type: ignore[union-attr]
                            new_entry["alternate_stream_assets"] = existing[  # type: ignore[index]
                                "alternate_stream_assets"
                            ]
                        textures[texture_id] = new_entry
                    else:
                        existing.setdefault("variants", []).append(new_entry)  # type: ignore[union-attr]
                    known_digests.add(texture_digest)
                    continue

                known_digests.add(texture_digest)
                textures[texture_id] = new_entry
        elif asset.record.asset_type == ASSET_TYPE_MODEL:
            rx2_path = _write_rx2(output_root, "models", asset)
            parsed = parse_model(
                asset.data,
                source_path=rx2_path,
                strict=False,
            )
            npz_path = output_root / "models" / f"{asset_id:016X}.npz"
            npz_path.parent.mkdir(parents=True, exist_ok=True)
            arrays: dict[str, numpy.ndarray] = {}
            mesh_entries: list[dict[str, object]] = []
            material_groups = _group_material_parameters(parsed.materials)
            for mesh_index, mesh in enumerate(parsed.meshes):
                material = _material_metadata(material_groups, mesh_index)
                arrays[f"vertices_{mesh_index}"] = numpy.asarray(
                    mesh.vertices,
                    dtype=numpy.float32,
                )
                arrays[f"faces_{mesh_index}"] = numpy.asarray(
                    mesh.faces,
                    dtype=numpy.uint32,
                )
                if mesh.uvs is not None:
                    arrays[f"uvs_{mesh_index}"] = numpy.asarray(
                        mesh.uvs,
                        dtype=numpy.float32,
                    )
                if mesh.normals is not None:
                    arrays[f"normals_{mesh_index}"] = numpy.asarray(
                        mesh.normals,
                        dtype=numpy.float32,
                    )
                minimum, maximum = mesh.bounds
                mesh_entries.append(
                    {
                        "index": mesh_index,
                        "name": mesh.name,
                        **material,
                        "vertex_count": mesh.vertex_count,
                        "triangle_count": mesh.triangle_count,
                        "vertex_stride": mesh.vertex_stride,
                        "attributes": [
                            {
                                "semantic": attribute.semantic,
                                "offset": attribute.offset,
                                "data_type": attribute.data_type,
                                "components": attribute.components,
                                "descriptor": attribute.descriptor.hex(),
                            }
                            for attribute in mesh.attributes
                        ],
                        "source_offsets": dict(mesh.source_offsets),
                        "bounds": {
                            "minimum": minimum.tolist(),
                            "maximum": maximum.tolist(),
                        },
                    }
                )
            numpy.savez_compressed(npz_path, **arrays)
            minimum, maximum = parsed.bounds
            models.append(
                {
                    "asset_id": f"0x{asset_id:016X}",
                    "stream_file": source_file,
                    "source_offset": asset.source_offset,
                    "rx2": str(rx2_path.relative_to(output_root)),
                    "npz": str(npz_path.relative_to(output_root)),
                    "bounds": {
                        "minimum": minimum.tolist(),
                        "maximum": maximum.tolist(),
                    },
                    "vertex_count": parsed.vertex_count,
                    "triangle_count": parsed.triangle_count,
                    "warnings": list(parsed.warnings),
                    "meshes": mesh_entries,
                }
            )
        else:
            rx2_path = _write_rx2(output_root, "presentation_other", asset)
            other.append(
                {
                    "asset_id": f"0x{asset_id:016X}",
                    "asset_type": f"0x{asset.record.asset_type:08X}",
                    "stream_file": source_file,
                    "source_offset": asset.source_offset,
                    "rx2": str(rx2_path.relative_to(output_root)),
                }
            )

    for asset in simulation_assets:
        rx2_path = _write_rx2(output_root, "simulation", asset)
        simulation.append(
            {
                "asset_id": f"0x{asset.record.asset_id:016X}",
                "asset_type": f"0x{asset.record.asset_type:08X}",
                "stream_file": asset.source_path.name,
                "source_offset": asset.source_offset,
                "size": len(asset.data),
                "rx2": str(rx2_path.relative_to(output_root)),
            }
        )

    used_texture_ids = {
        mesh["texture_id"]
        for model in models
        for mesh in model["meshes"]  # type: ignore[index]
        if mesh["texture_id"] is not None
    }
    manifest["summary"] = {
        "presentation_assets": len(presentation_assets),
        "model_assets": len(models),
        "mesh_parts": sum(len(model["meshes"]) for model in models),  # type: ignore[arg-type,index]
        "vertices": sum(model["vertex_count"] for model in models),  # type: ignore[arg-type]
        "triangles": sum(model["triangle_count"] for model in models),  # type: ignore[arg-type]
        "texture_stream_assets": sum(
            asset.record.asset_type == ASSET_TYPE_TEXTURE
            for asset in presentation_assets + extra_texture_assets
        ),
        "decoded_textures": len(textures),
        "used_diffuse_textures": len(used_texture_ids),
        "simulation_assets": len(simulation_assets),
        "unmatched_diffuse_textures": sorted(used_texture_ids - set(textures)),
    }

    manifest_path = output_root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def main(argv: list[str] | None = None) -> int:
    workspace = _default_workspace()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stream-dir",
        type=Path,
        default=_default_stream_directory(workspace),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=workspace / "intermediate" / "hawaiian_dream",
    )
    parser.add_argument(
        "--utt-root",
        type=Path,
        default=Path(r"C:\Users\Daddy\Documents\Skate3Research\UTT-1.1.7"),
    )
    args = parser.parse_args(argv)
    manifest_path = prepare(
        stream_directory=args.stream_dir.resolve(),
        output_root=args.output.resolve(),
        utt_root=args.utt_root.resolve(),
    )
    print(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    print(json.dumps(manifest["summary"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
