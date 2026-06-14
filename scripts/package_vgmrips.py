#!/usr/bin/env python3
"""Build a VGMRips-style ZIP package from a directory of VGM files."""

from __future__ import annotations

import argparse
import gzip
import os
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


VGM_RATE = 44100
SPC_FIELDS = {
    "title": (0x2E, 32),
    "game": (0x4E, 32),
    "artist": (0xB1, 32),
}


@dataclass
class Track:
    source: Path
    data: bytes
    title: str
    total_samples: int
    loop_samples: int
    game: str = ""
    artist: str = ""
    output_name: str = ""


def read_vgm(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    if len(data) < 0x40 or data[:4] != b"Vgm ":
        raise ValueError(f"not a valid VGM file: {path}")
    return data


def uint32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_gd3(data: bytes) -> dict[str, str]:
    relative = uint32(data, 0x14)
    if not relative:
        return {}
    offset = 0x14 + relative
    if offset + 12 > len(data) or data[offset : offset + 4] != b"Gd3 ":
        return {}
    length = uint32(data, offset + 8)
    raw = data[offset + 12 : offset + 12 + length]
    try:
        fields = raw.decode("utf-16le", errors="replace").split("\0")
    except UnicodeDecodeError:
        return {}
    names = (
        "title",
        "title_japanese",
        "game",
        "game_japanese",
        "system",
        "system_japanese",
        "artist",
        "artist_japanese",
        "release_date",
        "creator",
        "notes",
    )
    return {name: fields[index].strip() for index, name in enumerate(names) if index < len(fields)}


def decode_spc_text(raw: bytes) -> str:
    raw = raw.split(b"\0", 1)[0].rstrip(b" ")
    for encoding in ("utf-8", "cp1252", "shift_jis"):
        try:
            return raw.decode(encoding).strip()
        except UnicodeDecodeError:
            pass
    return raw.decode("latin-1", errors="replace").strip()


def parse_spc(path: Path | None) -> dict[str, str]:
    if path is None:
        return {}
    data = path.read_bytes()
    if len(data) < 0xD1 or not data.startswith(b"SNES-SPC700 Sound File Data"):
        return {}
    return {
        name: decode_spc_text(data[offset : offset + length])
        for name, (offset, length) in SPC_FIELDS.items()
    }


def natural_key(path: Path) -> list[object]:
    return [int(part) if part.isdigit() else part.casefold() for part in re.split(r"(\d+)", path.stem)]


def cleaned_filename_title(path: Path) -> str:
    stem = re.sub(r"_optimized$", "", path.stem, flags=re.IGNORECASE)
    title = re.sub(r"^\s*\d+\s*[-_. ]*\s*", "", stem)
    title = re.sub(r"[_]+", " ", title)
    title = re.sub(r"\s+", " ", title).strip()
    return title or path.stem


def safe_title(title: str) -> str:
    title = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "-", title)
    title = re.sub(r"\s+", " ", title).strip(" .")
    return title or "Untitled"


def find_spc(stem: str, directories: list[Path]) -> Path | None:
    for directory in directories:
        candidate = directory / f"{stem}.spc"
        if candidate.is_file():
            return candidate
        candidates = list(directory.glob(f"{stem}.[sS][pP][cC]")) if directory.is_dir() else []
        if candidates:
            return candidates[0]
    return None


def most_common(values: list[str], fallback: str) -> str:
    values = [value for value in values if value]
    return Counter(values).most_common(1)[0][0] if values else fallback


def find_vgm_cmp() -> Path:
    script_directory = Path(__file__).resolve().parent
    candidates = []
    if os.environ.get("VGM_CMP"):
        candidates.append(Path(os.environ["VGM_CMP"]))
    for name in ("vgm_cmp", "vgm_cmp.exe"):
        candidates += [
            script_directory.parent / "bin" / name,
            script_directory.parent / "build" / name,
            script_directory.parent.parent / "vgmtools" / "build" / name,
        ]
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("Unable to find bundled vgm_cmp. Rebuild spc2vgm or set VGM_CMP.")


def optimize_vgm_directory(directory: Path) -> None:
    vgm_cmp = find_vgm_cmp()
    optimized = 0
    unchanged = 0
    print(f"Checking VGM optimization in {directory}...", flush=True)
    for source in sorted(directory.glob("*.vgm"), key=natural_key):
        if source.stem.casefold().endswith("_optimized"):
            continue
        temporary = source.with_name(f"{source.name}.vgm_cmp_tmp.{os.getpid()}")
        temporary.unlink(missing_ok=True)
        print(f"Optimizing {source}", flush=True)
        result = subprocess.run([str(vgm_cmp), str(source), str(temporary)])
        if result.returncode:
            temporary.unlink(missing_ok=True)
            raise SystemExit(f"vgm_cmp failed for {source} with exit code {result.returncode}")
        if temporary.is_file():
            temporary.replace(source)
            optimized += 1
        else:
            unchanged += 1
    print(f"Done: {optimized} optimized, {unchanged} already optimal.")


def duration(samples: int) -> str:
    seconds = int(round(samples / VGM_RATE))
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{seconds:02d}"
    return f"{minutes}:{seconds:02d}"


def text_field(label: str, value: str) -> str:
    return f"{label + ':':<21}{value}"


def make_description(args: argparse.Namespace, tracks: list[Track], package_name: str) -> str:
    game = args.game_name or most_common([track.game for track in tracks], package_name)
    artist = args.music_author or most_common([track.artist for track in tracks], "Unknown")
    lines = [
        "***********************************************",
        "* VGM music package                           *",
        "* http://vgmrips.net/                         *",
        "***********************************************",
        text_field("Game name", game),
        text_field("System", args.system),
        text_field("Music hardware", args.hardware),
        "",
        text_field("Music author", artist),
        text_field("Game developer", args.developer),
        text_field("Game publisher", args.publisher),
        text_field("Game release date", args.release_date),
        "",
        text_field("Package created by", args.creator),
        text_field("Package version", args.version),
        "",
        "Song list, in approximate game order:",
        f"{'Song name':<42} {'Length':>7} {'Loop':>7}",
    ]
    for index, track in enumerate(tracks, 1):
        name = f"{index:02d} {track.title}"
        loop = duration(track.loop_samples) if track.loop_samples else "-"
        lines.append(f"{name:<42} {duration(track.total_samples):>7} {loop:>7}")
    one_play = sum(track.total_samples for track in tracks)
    one_loop = one_play + sum(track.loop_samples for track in tracks)
    lines += [
        "",
        f"Total length: {duration(one_play)} (one play), {duration(one_loop)} (with one loop)",
        "",
        "Notes:",
        args.notes,
        "",
        "Package history:",
        f"{args.version} {args.date} {args.creator}: Initial package.",
        "",
    ]
    return "\r\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a numbered VGZ/M3U/TXT ZIP package suitable for VGMRips."
    )
    parser.add_argument("directory", type=Path, help="directory containing top-level .vgm or .vgz files")
    parser.add_argument("-o", "--output", type=Path, help="output ZIP path")
    parser.add_argument("--name", help="package name; defaults to game metadata or directory name")
    parser.add_argument("--spc-dir", type=Path, help="directory containing matching SPC files")
    parser.add_argument("--game-name")
    parser.add_argument("--music-author")
    parser.add_argument("--system", default="Super Nintendo Entertainment System")
    parser.add_argument("--hardware", default="Moonsound (YMF278B)")
    parser.add_argument("--developer", default="Unknown")
    parser.add_argument("--publisher", default="Unknown")
    parser.add_argument("--release-date", default="Unknown")
    parser.add_argument("--creator", default=os.environ.get("USER", "spc2vgm"))
    parser.add_argument("--version", default="1.00")
    parser.add_argument("--date", default=__import__("datetime").date.today().isoformat())
    parser.add_argument(
        "--notes",
        default="Converted from SNES SPC music to YMF278B/OPL4 VGM using spc2vgm.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir = args.directory.resolve()
    if not source_dir.is_dir():
        raise SystemExit(f"Not a directory: {source_dir}")
    images = [path for path in source_dir.iterdir() if path.is_file() and path.suffix.casefold() == ".png"]
    if not images:
        raise SystemExit(
            f"No PNG image found in {source_dir}. "
            "The package will not be accepted by VGMRips without it."
        )
    if len(images) > 1:
        names = ", ".join(sorted(path.name for path in images))
        raise SystemExit(f"Multiple PNG images found in {source_dir}; keep exactly one: {names}")
    image = images[0]
    optimize_vgm_directory(source_dir)
    candidates = [path for path in source_dir.iterdir() if path.suffix.casefold() in (".vgm", ".vgz")]
    regular_stems = {
        path.stem.casefold()
        for path in candidates
        if not path.stem.casefold().endswith("_optimized")
    }
    paths = sorted(
        [
            path
            for path in candidates
            if not (
                path.stem.casefold().endswith("_optimized")
                and path.stem.casefold()[: -len("_optimized")] in regular_stems
            )
        ],
        key=natural_key,
    )
    if not paths:
        raise SystemExit(f"No top-level VGM or VGZ files found in {source_dir}")

    spc_dirs = []
    if args.spc_dir:
        spc_dirs.append(args.spc_dir.resolve())
    spc_dirs += [source_dir, source_dir.parent]
    tracks = []
    width = max(2, len(str(len(paths))))
    for index, path in enumerate(paths, 1):
        data = read_vgm(path)
        gd3 = parse_gd3(data)
        metadata_stem = re.sub(r"_optimized$", "", path.stem, flags=re.IGNORECASE)
        spc = parse_spc(find_spc(metadata_stem, spc_dirs))
        title = gd3.get("title") or spc.get("title") or cleaned_filename_title(path)
        title = safe_title(title)
        tracks.append(
            Track(
                source=path,
                data=data,
                title=title,
                total_samples=uint32(data, 0x18),
                loop_samples=uint32(data, 0x20),
                game=gd3.get("game") or spc.get("game", ""),
                artist=gd3.get("artist") or spc.get("artist", ""),
                output_name=f"{index:0{width}d} {title}.vgz",
            )
        )

    default_name = source_dir.parent.name if source_dir.name.casefold() == "vgm" else source_dir.name
    package_name = safe_title(args.name or args.game_name or most_common([track.game for track in tracks], default_name))
    output = (args.output or Path.cwd() / f"{package_name}.zip").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="spc2vgm-vgmrips-") as temporary:
        stage = Path(temporary)
        for track in tracks:
            (stage / track.output_name).write_bytes(gzip.compress(track.data, compresslevel=9, mtime=0))
        playlist = "\r\n".join(track.output_name for track in tracks) + "\r\n"
        with (stage / f"{package_name}.m3u").open("w", encoding="utf-8", newline="") as output_file:
            output_file.write(playlist)
        description = make_description(args, tracks, package_name)
        with (stage / f"{package_name}.txt").open("w", encoding="utf-8", newline="") as output_file:
            output_file.write(description)
        shutil.copyfile(image, stage / f"{package_name}.png")
        temporary_zip = stage / "package.zip"
        with zipfile.ZipFile(temporary_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(stage.iterdir(), key=lambda item: natural_key(item)):
                if path != temporary_zip:
                    archive.write(path, path.name)
        shutil.copyfile(temporary_zip, output)

    print(f"Wrote {output} with {len(tracks)} tracks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
