# spc2vgm

`spc2vgm` converts SNES SPC music into YMF278B/OPL4 VGM playback files. It
extracts and decodes the SPC's BRR samples, loads them into OPL4 RAM, and
translates traced SNES DSP events into OPL4 register writes.

The conversion is approximate because OPL4 cannot reproduce every SNES DSP
feature exactly, but the generated files use the original instrument samples
and preserve song timing and detected loop points.

Each conversion collects original-SPC audio statistics during tracing and
renders a provisional VGM, then sets a per-track VGM gain so their full-track
stereo RMS levels match. VGM header gain has approximately 0.188 dB
resolution, so the result is the closest representable unclipped match.
`--playback-gain-db DB` overrides automatic volume matching.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build creates `spc2vgm`, `spc_trace`, `spc_render`, bundled `vgm_cmp`, and
the RSN archive extractor `unarr_extract` in `build/`.

Install a complete package tree containing the executables, scripts,
documentation, and third-party notices:

```sh
cmake --install build --prefix package/spc2vgm
```

GitHub Actions builds downloadable ZIP packages for macOS Intel, macOS Apple
silicon, Windows Intel, Linux Intel, and Linux ARM. Tagged builds also attach
all five packages to the GitHub release.

The `spc2vgm`, `spc_trace`, `spc_render`, `vgm_cmp`, and `unarr_extract`
executables do not require Python. The soundtrack download and VGMRips
packaging helpers require Python 3.10 or newer, but use only Python's standard
library; no third-party modules or `pip install` step are needed. Without
Python, the native conversion tools continue to work, but those two helper
scripts cannot run.

## Usage

Convert one SPC:

```sh
build/spc2vgm song.spc --auto-playback -o converted/song.vgm
```

Add `--wav` to also render the resulting calibrated VGM into the current
working directory:

```sh
build/spc2vgm song.spc --auto-playback --wav -o converted/song.vgm
```

This writes `./song.wav`. The WAV is rendered from the exact VGM bytes written
to disk. `--wav` accepts a single SPC and cannot be combined with batch or
debug mode.

Convert every SPC in a directory:

```sh
build/spc2vgm --batch music --auto-playback
```

Batch conversion uses all detected host CPU cores by default. Use `--jobs N`
to set the maximum number of concurrent conversions, or `--jobs 1` for
sequential conversion.

Without `--batch-output`, batch conversion writes into a `vgm` directory one
level below the input directory:

```text
music/song.spc
music/vgm/song.vgm
```

Output filenames always use the original SPC stem with a `.vgm` extension.
Use `--batch-output DIR` to override the default `INPUT/vgm` directory.

Generated VGMs contain a GD3 metadata block. Track title, game name, and music
author are copied from the SPC's ID666 metadata when available; the title
falls back to the SPC filename. The original system is identified as
`Super Nintendo Entertainment System`, and notes record that the file was
converted to YMF278B/OPL4 with `spc2vgm`.

Set the GD3 creator field for single or batch conversion with:

```sh
build/spc2vgm song.spc --creator "Your Name"
build/spc2vgm --batch music --creator "Your Name"
```

The default creator is `spc2vgm`.

Download a soundtrack and screenshot from SNESmusic.org, extract its SPCs,
convert the complete soundtrack, optimize it, and create a VGMRips-style ZIP:

```sh
scripts/fetch_snesmusic.py "Plok!" --creator "Your Name"
```

On Windows, use `scripts\fetch_snesmusic.cmd` instead. The `.cmd` launcher
locates Python and prints a clear requirement message when Python 3.10 or newer
is unavailable.

The script searches SNESmusic's soundtrack index and prints all matching
regional releases. It selects the first result by default; use `--match N` to
choose another result. Use `--output DIR` to select the parent output directory
and `--jobs N` to control conversion concurrency.

The resulting directory contains the downloaded RSN, extracted SPCs, converted
VGMs, screenshot, and final ZIP. `--download-only` stops after downloading and
extracting the SPCs. `--profile URL` bypasses search and converts a specific
SNESmusic profile.

Manifests are not written by default. Use `--manifest FILE` for a single
conversion or `--manifest-output DIR` for batch manifests. Batch manifest
filenames use the original stem with a `.csv` extension.

Useful playback controls:

```text
--playback SECONDS
--fallback-seconds SECONDS
--no-loop-detect
--playback-gain-db DB
--hardware-gain-db DB
--minimum-tl VALUE
--solo-voice 0..7
--prune-samples
--opl4-ram-kib N
--jobs N
```

`--auto-playback` uses SPC timing metadata and detected musical loops. Batch
conversion enables the same timed playback behavior by default.

`--prune-samples` reduces the embedded OPL4 sample bank to instruments that
receive a key-on during the exported playback interval. This can substantially
reduce file size, especially for short tracks that share a large SPC instrument
bank. Used samples keep their original OPL4 wave numbers, while their payloads
are tightly packed into the selected target RAM. The VGM
contains multiple sparse RAM data blocks that omit unused headers and payloads.
This preserves the wave-number mapping and rendered audio while reducing RAM
requirements, file size, and MSX upload time.
It also prevents unused or invalid-looking SPC directory entries from consuming
the configured OPL4 RAM budget.

`--opl4-ram-kib` selects the target MoonSound sample-RAM capacity. It defaults
to 256 KiB and accepts multiples of 128 KiB through 2048 KiB. For example,
`--opl4-ram-kib 512` permits a 512 KiB decoded sample bank. Files requiring
more RAM will not play correctly on cartridges with less installed sample RAM.

`fetch_snesmusic.py` enables sample pruning by default and accepts the same
choice, for example `fetch_snesmusic.py "Game" --opl4-ram-kib 512`.

Run `vgm_cmp` over every top-level VGM in a directory and replace each original
with its optimized version:

```sh
scripts/optimize_vgm_directory.sh music/vgm
```

The script prefers the bundled `vgm_cmp` from the installed package or local
build, then checks `PATH`, `VGM_CMP`, and the adjacent `vgmtools` checkout.
Existing `_optimized.vgm` files are skipped.

Create a VGMRips-style ZIP package containing numbered VGZ files, an M3U
playlist, a descriptive TXT file, and a PNG image:

```sh
scripts/package_vgmrips.py music/vgm
scripts/package_vgmrips.py music/vgm --name "Game Name" --creator "Your Name"
```

On Windows, use `scripts\package_vgmrips.cmd`.

Track titles are taken from VGM GD3 metadata when present. Otherwise, the
script looks for matching SPC files in the VGM directory and its parent and
uses their ID666 titles, then falls back to the original filename. Use
`--spc-dir DIR` when the matching SPC files live elsewhere. The TXT song list
includes track and loop lengths read directly from the VGM headers.

The input directory must contain exactly one top-level PNG image. It is copied
into the ZIP and renamed to match the package. The script reports that VGMRips
will not accept the package and stops when the image is missing.

Before building the package, the script optimizes the input directory's VGM
files using the bundled `vgm_cmp`. This applies any remaining lossless
optimizations in place. Files that have already been optimized are left
unchanged. The standalone `optimize_vgm_directory.sh` provides the same
behavior for Bash users. Packaging stops if optimization fails.

The TXT notes include the version reported by the bundled or local
`spc2vgm --version`, so packages record which converter release created them.

Package metadata can be supplied with `--game-name`, `--music-author`,
`--developer`, `--publisher`, `--release-date`, `--system`, `--hardware`,
`--version`, and `--notes`. Run `scripts/package_vgmrips.py --help` for all
options. The package defaults identify the original system as
`Super Nintendo Entertainment System` and playback hardware as
`Moonsound (YMF278B)`.

## Debugging tools

The CMake build also creates two small SPC debugging tools:

```sh
build/spc_trace song.spc 60 > song-trace.csv
build/spc_render song.spc 60 song-original.wav
build/spc_render song.spc 60 voice-5.wav 5
```

`spc_trace` records every SNES DSP register write with its SPC clock timestamp.
It is useful for checking key-ons, source-number changes, pitch updates,
volumes, envelopes, noise, pitch modulation, and echo configuration.

`spc_render` renders the original SPC to WAV using the bundled SNES SPC
emulator. Its optional final argument solos one DSP voice from `0` through `7`,
which makes it easier to compare individual voices with `spc2vgm --solo-voice`.

The converter can export all eight original and converted voices in one step:

```sh
build/spc2vgm --debug song.spc
```

This single-SPC mode writes comparison and routing-isolation WAV files into
`./debug/` in the current working directory:

```text
debug/song-spc-voice-0.wav
debug/song-vgm-voice-0.wav
debug/song-vgm-voice-0-direct.wav
debug/song-vgm-voice-0-echo.wav
...
debug/song-spc-voice-7.wav
debug/song-vgm-voice-7.wav
debug/song-vgm-voice-7-direct.wav
debug/song-vgm-voice-7-echo.wav
```

Each `vgm-voice-N.wav` contains the direct OPL4 voice plus any synthetic echo
slots sourced from that voice, matching its contribution to the complete VGM.
The `-direct.wav` and `-echo.wav` files isolate those two parts. Echo-only files
are silent when that source voice receives no synthetic OPL4 echo.

Use `--playback SECONDS` to limit the debug render duration.

## Current approximations

- SNES echo and FIR filtering cannot be reproduced exactly by OPL4.
- Echo uses stereo `EVOL`, `EDL`, feedback, echo-write-disable state, and a
  conservative FIR-response estimate.
- Sustained looped notes use one continuously tracked delayed voice. Finite
  notes use one or two bounded taps based on feedback and estimated duration.
- After sustained notes end, explicit `EDL`-spaced level updates approximate
  the remaining `EFB`-decaying sound already circulating in the echo buffer.
- OPL4 cannot reproduce arbitrary SNES FIR phase, polarity, or frequency
  response exactly.
- Pitch modulation and noise are approximated when a track uses them.
- SNES ADSR and GAIN envelopes are followed using traced live `ENVX` level
  updates. OPL4 autonomous ADSR is kept neutral because static rate mapping
  caused sustain and percussion regressions.
