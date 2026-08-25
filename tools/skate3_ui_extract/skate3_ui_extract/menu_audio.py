from __future__ import annotations

import hashlib
import json
import math
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .big import BigArchive
from .binary import FormatError


_COLLECTION_EXPRESSION = 0xAD303B8F42B3307E
_FE_CLASS = 0x5831CB95F3E90598  # lookup8("fe")
_SK8_MENU_TYPE = 0x795A5293DFD8B507  # lookup8("sk8_menu")
_EVENT_NAMES = (
    "core_nav_up",
    "core_nav_down",
    "crossbar_up",
    "crossbar_down",
    "core_a_button",
    "core_b_button",
    "core_fade",
    "crossbar_in",
    "crossbar_out",
    "core_popup",
)
_NORMAL_GRAIN_TAG = bytes.fromhex("00 01 00 00 01 01 00 00")


def lookup8(value: str) -> int:
    """The exact 64-bit VLT string hash used by Skate 3."""

    mask = (1 << 64) - 1

    def unsigned(number: int) -> int:
        return number & mask

    def mix(a: int, b: int, c: int) -> tuple[int, int, int]:
        a = unsigned(a - b - c)
        a ^= c >> 43
        b = unsigned(b - c - a)
        b ^= unsigned(a << 9)
        c = unsigned(c - a - b)
        c ^= b >> 8
        a = unsigned(a - b - c)
        a ^= c >> 38
        b = unsigned(b - c - a)
        b ^= unsigned(a << 23)
        c = unsigned(c - a - b)
        c ^= b >> 5
        a = unsigned(a - b - c)
        a ^= c >> 35
        b = unsigned(b - c - a)
        b ^= unsigned(a << 49)
        c = unsigned(c - a - b)
        c ^= b >> 11
        a = unsigned(a - b - c)
        a ^= c >> 12
        b = unsigned(b - c - a)
        b ^= unsigned(a << 18)
        c = unsigned(c - a - b)
        c ^= b >> 22
        return unsigned(a), unsigned(b), unsigned(c)

    data = value.encode("utf-8")
    length = len(data)
    a = b = 0xABCDEF0011223344
    c = 0x9E3779B97F4A7C13
    cursor = 0
    while length - cursor >= 24:
        a = unsigned(a + int.from_bytes(data[cursor : cursor + 8], "little"))
        b = unsigned(b + int.from_bytes(data[cursor + 8 : cursor + 16], "little"))
        c = unsigned(c + int.from_bytes(data[cursor + 16 : cursor + 24], "little"))
        a, b, c = mix(a, b, c)
        cursor += 24
    c = unsigned(c + length)
    for index, byte in enumerate(data[cursor:]):
        if index < 8:
            a = unsigned(a + (byte << (index * 8)))
        elif index < 16:
            b = unsigned(b + (byte << ((index - 8) * 8)))
        else:
            c = unsigned(c + (byte << ((index - 16) * 8)))
    return mix(a, b, c)[2]


def _chunk(data: bytes, tag: bytes) -> tuple[int, int]:
    cursor = 0
    while cursor + 8 <= len(data):
        size = struct.unpack_from(">I", data, cursor + 4)[0]
        if size < 8 or cursor + size > len(data):
            raise FormatError(f"malformed VLT chunk at {cursor:#x}")
        if data[cursor : cursor + 4] == tag:
            return cursor, size
        cursor += size
    raise FormatError(f"VLT chunk {tag.decode('ascii')} was not found")


def _entry(archive: BigArchive, suffix: str) -> bytes:
    matches = [entry for entry in archive.entries if entry.path.endswith(suffix)]
    if len(matches) != 1:
        raise FormatError(
            f"{archive.path}: expected one {suffix!r} entry, found {len(matches)}"
        )
    return archive.read(matches[0])


def _menu_event_identity(
    collection: bytes, wanted: dict[int, str]
) -> tuple[str, int, int, int] | None:
    # Most fe/sk8_menu collections store their object and class hashes at
    # +0x10/+0x18. Inherited collections retain an additional identity pair at
    # +0x40/+0x48; crossbar_up uses that retail layout.
    for offset in (0x10, 0x40):
        if offset + 16 > len(collection):
            continue
        event_hash, class_hash = struct.unpack_from(">QQ", collection, offset)
        event_name = wanted.get(event_hash)
        if event_name is not None and class_hash == _FE_CLASS:
            return event_name, event_hash, class_hash, offset
    return None


def _pointer_relocations(
    vlt: bytes, pointer_start: int, pointer_size: int
) -> dict[int, dict[int, int]]:
    relocations: dict[int, dict[int, int]] = {}
    section = -1
    pointer_count = (pointer_size - 8) // 16
    for index in range(pointer_count):
        source, relocation_type, identifier, destination = struct.unpack_from(
            ">IHHQ", vlt, pointer_start + 8 + index * 16
        )
        if relocation_type == 2:
            section = identifier
        elif relocation_type == 3 and section >= 0:
            relocations.setdefault(section, {})[source] = destination
    return relocations


def _menu_symbol_rows(
    bin_data: bytes,
    symbol_relocations: dict[int, int],
    wanted: set[str],
) -> dict[str, dict]:
    """Resolve serialized fe_sfx rows through PtrN's BIN symbol section."""

    rows: dict[str, dict] = {}
    for source, destination in symbol_relocations.items():
        if source + 12 > len(bin_data) or destination >= len(bin_data):
            continue
        end = bin_data.find(b"\0", destination)
        if end < 0:
            continue
        try:
            name = bin_data[destination:end].decode("ascii")
        except UnicodeDecodeError:
            continue
        if name not in wanted:
            continue
        if name in rows:
            raise FormatError(f"{name}: duplicate VLT menu-audio symbol")
        reserved, volume, sound_enum = struct.unpack_from(">IfI", bin_data, source)
        rows[name] = {
            "bin_data_offset": f"{source:#x}",
            "symbol_string_offset": f"{destination:#x}",
            "reserved": reserved,
            "volume": volume,
            "sound_enum": sound_enum,
        }
    return rows


def _menu_event_rows(db_archive: Path) -> dict[str, dict]:
    archive = BigArchive(db_archive)
    vlt = _entry(archive, "data/db/skatercollections.vlt")
    bin_data = _entry(archive, "data/db/skatercollections.bin")
    dat_start, _ = _chunk(vlt, b"DatN")
    expression_start, expression_size = _chunk(vlt, b"ExpN")
    pointer_start, pointer_size = _chunk(vlt, b"PtrN")

    wanted = {lookup8(name): name for name in _EVENT_NAMES}
    relocations = _pointer_relocations(vlt, pointer_start, pointer_size)
    symbol_rows = _menu_symbol_rows(
        bin_data, relocations.get(1, {}), set(_EVENT_NAMES)
    )
    identities: dict[str, dict] = {}
    expression_count = struct.unpack_from(">Q", vlt, expression_start + 8)[0]
    maximum_expressions = (expression_size - 16) // 24
    if expression_count > maximum_expressions:
        raise FormatError("VLT expression count exceeds its chunk")
    for index in range(expression_count):
        record = expression_start + 16 + index * 24
        _, expression_type, data_size, data_offset = struct.unpack_from(
            ">QQII", vlt, record
        )
        if expression_type != _COLLECTION_EXPRESSION or data_size < 0x30:
            continue
        physical = dat_start + 16 + data_offset
        if physical + data_size > len(vlt):
            raise FormatError("VLT collection data exceeds DatN")
        collection = vlt[physical : physical + data_size]
        field_type = struct.unpack_from(">Q", collection)[0]
        identity = _menu_event_identity(collection, wanted)
        if field_type != _SK8_MENU_TYPE or identity is None:
            continue
        event_name, event_hash, class_hash, identity_offset = identity
        identities[event_name] = {
            "name_hash": f"{event_hash:#018x}",
            "class_hash": f"{class_hash:#018x}",
            "field_type_hash": f"{field_type:#018x}",
            "identity_offset": f"{identity_offset:#x}",
            "expression_index": index,
            "expression_data_offset": f"{data_offset:#x}",
        }
    missing_identities = sorted(set(_EVENT_NAMES) - identities.keys())
    if missing_identities:
        raise FormatError(
            "front-end VLT is missing menu-event collections: "
            + ", ".join(missing_identities)
        )
    missing_rows = sorted(set(_EVENT_NAMES) - symbol_rows.keys())
    if missing_rows:
        raise FormatError(
            "front-end VLT is missing symbolic menu-audio rows: "
            + ", ".join(missing_rows)
        )
    rows = {
        name: {
            **identities[name],
            **symbol_rows[name],
            "row_resolution": "PtrN section 1 event-name symbol",
        }
        for name in _EVENT_NAMES
    }
    missing = sorted(set(_EVENT_NAMES) - rows.keys())
    if missing:
        raise FormatError(f"front-end VLT is missing menu events: {', '.join(missing)}")
    return rows


@dataclass(frozen=True)
class Stream:
    index: int
    header_offset: int
    sample_rate: int
    sample_count: int
    payload: bytes


def _xma1_riff(stream: Stream) -> bytes:
    # This is the canonical mono XMA1 WAVEFORMAT used by vgmstream's raw-XMA1
    # adapter. The retail payload is copied byte-for-byte after the header.
    header_size = 0x3C
    header = bytearray(header_size)
    header[0:4] = b"RIFF"
    struct.pack_into("<I", header, 4, header_size - 8 + len(stream.payload))
    header[8:12] = b"WAVE"
    header[12:16] = b"fmt "
    struct.pack_into("<IHHHHH", header, 16, 0x20, 0x0165, 16, 0x10D6, 0, 1)
    header[30] = 0
    header[31] = 2
    struct.pack_into(
        "<IIII",
        header,
        32,
        stream.sample_rate // 2,
        stream.sample_rate,
        0,
        0,
    )
    header[48] = 0
    header[49] = 1
    struct.pack_into("<H", header, 50, 1)
    header[52:56] = b"data"
    struct.pack_into("<I", header, 56, len(stream.payload))
    return bytes(header) + stream.payload


def _bind_splc_patches(
    graph_rows: list[dict], event_roots: list[int], grains: list[dict]
) -> list[dict]:
    grain_cursor = 0
    for graph in graph_rows:
        count = graph["grain_count"]
        graph["grains"] = grains[grain_cursor : grain_cursor + count]
        grain_cursor += count
    if grain_cursor != len(grains):
        raise FormatError("SPLC graphs do not consume every grain")

    return [
        {
            "patch": patch,
            "root": root,
            "grain_count": graph_rows[root]["grain_count"],
            "parameters": graph_rows[root]["parameters"],
            "grains": graph_rows[root]["grains"],
        }
        for patch, root in enumerate(event_roots)
    ]


def _splc_playback_parameters(
    patch_parameters: list[float], grain_parameters: list[float]
) -> dict:
    """Map serialized SPLC fields to the parameters used by retail playback."""

    if len(patch_parameters) < 5 or len(grain_parameters) < 13:
        raise FormatError("SPLC playback parameter block is truncated")
    return {
        "graph_gain": patch_parameters[0],
        "graph_pitch_base": patch_parameters[1],
        "graph_pitch_random_range": patch_parameters[2],
        "gain": grain_parameters[0],
        "pitch_base": grain_parameters[1],
        "delay_seconds": grain_parameters[4],
        "gain_random_min": grain_parameters[10],
        "pitch_random_range": grain_parameters[11],
        "delay_random_range_seconds": grain_parameters[12],
    }


def _parse_splc(bank: bytes) -> tuple[list[dict], list[Stream], dict]:
    if len(bank) < 0x3C or bank[:4] != b"SPLC":
        raise FormatError("sk8_menu.bnk is not an SPLC bank")
    version, graph_end, patch_count, second_patch_count = struct.unpack_from(
        ">IIII", bank, 4
    )
    stream_count = struct.unpack_from(">I", bank, 0x18)[0]
    if version != 3 or patch_count != second_patch_count or not patch_count:
        raise FormatError("unsupported SPLC menu-bank header")
    if bank[0x1C:0x24].rstrip(b"\0") != b"sk8_menu":
        raise FormatError("SPLC bank is not sk8_menu")

    patch_table = 0x38
    root_table = patch_table + patch_count * 36
    grain_start = root_table + patch_count * 72
    if grain_start > len(bank):
        raise FormatError("SPLC patch tables exceed the bank")

    graph_rows = []
    event_roots = []
    grain_total = 0
    for graph in range(patch_count):
        patch_offset = patch_table + graph * 36
        packed = struct.unpack_from(">I", bank, patch_offset + 8)[0]
        declared_graph = packed >> 16
        grain_count = packed & 0xFFFF
        if declared_graph != graph:
            raise FormatError(f"SPLC graph {graph} has mismatched identity")
        graph_rows.append(
            {
                "graph": graph,
                "grain_count": grain_count,
                "parameters": [
                    struct.unpack_from(">f", bank, patch_offset + offset)[0]
                    for offset in range(12, 32, 4)
                ],
            }
        )
        grain_total += grain_count
    for patch in range(patch_count):
        root = struct.unpack_from(">H", bank, root_table + patch * 72 + 8)[0]
        if root >= patch_count:
            raise FormatError(f"SPLC patch {patch} references graph {root}")
        event_roots.append(root)

    grains = []
    cursor = grain_start
    for index in range(grain_total):
        if cursor + 20 > len(bank):
            raise FormatError("SPLC grain table is truncated")
        if bank[cursor + 8 : cursor + 16] == _NORMAL_GRAIN_TAG:
            streams = [struct.unpack_from(">H", bank, cursor + 16)[0]]
            parameters = [
                struct.unpack_from(">f", bank, cursor + offset)[0]
                for offset in range(20, 84, 4)
            ]
            size = 84
            grain_type = "sample"
        else:
            child_count = bank[cursor + 12]
            if child_count == 0:
                raise FormatError(f"unknown SPLC grain node at {cursor:#x}")
            size = 12 + child_count * 72
            if cursor + size > len(bank):
                raise FormatError("SPLC random grain node is truncated")
            children = []
            for child in range(child_count):
                child_offset = cursor + 12 + child * 72
                children.append(
                    {
                        "child": child,
                        "stream_index": struct.unpack_from(
                            ">H", bank, child_offset + 4
                        )[0],
                        "parameters": [
                            struct.unpack_from(
                                ">f", bank, child_offset + parameter_offset
                            )[0]
                            for parameter_offset in range(8, 72, 4)
                        ],
                    }
                )
            streams = [child["stream_index"] for child in children]
            parameters = []
            grain_type = "random"
        grains.append(
            {
                "index": index,
                "type": grain_type,
                "source_offset": f"{cursor:#x}",
                "stream_indices": streams,
                "parameters": parameters,
                **({"children": children} if grain_type == "random" else {}),
            }
        )
        cursor += size

    patches = _bind_splc_patches(graph_rows, event_roots, grains)

    streams = []
    signature = b"\x03\x00\xBB\x80"
    cursor = max(graph_end, grain_start)
    while len(streams) < stream_count:
        header = bank.find(signature, cursor)
        if header < 0 or header + 20 > len(bank):
            raise FormatError("SPLC embedded XMA stream table is truncated")
        sample_rate = struct.unpack_from(">H", bank, header + 2)[0]
        sample_count, payload_size, repeated_count = struct.unpack_from(
            ">III", bank, header + 4
        )
        if (
            sample_rate != 48000
            or sample_count == 0
            or sample_count != repeated_count
            or payload_size == 0
            or header + 20 + payload_size > len(bank)
        ):
            cursor = header + 1
            continue
        streams.append(
            Stream(
                len(streams),
                header,
                sample_rate,
                sample_count,
                bank[header + 20 : header + 20 + payload_size],
            )
        )
        cursor = header + 20 + payload_size
    for patch in patches:
        for grain in patch["grains"]:
            if any(index >= len(streams) for index in grain["stream_indices"]):
                raise FormatError("SPLC grain references a missing stream")
    return patches, streams, {
        "version": version,
        "graph_end": f"{graph_end:#x}",
        "patch_count": patch_count,
        "grain_count": grain_total,
        "stream_count": stream_count,
    }


def _find_ffmpeg(explicit: Path | None) -> str:
    candidates = []
    if explicit:
        candidates.append(str(explicit))
    discovered = shutil.which("ffmpeg")
    if discovered:
        candidates.append(discovered)
    candidates.append(r"C:\ffmpeg\bin\ffmpeg.exe")
    for candidate in candidates:
        path = Path(candidate)
        if path.is_file():
            return str(path)
    raise FileNotFoundError(
        "ffmpeg was not found; pass --ffmpeg or add ffmpeg to PATH"
    )


def extract_menu_audio(
    game_root: Path,
    output: Path,
    *,
    ffmpeg: Path | None = None,
    force: bool = False,
) -> dict:
    """Extract the exact retail Career-menu sound graph and embedded XMA."""

    game_root = Path(game_root).resolve()
    output = Path(output).resolve()
    db_path = game_root / "data" / "big" / "db.big"
    audio_path = game_root / "data" / "audio" / "audiofiles.big"
    for source in (db_path, audio_path):
        if not source.is_file():
            raise FileNotFoundError(source)
    if output == game_root or game_root in output.parents:
        raise ValueError("audio output must not be placed inside the game-data tree")
    manifest_path = output / "manifest.json"
    if manifest_path.exists() and not force:
        raise FileExistsError(f"refusing to overwrite {manifest_path}; pass --force")

    rows = _menu_event_rows(db_path)
    audio_archive = BigArchive(audio_path)
    bank = _entry(audio_archive, "data/audio/sk8_menu.bnk")
    patches, streams, bank_info = _parse_splc(bank)
    patch_by_index = {patch["patch"]: patch for patch in patches}
    # The sk8_menu enum occupies the contiguous range ending at 403. SPLC has
    # 202 local patches, so its first enum is 202.
    enum_base = 403 - bank_info["patch_count"] + 1

    selected_streams: set[int] = set()
    events = {}
    for name in _EVENT_NAMES:
        row = rows[name]
        patch_index = row["sound_enum"] - enum_base
        patch = patch_by_index.get(patch_index)
        if patch is None:
            raise FormatError(
                f"{name}: enum {row['sound_enum']} does not address sk8_menu"
            )
        layers = []
        random_groups = []

        def layer(
            grain: dict,
            child: int,
            stream_index: int,
            parameters: list[float],
        ) -> dict:
            playback = _splc_playback_parameters(
                patch["parameters"], parameters
            )
            if not math.isfinite(playback["graph_gain"]) or playback[
                "graph_gain"
            ] < 0.0:
                raise FormatError(f"{name}: SPLC graph has an invalid gain")
            if (
                not math.isfinite(playback["graph_pitch_base"])
                or playback["graph_pitch_base"] <= 0.0
                or not math.isfinite(playback["graph_pitch_random_range"])
            ):
                raise FormatError(f"{name}: SPLC graph has invalid pitch")
            if not math.isfinite(playback["gain"]) or playback["gain"] < 0.0:
                raise FormatError(f"{name}: SPLC grain has an invalid gain")
            if (
                not math.isfinite(playback["pitch_base"])
                or playback["pitch_base"] <= 0.0
                or not math.isfinite(playback["pitch_random_range"])
            ):
                raise FormatError(f"{name}: SPLC grain has an invalid pitch ratio")
            if (
                not math.isfinite(playback["delay_seconds"])
                or not math.isfinite(playback["delay_random_range_seconds"])
            ):
                raise FormatError(f"{name}: SPLC grain has an invalid delay")
            if (
                not math.isfinite(playback["gain_random_min"])
                or playback["gain_random_min"] <= 0.0
            ):
                raise FormatError(
                    f"{name}: SPLC grain has an invalid random-gain floor"
                )
            selected_streams.add(stream_index)
            return {
                "grain_index": grain["index"],
                "grain_type": grain["type"],
                "child": child,
                "stream_index": stream_index,
                "wav": f"streams/stream_{stream_index:03d}.wav",
                "xma": f"streams/stream_{stream_index:03d}.xma",
                "gain": playback["gain"],
                "pitch_base": playback["pitch_base"],
                "pitch_random_range": playback["pitch_random_range"],
                "delay_seconds": playback["delay_seconds"],
                "delay_random_range_seconds": playback[
                    "delay_random_range_seconds"
                ],
                "gain_random_min": playback["gain_random_min"],
                "grain_parameters": parameters,
            }

        for grain in patch["grains"]:
            if grain["type"] == "sample":
                layers.append(
                    layer(
                        grain,
                        0,
                        grain["stream_indices"][0],
                        grain["parameters"],
                    )
                )
            elif grain["type"] == "random":
                alternatives = [
                    layer(
                        grain,
                        child["child"],
                        child["stream_index"],
                        child["parameters"],
                    )
                    for child in grain["children"]
                ]
                random_groups.append(
                    {
                        "grain_index": grain["index"],
                        "grain_type": grain["type"],
                        "selection": "uniform_one",
                        "alternatives": alternatives,
                    }
                )
            else:
                raise FormatError(
                    f"{name}: its SPLC patch uses an unresolved "
                    f"{grain['type']} grain"
                )
        duration_ms = patch["parameters"][4]
        if not math.isfinite(duration_ms) or duration_ms <= 0.0:
            raise FormatError(f"{name}: SPLC patch has an invalid duration")
        events[name] = {
            **row,
            "enum_base": enum_base,
            "patch": patch_index,
            "patch_root": patch["root"],
            "patch_parameters": patch["parameters"],
            "graph_gain": patch["parameters"][0],
            "graph_pitch_base": patch["parameters"][1],
            "graph_pitch_random_range": patch["parameters"][2],
            "duration_ms": duration_ms,
            "layers": layers,
            "random_groups": random_groups,
        }

    ffmpeg_path = _find_ffmpeg(ffmpeg)
    stream_dir = output / "streams"
    stream_dir.mkdir(parents=True, exist_ok=True)
    for stream_index in sorted(selected_streams):
        stream = streams[stream_index]
        xma_path = stream_dir / f"stream_{stream_index:03d}.xma"
        wav_path = stream_dir / f"stream_{stream_index:03d}.wav"
        if not force and (xma_path.exists() or wav_path.exists()):
            raise FileExistsError(
                f"refusing to overwrite extracted stream {stream_index}; pass --force"
            )
        xma_path.write_bytes(_xma1_riff(stream))
        command = [
            ffmpeg_path,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(xma_path),
            "-af",
            f"atrim=end_sample={stream.sample_count},asetpts=PTS-STARTPTS",
            "-ar",
            "48000",
            "-ac",
            "1",
            "-c:a",
            "pcm_f32le",
            str(wav_path),
        ]
        completed = subprocess.run(
            command, capture_output=True, text=True, check=False
        )
        if completed.returncode:
            raise RuntimeError(
                f"ffmpeg failed for stream {stream_index}: "
                f"{completed.stderr.strip()}"
            )

    manifest = {
        "format": "skate3-native-menu-audio-v2",
        "source": {
            "db_archive": str(db_path),
            "db_sha256": hashlib.sha256(db_path.read_bytes()).hexdigest(),
            "audio_archive": str(audio_path),
            "audio_sha256": hashlib.sha256(audio_path.read_bytes()).hexdigest(),
            "bank_path": "data/audio/sk8_menu.bnk",
            "bank_sha256": hashlib.sha256(bank).hexdigest(),
        },
        "bank": bank_info,
        "enum_base": enum_base,
        "events": events,
        "streams": {
            str(index): {
                "header_offset": f"{streams[index].header_offset:#x}",
                "sample_rate": streams[index].sample_rate,
                "sample_count": streams[index].sample_count,
                "payload_size": len(streams[index].payload),
            }
            for index in sorted(selected_streams)
        },
        "playback": {
            "navigation": (
                "crossbar_up/down fire only after the selected row actually changes"
            ),
            "confirm": "menu_picker.OnMenuSelect when select sound is enabled",
            "back": "cancel/back before closing the current menu",
            "fade": "screens/main/dimmer intro and outro labels",
            "transition_out": "fires with confirm when entering a crossbar submenu",
            "transition_in": "fires after back when returning to the parent crossbar",
            "popup": "front-end popup presentation",
            "gain": (
                "live front-end setting multiplied by VLT event, SPLC graph, "
                "grain and authored random-gain factors"
            ),
            "pitch": (
                "SPLC graph pitch multiplied by independently randomized "
                "grain pitch"
            ),
            "grain_delay": "authored base plus random-range delay in seconds",
            "random_grains": "one uniformly selected child per random grain",
        },
    }
    output.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest
