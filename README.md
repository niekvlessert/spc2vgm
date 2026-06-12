# spc2vgm

`spc2vgm` converts SNES SPC music into YMF278B/OPL4 VGM playback files. It
extracts and decodes the SPC's BRR samples, loads them into OPL4 RAM, and
translates traced SNES DSP events into OPL4 register writes.

The conversion is approximate because OPL4 cannot reproduce every SNES DSP
feature exactly, but the generated files use the original instrument samples
and preserve song timing and detected loop points.

Each conversion renders the original SPC and provisional VGM internally, then
sets a per-track VGM gain so their full-track stereo RMS levels match. The
converter prints the final rendered RMS error and clipping count. VGM header
gain has approximately 0.188 dB resolution, so the result is the closest
representable unclipped match. `--playback-gain-db DB` overrides automatic
volume matching.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is written to `build/spc2vgm`.

## Usage

Convert one SPC:

```sh
build/spc2vgm song.spc --auto-playback -o converted/song.vgm
```

Convert every SPC in a directory:

```sh
build/spc2vgm --batch music --auto-playback
```

Without `--batch-output`, batch conversion writes into a `vgm` directory one
level below the input directory:

```text
music/song.spc
music/vgm/song.vgm
```

Output filenames always use the original SPC stem with a `.vgm` extension.
Use `--batch-output DIR` to override the default `INPUT/vgm` directory.

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
```

`--auto-playback` uses SPC timing metadata and detected musical loops. Batch
conversion enables the same timed playback behavior by default.

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

## Current approximations

- SNES echo and FIR filtering cannot be reproduced exactly by OPL4.
- Pitch modulation and noise are approximated when a track uses them.
- SNES ADSR and GAIN rates are mapped to the closest OPL4 envelope settings.
- The SNES DSP's exact live envelope position is not available to OPL4, so
  complex envelope transitions can still differ.

`gg-03.spc` does not use echo, FIR, pitch modulation, or noise. Its main
conversion challenge is envelope behavior plus frequent pitch and volume
updates. It also uses several SRCN entries that alias the same BRR sample
address. The converter maps all aliases to their shared OPL4 wave. Its exact
ADSR progression remains an approximation.
