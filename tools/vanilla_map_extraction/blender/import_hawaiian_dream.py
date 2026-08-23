"""Build and save Danny Way's Hawaiian Dream as a textured Blender scene."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys

import bpy
import numpy


def _safe_name(value: str, fallback: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_")
    return (cleaned or fallback)[:120]


def _runtime_to_blender(values: numpy.ndarray) -> numpy.ndarray:
    converted = values[:, [0, 2, 1]].copy()
    converted[:, 1] *= -1.0
    return converted


def _clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        bpy.data.collections.remove(collection)


def _new_child_collection(
    parent: bpy.types.Collection | bpy.types.Scene,
    name: str,
) -> bpy.types.Collection:
    collection = bpy.data.collections.new(name)
    if isinstance(parent, bpy.types.Scene):
        parent.collection.children.link(collection)
    else:
        parent.children.link(collection)
    return collection


def _load_materials(
    manifest: dict[str, object],
    cache_root: Path,
) -> dict[str, bpy.types.Material]:
    materials: dict[str, bpy.types.Material] = {}
    for texture_id, entry in manifest["textures"].items():
        image_path = cache_root / entry["png"]
        image = bpy.data.images.load(str(image_path), check_existing=True)
        image.name = texture_id

        material = bpy.data.materials.new(f"MAT_{texture_id}")
        material.use_nodes = True
        material.use_backface_culling = True
        nodes = material.node_tree.nodes
        links = material.node_tree.links
        principled = nodes.get("Principled BSDF")
        texture = nodes.new("ShaderNodeTexImage")
        texture.name = f"TEX_{texture_id}"
        texture.label = texture_id
        texture.image = image
        texture.interpolation = "Linear"
        links.new(texture.outputs["Color"], principled.inputs["Base Color"])
        links.new(texture.outputs["Alpha"], principled.inputs["Alpha"])
        principled.inputs["Roughness"].default_value = 0.68
        principled.inputs["Metallic"].default_value = 0.0
        if entry["format"] in {"DXT3", "DXT5"}:
            try:
                material.surface_render_method = "DITHERED"
            except Exception:
                try:
                    material.blend_method = "HASHED"
                except Exception:
                    pass
        material["skate3_texture_id"] = texture_id
        material["skate3_stream_asset_id"] = entry["stream_asset_id"]
        material["skate3_stream_file"] = entry["stream_file"]
        materials[texture_id] = material

    for texture_id in manifest["summary"]["unmatched_diffuse_textures"]:
        material = bpy.data.materials.new(f"MAT_{texture_id}_FALLBACK")
        material.use_nodes = True
        material.use_backface_culling = True
        principled = material.node_tree.nodes.get("Principled BSDF")
        principled.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        principled.inputs["Roughness"].default_value = 0.68
        material["skate3_texture_id"] = texture_id
        material["skate3_fallback_reason"] = (
            "Package references default_white but does not embed the shared texture."
        )
        materials[texture_id] = material
    return materials


def build_scene(manifest_path: Path) -> dict[str, int]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    cache_root = manifest_path.parent
    _clear_scene()

    scene = bpy.context.scene
    scene_name = manifest["map_name"]
    scene.name = scene_name
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = _new_child_collection(scene, scene_name)
    presentation = _new_child_collection(root, "Presentation")
    collision = _new_child_collection(root, "Collision_RAW")
    collision.hide_viewport = True
    collision.hide_render = True
    metadata = _new_child_collection(root, "Metadata")
    materials = _load_materials(manifest, cache_root)

    object_count = 0
    vertex_count = 0
    triangle_count = 0
    for model_entry in manifest["models"]:
        asset_id = model_entry["asset_id"]
        source_stem = Path(model_entry["stream_file"]).stem
        cell_collection = presentation.children.get(source_stem)
        if cell_collection is None:
            cell_collection = _new_child_collection(presentation, source_stem)
        asset_collection = _new_child_collection(
            cell_collection,
            f"MODEL_{asset_id[2:]}",
        )
        npz_path = cache_root / model_entry["npz"]
        with numpy.load(npz_path) as arrays:
            for mesh_entry in model_entry["meshes"]:
                mesh_index = mesh_entry["index"]
                vertices = _runtime_to_blender(arrays[f"vertices_{mesh_index}"])
                faces = arrays[f"faces_{mesh_index}"]
                uvs = (
                    arrays[f"uvs_{mesh_index}"]
                    if f"uvs_{mesh_index}" in arrays
                    else None
                )

                object_name = _safe_name(
                    mesh_entry["material_name"] or mesh_entry["name"],
                    f"{asset_id[2:]}_{mesh_index:02d}",
                )
                mesh_data = bpy.data.meshes.new(f"{object_name}_MESH")
                mesh_data.from_pydata(vertices.tolist(), [], faces.tolist())
                mesh_data.update(calc_edges=True)

                if uvs is not None:
                    uv_layer = mesh_data.uv_layers.new(name="UVMap")
                    for loop in mesh_data.loops:
                        uv = uvs[loop.vertex_index]
                        uv_layer.data[loop.index].uv = (
                            float(uv[0]),
                            1.0 - float(uv[1]),
                        )

                for polygon in mesh_data.polygons:
                    polygon.use_smooth = True

                obj = bpy.data.objects.new(object_name, mesh_data)
                asset_collection.objects.link(obj)
                texture_id = mesh_entry["texture_id"]
                if texture_id in materials:
                    mesh_data.materials.append(materials[texture_id])
                obj["skate3_asset_id"] = asset_id
                obj["skate3_stream_file"] = model_entry["stream_file"]
                obj["skate3_mesh_index"] = mesh_index
                obj["skate3_material_name"] = mesh_entry["material_name"] or ""
                obj["skate3_texture_id"] = texture_id or ""
                obj["skate3_vertex_stride"] = mesh_entry["vertex_stride"]
                obj["skate3_source_offsets"] = json.dumps(
                    mesh_entry["source_offsets"],
                    sort_keys=True,
                )
                object_count += 1
                vertex_count += len(vertices)
                triangle_count += len(faces)

    for simulation_entry in manifest["simulation_assets"]:
        empty = bpy.data.objects.new(
            f"SIM_{simulation_entry['asset_id'][2:]}",
            None,
        )
        empty.empty_display_type = "CUBE"
        empty.empty_display_size = 0.25
        empty.hide_render = True
        empty["skate3_asset_id"] = simulation_entry["asset_id"]
        empty["skate3_stream_file"] = simulation_entry["stream_file"]
        empty["skate3_rx2"] = simulation_entry["rx2"]
        empty["skate3_size"] = simulation_entry["size"]
        collision.objects.link(empty)

    info = bpy.data.objects.new(f"{scene_name} Import Status", None)
    info.empty_display_type = "PLAIN_AXES"
    info.empty_display_size = 2.0
    info["status"] = (
        "Presentation geometry and diffuse textures imported. "
        "Original presentation and simulation RX2 assets are preserved in the cache. "
        "Collision decoding is pending."
    )
    info["source_manifest"] = str(manifest_path)
    metadata.objects.link(info)

    scene["skate3_map_name"] = manifest["map_name"]
    scene["skate3_package_name"] = manifest["package_name"]
    scene["skate3_district_name"] = manifest["district_name"]
    scene["skate3_manifest"] = str(manifest_path)
    scene["skate3_import_format"] = manifest["format"]
    scene["skate3_collision_status"] = "raw assets preserved; decoder pending"
    return {
        "objects": object_count,
        "vertices": vertex_count,
        "triangles": triangle_count,
        "textures": len(materials),
        "simulation_assets": len(manifest["simulation_assets"]),
        "expected_objects": manifest["summary"]["mesh_parts"],
    }


def main() -> int:
    arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    if len(arguments) != 2:
        raise SystemExit(
            "usage: blender --background --python import_hawaiian_dream.py -- "
            "MANIFEST OUTPUT_BLEND"
        )
    manifest_path = Path(arguments[0]).resolve()
    output_blend = Path(arguments[1]).resolve()
    summary = build_scene(manifest_path)
    if summary["objects"] != summary["expected_objects"]:
        raise RuntimeError(
            f"created {summary['objects']} objects; "
            f"expected {summary['expected_objects']}"
        )
    output_blend.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(output_blend))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
