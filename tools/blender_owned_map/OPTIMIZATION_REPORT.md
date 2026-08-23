# SKATE package-size optimization

## Measured baseline

The tracked `maps/blender_bake_showcase.skate` is the reproducible SKATE v8
baseline. `analyze_skate.py` attributes its 8,931,154 bytes as follows:

| Section | v8 bytes | Share |
| --- | ---: | ---: |
| Embedded RGBA8 textures | 6,685,208 | 74.85% |
| Visual vertices | 1,989,504 | 22.28% |
| Sequential visual indices | 180,864 | 2.03% |
| Collision | 45,232 | 0.51% |
| Doors | 27,756 | 0.31% |
| All other records | 2,590 | 0.03% |

The v8 exporter wrote one complete 44-byte vertex for every triangle corner,
then wrote a redundant sequential index for each corner. It also expanded
every source image to uncompressed RGBA8. The runtime read those fields into
the same float32/u32 geometry and RGBA8 texture structures, so both costs were
package storage overhead rather than renderer requirements.

The runtime format has no object or instance table. Blender object transforms
are baked into world-space vertices, and the current University extraction
does not retain authoritative retail instance references. Instance support
would therefore require a renderer/runtime format change and new extraction
evidence; it is not a safe storage-only optimization for this pass.

## Implemented changes

- Exact complete-record vertex indexing. Position, normal, base UV, lightmap
  UV, and material ID must all match before corners share an index.
- SKATE v9 bounded storage records using zlib-wrapped DEFLATE.
- Independent lossless compression for each RGBA8 texture and for the visual
  vertex, visual index, and collision blocks.
- Raw texture fallback when compression would increase size.
- Loader allocation sizes inferred from validated dimensions/counts, with
  decoded-size and codec checks before normal map validation.
- Backward loader compatibility for SKATE v1 through v8.

No texture is resized or converted to a lossy GPU format. No float is
quantized. No triangle, UV seam, hard normal, material boundary, collision
triangle, light, rail, route, door, or transform is removed.

## Before and after

The same showcase `.blend`, exported in Blender 5.0.1:

| Metric | SKATE v8 | SKATE v9 | Change |
| --- | ---: | ---: | ---: |
| File bytes | 8,931,154 | 668,613 | -92.51% |
| Visual vertices | 45,216 | 29,330 | -35.13% |
| Visual indices | 45,216 | 45,216 | unchanged |
| Render triangles | 15,072 | 15,072 | unchanged |
| Decoded texture bytes | 6,684,672 | 6,684,672 | unchanged |
| Collision triangles | 1,028 | 1,028 | unchanged |
| Bounds minimum | -180, -0.64, -173 | -180, -0.64, -173 | unchanged |
| Bounds maximum | 180, 5.25, 163 | 180, 5.25, 163 | unchanged |

The semantic comparison expands both indexed meshes back into triangle-corner
records. Materials, decoded texture bytes, UVs, lightmap UVs, material IDs,
collision, authored features, counts, and bounds match. Re-evaluating the old
Blender scene changes some normalized normal components by at most one
float32 ULP (`1.1920929e-07`), below the comparator's `1e-6` limit.

The actual C++ `LoadOwnedMapPackage` loader accepts both files and reports the
same triangle, texture, collision, feature, and bounds data. The only runtime
count change is the intended removal of duplicate visual vertices.

## Reproduce

From the repository root:

```powershell
python tools/blender_owned_map/analyze_skate.py maps/blender_bake_showcase.skate

& "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe" `
  --background maps/blender_bake_showcase.blend `
  --python tools/blender_owned_map/export_skate.py -- `
  "$env:TEMP\blender_bake_showcase_v9.skate" --force

python tools/blender_owned_map/analyze_skate.py `
  "$env:TEMP\blender_bake_showcase_v9.skate"

python tools/blender_owned_map/compare_skate.py `
  maps/blender_bake_showcase.skate `
  "$env:TEMP\blender_bake_showcase_v9.skate"

.\out\owned-world-map-v9\skate_owned_map_validate.exe `
  "$env:TEMP\blender_bake_showcase_v9.skate"
```

The Blender addon workflow tests cover bulk and scalar geometry packing,
decoded-content equivalence, incremental export, material/texture handling,
and collision cleanup. The owned-world C++ tests cover the loader's existing
validation contracts and future-version rejection.
