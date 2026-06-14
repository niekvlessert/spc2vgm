#!/usr/bin/env python3
"""Fetch a SNESmusic soundtrack, convert it, and build a VGMRips package."""

from __future__ import annotations

import argparse
import html
import os
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path


BASE_URL = "https://snesmusic.org/v2/"
USER_AGENT = "spc2vgm/0.1 (+https://github.com/niekvlessert/spc2vgm)"
REGION_ORDER = {"NTSC": 0, "PAL": 1, "NTSC-J": 2}


@dataclass
class SearchResult:
    selected: str
    title: str
    region: str

    @property
    def profile_url(self) -> str:
        return urllib.parse.urljoin(BASE_URL, f"profile.php?profile=set&selected={self.selected}")


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request) as response:
            return response.read()
    except Exception as error:
        raise SystemExit(f"Unable to download {url}: {error}") from error


def fetch_text(url: str) -> str:
    return fetch(url).decode("utf-8", errors="replace")


def plain(value: str) -> str:
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", value))).strip()


def normalized(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.casefold())


def safe_name(value: str) -> str:
    value = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "-", value)
    return re.sub(r"\s+", " ", value).strip(" .") or "SNESmusic"


def search(query: str) -> list[SearchResult]:
    matches: dict[str, SearchResult] = {}
    initials = []
    for word in re.findall(r"[A-Za-z0-9]+", query):
        initial = "n1-9" if word[0].isdigit() else word[0].upper()
        if initial not in initials:
            initials.append(initial)
    for initial in initials or ["n1-9"]:
        for limit in range(0, 3000, 30):
            url = urllib.parse.urljoin(BASE_URL, f"select.php?view=sets&char={urllib.parse.quote(initial)}&limit={limit}")
            page = fetch_text(url)
            found = re.findall(
                r"<img\s+alt=['\"](NTSC|PAL|NTSC-J|Beta|SatellaView|SGB)['\"][^>]*>\s*<b><a\s+href=['\"]profile\.php\?profile=set&amp;selected=(\d+)['\"]>(.*?)</a></b>",
                page,
                flags=re.IGNORECASE | re.DOTALL,
            )
            if not found:
                break
            for region, selected, title in found:
                title = plain(title)
                if normalized(query) in normalized(title) or all(
                    normalized(word) in normalized(title) for word in re.findall(r"[A-Za-z0-9]+", query)
                ):
                    matches[selected] = SearchResult(selected, title, plain(region))
            if f"limit={limit + 30}" not in page:
                break
    query_key = normalized(query)
    return sorted(
        matches.values(),
        key=lambda result: (
            normalized(result.title) != query_key,
            not normalized(result.title).startswith(query_key),
            REGION_ORDER.get(result.region, 9),
            result.title.casefold(),
            int(result.selected),
        ),
    )


def profile_data(profile_url: str) -> dict[str, str]:
    page = fetch_text(profile_url)
    project = re.search(r"Project\s+([a-z0-9_-]+)\s+version", page, re.IGNORECASE)
    title = re.search(r"<h2>\s*<img\s+alt=['\"][^'\"]+['\"][^>]*>\s*(.*?)</h2>", page, re.IGNORECASE | re.DOTALL)
    screenshot = re.search(r"class=['\"]screen['\"]\s+src=['\"]([^'\"]+)", page, re.IGNORECASE)
    if not project or not title:
        raise SystemExit(f"Could not read SNESmusic project information from {profile_url}")
    data = {"code": project.group(1), "title": plain(title.group(1)), "profile_url": profile_url}
    if screenshot:
        data["image_url"] = urllib.parse.urljoin(profile_url, html.unescape(screenshot.group(1)))
    for label, key in (
        ("Composer", "music_author"),
        ("Developer", "developer"),
        ("Publisher", "publisher"),
        ("Released", "release_date"),
    ):
        match = re.search(
            rf"<td>\s*{label}s?:\s*</td>\s*<td>(.*?)</td>",
            page,
            re.IGNORECASE | re.DOTALL,
        )
        if match:
            data[key] = plain(match.group(1))
    data["rsn_url"] = urllib.parse.urljoin(BASE_URL, f"download.php?spcNow={data['code']}")
    data.setdefault("image_url", urllib.parse.urljoin(BASE_URL, f"images/screenshots/{data['code']}.png"))
    return data


def find_program(name: str) -> Path:
    script_directory = Path(__file__).resolve().parent
    suffixes = (".exe", "") if os.name == "nt" else ("", ".exe")
    for suffix in suffixes:
        candidates = [
            script_directory.parent / "bin" / f"{name}{suffix}",
            script_directory.parent / "build" / f"{name}{suffix}",
        ]
        found = shutil.which(f"{name}{suffix}")
        if found:
            candidates.append(Path(found))
        for candidate in candidates:
            if candidate.is_file():
                return candidate
    raise SystemExit(f"Unable to find {name}. Build or install spc2vgm first.")


def run(command: list[str | Path]) -> None:
    print("+", " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Search SNESmusic.org, download an RSN and screenshot, convert all SPCs, and create a VGMRips ZIP."
    )
    parser.add_argument("game", help="game title to search for")
    parser.add_argument("--match", type=int, default=1, help="use numbered search result; default: 1")
    parser.add_argument("--output", type=Path, default=Path.cwd(), help="parent output directory")
    parser.add_argument("--creator", default=os.environ.get("USER", "spc2vgm"))
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--opl4-ram-kib", type=int, default=256, help="target MoonSound sample RAM in KiB; default: 256")
    parser.add_argument("--profile", help="use this SNESmusic profile URL instead of searching")
    parser.add_argument("--download-only", action="store_true", help="download and extract SPCs without converting")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.profile:
        selected_profile = args.profile
    else:
        results = search(args.game)
        if not results:
            raise SystemExit(f"No SNESmusic soundtrack matches found for: {args.game}")
        print("SNESmusic matches:")
        for index, result in enumerate(results, 1):
            marker = " <- selected" if index == args.match else ""
            print(f"  {index:2d}. [{result.region}] {result.title}{marker}")
        if args.match < 1 or args.match > len(results):
            raise SystemExit(f"--match must be between 1 and {len(results)}")
        selected_profile = results[args.match - 1].profile_url

    metadata = profile_data(selected_profile)
    title = safe_name(metadata["title"])
    root = args.output.resolve() / title
    spc_directory = root / "spc"
    vgm_directory = root / "vgm"
    spc_directory.mkdir(parents=True, exist_ok=True)
    vgm_directory.mkdir(parents=True, exist_ok=True)

    archive = root / f"{metadata['code']}.rsn"
    image = vgm_directory / f"{title}.png"
    print(f"Downloading {metadata['rsn_url']}")
    archive.write_bytes(fetch(metadata["rsn_url"]))
    print(f"Downloading {metadata['image_url']}")
    image.write_bytes(fetch(metadata["image_url"]))

    extractor = find_program("unarr_extract")
    run([extractor, archive, spc_directory])
    if args.download_only:
        print(f"Downloaded and extracted to {spc_directory}")
        return 0

    converter = find_program("spc2vgm")
    conversion = [
        converter,
        "--batch",
        spc_directory,
        "--batch-output",
        vgm_directory,
        "--auto-playback",
        "--prune-samples",
        "--opl4-ram-kib",
        str(args.opl4_ram_kib),
        "--creator",
        args.creator,
    ]
    if args.jobs:
        conversion += ["--jobs", str(args.jobs)]
    run(conversion)

    packager = Path(__file__).resolve().with_name("package_vgmrips.py")
    package = root / f"{title}.zip"
    command: list[str | Path] = [
        sys.executable,
        packager,
        vgm_directory,
        "--spc-dir",
        spc_directory,
        "--name",
        title,
        "--game-name",
        title,
        "--creator",
        args.creator,
        "--developer",
        metadata.get("developer", "Unknown"),
        "--publisher",
        metadata.get("publisher", "Unknown"),
        "--release-date",
        metadata.get("release_date", "Unknown"),
        "-o",
        package,
    ]
    if metadata.get("music_author"):
        command += ["--music-author", metadata["music_author"]]
    run(command)
    print(f"Complete package: {package}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
