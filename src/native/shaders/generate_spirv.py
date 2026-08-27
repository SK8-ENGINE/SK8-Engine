#!/usr/bin/env python3
"""Regenerate selected offline Vulkan shader blobs with DXC."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
SHADER_ROOT = Path(__file__).resolve().parent
HEADER = SHADER_ROOT / "spirv" / "skate3_native_shaders_spirv.h"
ENTRY_PATTERN = re.compile(
    r'\{"([^"]+)", "([^"]+)", "([^"]*)", '
    r"(k_[A-Za-z0-9_]+), sizeof\(\4\)\}"
)


def find_dxc() -> Path:
    located = shutil.which("dxc")
    candidates = sorted(
        Path("C:/VulkanSDK").glob("*/Bin/dxc.exe"),
        reverse=True,
    )
    if located:
        candidates.append(Path(located))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError(
        "DXC with SPIR-V support was not found; install the Vulkan SDK"
    )


def compile_blob(
    dxc: Path,
    shader: str,
    entry: str,
    variant: str,
) -> bytes:
    stage = entry.split("_", 1)[0]
    if stage not in {"vs", "ps", "cs"}:
        raise RuntimeError(f"cannot infer shader stage for {entry!r}")
    with tempfile.TemporaryDirectory(
        prefix="skate-native-spirv-"
    ) as directory:
        output = Path(directory) / "shader.spv"
        command = [
            str(dxc),
            "-spirv",
            "-fspv-target-env=vulkan1.1",
            "-O3",
            "-T",
            f"{stage}_6_0",
            "-E",
            entry,
            "-Fo",
            str(output),
            "-I",
            str(SHADER_ROOT),
        ]
        for definition in filter(None, variant.split(";")):
            command.extend(["-D", definition])
        command.append(str(SHADER_ROOT / shader))
        subprocess.run(command, check=True)
        payload = output.read_bytes()
    if len(payload) % 4:
        raise RuntimeError(f"{shader}:{entry} emitted a partial word")
    return payload


def array_source(
    shader: str,
    entry: str,
    variant: str,
    name: str,
    payload: bytes,
) -> str:
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    suffix = f" [{variant}]" if variant else ""
    lines = [
        f"// {shader} : {entry}{suffix} ({len(payload)} bytes)",
        f"static const uint32_t {name}[] = {{",
    ]
    for first in range(0, len(words), 8):
        row = ", ".join(
            f"0x{word:08x}" for word in words[first : first + 8]
        )
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "shaders",
        nargs="*",
        default=["scene.hlsl"],
        help="Shader filenames to regenerate (default: scene.hlsl)",
    )
    arguments = parser.parse_args()
    selected = set(arguments.shaders)
    source = HEADER.read_text(encoding="utf-8")
    entries = [
        match.groups()
        for match in ENTRY_PATTERN.finditer(source)
        if match.group(1) in selected
    ]
    missing = selected - {entry[0] for entry in entries}
    if missing:
        raise RuntimeError(
            "no blob manifest entries for " + ", ".join(sorted(missing))
        )
    dxc = find_dxc()
    for shader, entry, variant, name in entries:
        payload = compile_blob(dxc, shader, entry, variant)
        replacement = array_source(
            shader, entry, variant, name, payload
        )
        pattern = re.compile(
            rf"// {re.escape(shader)} : {re.escape(entry)}[^\n]*\n"
            rf"static const uint32_t {re.escape(name)}\[\] = \{{.*?\n\}};",
            re.DOTALL,
        )
        source, count = pattern.subn(replacement, source, count=1)
        if count != 1:
            raise RuntimeError(f"could not replace generated array {name}")
        print(
            f"SPIRV {shader}:{entry} {variant or 'default'} "
            f"{len(payload)} bytes"
        )
    HEADER.write_text(source, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
