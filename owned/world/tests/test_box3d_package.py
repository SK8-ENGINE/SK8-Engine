#!/usr/bin/env python3
"""Exercise SKATE14/MOBJ3 validation and legacy object compatibility."""

from __future__ import annotations

import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "blender_owned_map"))
from analyze_skate import PackageError, analyze_package  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def unpack_mobj(package: bytes) -> tuple[int, bytearray]:
    tag_offset = package.find(b"MOBJ")
    require(tag_offset >= 0, "sample has no MOBJ extension")
    schema = struct.unpack_from("<I", package, tag_offset + 4)[0]
    decoded_size, method, stored_size = struct.unpack_from(
        "<III", package, tag_offset + 8
    )
    require(schema == 3, "sample does not use MOBJ schema 3")
    stored_offset = tag_offset + 20
    stored = package[stored_offset : stored_offset + stored_size]
    require(len(stored) == stored_size, "sample MOBJ storage is truncated")
    if method == 0:
        payload = stored
    else:
        require(method == 1, "sample MOBJ storage method is unsupported")
        payload = zlib.decompress(stored)
    require(len(payload) == decoded_size, "sample MOBJ size is inconsistent")
    return tag_offset, bytearray(payload)


def physics_offsets(payload: bytearray) -> list[int]:
    offset = 0

    def u32() -> int:
        nonlocal offset
        value = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        return value

    result: list[int] = []
    count = u32()
    for _ in range(count):
        u32()
        name_size = u32()
        offset += name_size + 12 + 4 * 4
        grind_count = u32()
        offset += grind_count * 4
        result.append(offset)
        offset += 40
    require(offset == len(payload), "test parser did not consume MOBJ")
    return result


def repack_mobj(
    package: bytes,
    tag_offset: int,
    payload: bytes,
    *,
    schema: int = 3,
) -> bytes:
    stored = zlib.compress(payload, level=6)
    prefix = bytearray(package[: tag_offset + 4])
    prefix += struct.pack("<IIII", schema, len(payload), 1, len(stored))
    old_stored_size = struct.unpack_from("<I", package, tag_offset + 16)[0]
    suffix = package[tag_offset + 20 + old_stored_size :]
    return bytes(prefix) + stored + suffix

def unpack_bgrp(package: bytes) -> tuple[int, bytearray]:
    tag_offset = package.find(b"BGRP")
    require(tag_offset >= 0, "glass sample has no BGRP extension")
    schema = struct.unpack_from("<I", package, tag_offset + 4)[0]
    decoded_size, method, stored_size = struct.unpack_from(
        "<III", package, tag_offset + 8
    )
    require(schema == 1, "glass sample does not use BGRP schema 1")
    stored_offset = tag_offset + 20
    stored = package[stored_offset : stored_offset + stored_size]
    require(len(stored) == stored_size, "sample BGRP storage is truncated")
    payload = stored if method == 0 else zlib.decompress(stored)
    require(method in (0, 1), "sample BGRP storage method is unsupported")
    require(len(payload) == decoded_size, "sample BGRP size is inconsistent")
    return tag_offset, bytearray(payload)


def repack_bgrp(
    package: bytes,
    tag_offset: int,
    payload: bytes,
    *,
    schema: int = 1,
) -> bytes:
    stored = zlib.compress(payload, level=6)
    prefix = bytearray(package[: tag_offset + 4])
    prefix += struct.pack("<IIII", schema, len(payload), 1, len(stored))
    old_stored_size = struct.unpack_from("<I", package, tag_offset + 16)[0]
    suffix = package[tag_offset + 20 + old_stored_size :]
    return bytes(prefix) + stored + suffix


def expect_rejected(
    package: bytes,
    validator: Path,
    label: str,
) -> None:
    with tempfile.NamedTemporaryFile(
        suffix=".skateobj", delete=False
    ) as output:
        output.write(package)
        path = Path(output.name)
    try:
        try:
            analyze_package(path)
        except PackageError:
            pass
        else:
            raise AssertionError(f"analyzer accepted malformed {label}")
        completed = subprocess.run(
            [str(validator), str(path), "--object-profile"],
            check=False,
            capture_output=True,
            text=True,
        )
        require(
            completed.returncode != 0,
            f"runtime validator accepted malformed {label}",
        )
    finally:
        path.unlink(missing_ok=True)


def main() -> int:
    sample = Path(sys.argv[1]).resolve()
    validator = Path(sys.argv[2]).resolve()
    legacy_object = Path(sys.argv[3]).resolve()
    editable_map = Path(sys.argv[4]).resolve()
    glass_sample = Path(sys.argv[5]).resolve()

    analysis = analyze_package(sample)
    roots = analysis["map_objects"]
    require(analysis["version"] == 14, "sample is not SKATE14")
    require(len(roots) == 11, "sample does not contain 11 independent roots")
    require(
        roots[0]["physics"]["type"] == 1
        and all(root["physics"]["type"] == 2 for root in roots[1:]),
        "sample does not contain one static base and ten dynamic cubes",
    )
    require(
        [root["name"] for root in roots[1:]]
        == [f"Box3D_Cube_{index:02d}" for index in range(1, 11)],
        "sample cube roots are not deterministic",
    )
    runtime_sample = subprocess.run(
        [str(validator), str(sample), "--object-profile"],
        check=False,
        capture_output=True,
        text=True,
    )
    require(
        runtime_sample.returncode == 0
        and "SKATEOBJ_PHYSICS_OK static=1 dynamic=10"
        in runtime_sample.stdout,
        "Box3D-linked runtime did not create the sample's eleven bodies",
    )
    legacy_analysis = analyze_package(legacy_object)
    require(
        legacy_analysis["version"] == 12
        and len(legacy_analysis["map_objects"]) == 1
        and legacy_analysis["map_objects"][0]["physics"]["type"] == 0,
        "legacy SKATEOBJ did not default physics to disabled",
    )
    editable_map_analysis = analyze_package(editable_map)
    editable_map_grind_indices = {
        index
        for root in editable_map_analysis["map_objects"]
        for index in root["grind_indices"]
    }
    require(
        editable_map_analysis["version"] == 14
        and len(editable_map_analysis["map_objects"]) > 1
        and editable_map_analysis["counts"]["grind_rails"] > 3
        and editable_map_grind_indices
        == set(range(editable_map_analysis["counts"]["grind_rails"]))
        and all(
            root["physics"]["type"] == 0
            for root in editable_map_analysis["map_objects"]
        ),
        "editable SKATE14 map lost broad owned grind coverage "
        "or has unexpected object physics",
    )
    for package_path, profile in (
        (legacy_object, True),
        (editable_map, False),
    ):
        completed = subprocess.run(
            [
                str(validator),
                str(package_path),
                *(["--object-profile"] if profile else []),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        require(
            completed.returncode == 0,
            f"runtime validator rejected package {package_path}: "
            f"{completed.stderr}",
        )

    package = sample.read_bytes()
    tag_offset, payload = unpack_mobj(package)
    offsets = physics_offsets(payload)

    expect_rejected(
        repack_mobj(package, tag_offset, payload, schema=4),
        validator,
        "MOBJ schema",
    )

    invalid_enum = bytearray(payload)
    struct.pack_into("<I", invalid_enum, offsets[1], 99)
    expect_rejected(
        repack_mobj(package, tag_offset, invalid_enum),
        validator,
        "physics enum",
    )

    invalid_density = bytearray(payload)
    struct.pack_into("<f", invalid_density, offsets[1] + 8, math.nan)
    expect_rejected(
        repack_mobj(package, tag_offset, invalid_density),
        validator,
        "physics density",
    )

    expect_rejected(
        repack_mobj(package, tag_offset, payload[:-1]),
        validator,
        "truncated physics record",
    )

    glass_analysis = analyze_package(glass_sample)
    glass_roots = glass_analysis["map_objects"]
    breakable = [
        root for root in glass_roots
        if root["physics"]["break_group"] != 0
    ]
    require(
        glass_analysis["version"] == 14
        and len(glass_roots) == 48
        and len(breakable) == 48
        and glass_analysis["counts"]["textures"] == 2
        and [
            (
                texture["name"],
                texture["width"],
                texture["height"],
                texture["alpha_min"],
                texture["alpha_max"],
            )
            for texture in glass_analysis["texture_dimensions"]
        ]
        == [
            ("Glass_Uniform_RGBA", 1, 1, 71, 71),
            ("Glass_Hidden_RGBA", 1, 1, 0, 0),
        ]
        and all(root["physics"]["break_group"] == 1 for root in breakable)
        and all(root["physics"]["gravity_scale"] == 0.0 for root in breakable),
        "glass sample is not one uniformly shaded panel of 48 locked shards",
    )
    runtime_glass = subprocess.run(
        [str(validator), str(glass_sample), "--object-profile"],
        check=False,
        capture_output=True,
        text=True,
    )
    require(
        runtime_glass.returncode == 0
        and "SKATEOBJ_PHYSICS_OK static=0 dynamic=48"
        in runtime_glass.stdout,
        "Box3D-linked runtime did not create the glass sample bodies",
    )
    glass_package = glass_sample.read_bytes()
    bgrp_offset, bgrp_payload = unpack_bgrp(glass_package)
    expect_rejected(
        repack_bgrp(
            glass_package, bgrp_offset, bgrp_payload, schema=2
        ),
        validator,
        "BGRP schema",
    )
    invalid_group = bytearray(bgrp_payload)
    struct.pack_into("<I", invalid_group, 8, 0)
    expect_rejected(
        repack_bgrp(glass_package, bgrp_offset, invalid_group),
        validator,
        "zero break group",
    )
    invalid_reference = bytearray(bgrp_payload)
    struct.pack_into("<I", invalid_reference, 4, 0)
    expect_rejected(
        repack_bgrp(glass_package, bgrp_offset, invalid_reference),
        validator,
        "break group object reference",
    )

    print(
        "BOX3D_PACKAGE_TEST_PASS "
        "roots=11 dynamic=10 glass_shards=48 malformed_cases=7 "
        "legacy_skate=12 legacy_skateobj=12"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
