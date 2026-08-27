"""Headless regression tests for automatic grind source and density limits."""

from pathlib import Path
import sys

import bpy
from mathutils import Vector


SCRIPT_ROOT = (
    Path(__file__).resolve().parent
    / "agent_workflow"
    / "sk8-auto-map"
    / "scripts"
)
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))

from generate_grinds import (  # noqa: E402
    GrindSettings,
    _candidate_segments,
    _limit_chain_density,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def make_plane(collection: bpy.types.Collection, name: str, x: float) -> None:
    mesh = bpy.data.meshes.new(f"{name}Mesh")
    mesh.from_pydata(
        [
            (x - 1.0, -1.0, 0.0),
            (x + 1.0, -1.0, 0.0),
            (x + 1.0, 1.0, 0.0),
            (x - 1.0, 1.0, 0.0),
        ],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()
    collection.objects.link(bpy.data.objects.new(name, mesh))


def main() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        bpy.data.collections.remove(collection)

    collision = bpy.data.collections.new(
        "OW_GROUP_1_PRESENTATION_COLLISION"
    )
    visual_only = bpy.data.collections.new("OW_GROUP_3_NO_COLLISION")
    bpy.context.scene.collection.children.link(collision)
    bpy.context.scene.collection.children.link(visual_only)
    make_plane(collision, "CollisionPlane", 0.0)
    make_plane(visual_only, "VisualPlane", 10.0)

    candidates = _candidate_segments(
        GrindSettings(
            minimum_segment_length=0.1,
            maximum_slope_degrees=90.0,
        )
    )
    require(len(candidates) == 4, f"Expected four boundary edges: {candidates}")
    require(
        {segment.source for segment in candidates} == {"CollisionPlane"},
        "Visual-only geometry was used as a grind source",
    )

    chains = {
        "DenseRail": [
            [Vector((0.0, 0.01 * index, 0.0)), Vector((length, 0.01 * index, 0.0))]
            for index, length in enumerate((1.0, 2.0, 3.0, 4.0))
        ]
    }
    limited, rejected = _limit_chain_density(
        chains,
        GrindSettings(
            density_cell_size=10.0,
            maximum_splines_per_cell=2,
            maximum_splines_per_source=128,
        ),
    )
    lengths = sorted(
        round((chain[1] - chain[0]).length, 4)
        for chain in limited["DenseRail"]
    )
    require(lengths == [3.0, 4.0], f"Density limit kept {lengths}")
    require(rejected == 2, f"Density limit rejected {rejected} chains")
    print("AUTO_MAP_GRINDS_OK", len(candidates), lengths, rejected)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback

        traceback.print_exc()
        sys.exit(1)
