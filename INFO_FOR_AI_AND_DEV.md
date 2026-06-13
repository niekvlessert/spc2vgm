# Technical Handoff: spc2vgm

This document is a fast technical orientation for developers and AI agents
working on this repository. Read it before changing conversion behavior.

## Purpose

`spc2vgm` converts SNES SPC snapshots into standalone YMF278B/OPL4 VGM files,
primarily for playback on MSX with MoonSound and VGMPlay MSX.

The converter:

- Runs the original SPC to trace live SNES DSP activity.
- Decodes original BRR instrument samples into 16-bit OPL4 RAM samples.
- Translates DSP voice state into OPL4 wave-channel register writes.
- Approximates effects that OPL4 cannot reproduce directly.
- Matches full-track output volume against the original SPC.
- Optimizes the VGM command stream without changing effective OPL4 state.

The main implementation is intentionally concentrated in
`src/spc2vgm.cpp`.

## Build And Dependencies

The project uses CMake and C++17:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Important dependencies:

- `external/smwcentral-spc-player`: SPC execution, DSP emulation, and rendering.
- `external/libvgm`: VGM rendering used for calibration and WAV/debug output.

Built tools:

- `build/spc2vgm`: converter.
- `build/spc_trace`: raw SPC DSP-write tracer.
- `build/spc_render`: original-SPC WAV renderer with optional voice solo.

## Important Constants

From `src/spc2vgm.cpp`:

```text
SPC clock:             1,024,000 Hz
VGM timing rate:          44,100 Hz
OPL4 clock:           33,868,800 Hz
OPL4 ROM size:         2 MiB
OPL4 RAM size:         256 KiB
OPL4 RAM base address: 0x200000
RAM wave-number base:  384
Wave header size:      12 bytes
```

OPL4 RAM begins with room for 384 wave headers. Decoded sample payloads begin
at `384 * 12`.

## Conversion Pipeline

`convert()` performs the following:

1. Validate and load the SPC.
2. Extract and decode BRR samples from the SPC sample directory.
3. Determine duration and possible loop point from SPC metadata.
4. Run the SPC and trace DSP register writes plus live envelope values.
5. Optionally detect a musical loop from repeated key-on signatures.
6. Optionally mark unused samples for sparse upload with `--prune-samples`.
7. Build the OPL4 RAM image and upload ranges.
8. Translate traced DSP state into timed OPL4 commands.
9. Optimize redundant commands and waits.
10. Use audio statistics collected during tracing and render the generated VGM
    to calculate header gain.
11. Write the final VGM and optional manifest/WAV.

## SPC Tracing And Envelopes

The SPC emulator is compiled with `src/spc_trace_hook.h`, allowing
`spc_trace_dsp_write()` to receive DSP register writes.

The tracer also polls each voice's live `ENVX` approximately every 32 stereo
sample pairs, roughly once per millisecond. ENVX events use synthetic register
numbers `0x80` through `0x87`.

This is important: OPL4 ADSR alone did not reproduce SNES envelope behavior
accurately. The converter therefore drives OPL4 total level from live SNES
envelope values. Do not replace this with static ADSR mapping without extensive
voice-by-voice comparison.

After a key-on, ENVX is force-emitted even when its sampled value matches the
previous value. This handles very fast attacks that reset and recover between
normal polling points.

Trace callback state is `thread_local`. Batch conversions may run concurrently.

## Sample Extraction And OPL4 RAM

`extract_samples()`:

- Reads the SPC sample directory selected by DSP register `DIR`.
- Decodes valid BRR streams.
- Detects looped samples and loop offsets.
- Treats SRCN entries sharing one BRR start address as aliases.
- Builds an optional decoded continuation for smoother OPL4 loops.

`build_ram()`:

- Writes OPL4 wave headers and decoded 16-bit PCM.
- Preserves sample order and wave numbers.
- Optionally extends selected looped samples when RAM budget allows and doing so
  improves the loop boundary.
- Adds a generated noise waveform after the instrument samples.

Do not casually reorder samples. OPL4 wave numbers and sample start addresses
are referenced by playback commands and can affect rendered behavior.

## Sparse OPL4 RAM Blocks

`--prune-samples` does not repack or renumber samples.

It marks samples that receive a key-on during the exported interval, then emits
multiple VGM `0x87` OPL4 RAM data blocks containing only required headers and
sample payload ranges. Used samples retain the exact RAM addresses they have in
the full bank.

Noise data is omitted when SNES noise is never enabled.

Each `0x87` block contains:

```text
total OPL4 RAM size
start address within OPL4 RAM
payload bytes
```

Multiple `0x87` blocks are supported by VGMPlay MSX. Sparse output has been
verified to render byte-for-byte identical audio to full-bank output on tested
GG tracks.

## DSP To OPL4 Translation

Main OPL4 wave slots:

- Slots `0..7`: translated SNES DSP voices.
- Slots `8..15` and `16..23`: alternating echo approximation voices.

Key-on setup writes wave number, pitch, level, pan, modulation approximation,
fast attack/release settings, and finally enables the OPL4 key.

Pitch is translated from SNES pitch units to the closest OPL4 F-number and
octave.

Volume uses SNES per-voice left/right volume, master volume, live ENVX, hardware
gain, OPL4 total-level quantization, and the nearest representable OPL4 pan.

## Echo And Other Approximations

SNES echo/FIR cannot be reproduced exactly by OPL4.

Current echo approximation:

- Applies to key-ons for non-looped samples whose voice is enabled in `EON`.
- Schedules repeated delayed key-ons on spare OPL4 slots.
- Uses `EDL`, echo volume, master volume, and feedback to approximate timing and
  decay.
- Alternates between two spare slots per source voice.

Other limitations:

- FIR filtering is not reproduced exactly.
- Pitch modulation is approximated.
- Noise uses a generated OPL4 RAM noise sample.
- Some DSP behaviors cannot be mapped exactly to OPL4 hardware.

## VGM Layout

Generated VGM version is 1.71. The stream contains:

1. A zero-payload `0x84` OPL4 ROM block declaring the 2 MiB ROM size.
2. One full or several sparse `0x87` OPL4 RAM blocks.
3. Timed OPL4 writes using VGM command `0xD0`.
4. VGM end command `0x66`.
5. A GD3 metadata block containing SPC ID666 title/game/artist metadata,
   original system `Super Nintendo Entertainment System`, creator, and
   conversion notes.

The loop offset points into the music-command stream after all data blocks.

## Lossless Command Optimization

`optimize_tail()` runs after music-command generation.

It:

- Removes OPL4 writes that repeat the current value of the same register.
- Combines adjacent waits.
- Uses shorter equivalent VGM wait commands.
- Resets known register state at the loop boundary so loop playback remains
  correct.

This optimization has been verified by rendering old and optimized files and
comparing WAV SHA-256 hashes.

Remaining writes change OPL4 register state. Some may still be inaudible, but
removing them requires timing-aware or renderer-verified analysis and is not
automatically lossless on real hardware.

## Volume Matching

Unless `--playback-gain-db` is supplied:

1. Collect original-SPC audio statistics during the required DSP trace pass.
2. Render the generated VGM with zero header gain.
3. Calculate the closest VGM header-gain step that matches stereo RMS without
   clipping.
4. Calculate and print the expected matched RMS from that exact gain step.

The header gain has approximately 0.188 dB resolution.

The trace audio replaces a formerly separate SPC analysis render. A second VGM
verification render is also unnecessary because the selected header gain is
limited using the provisional render's peak. These optimizations do not alter
the selected gain or generated VGM bytes. If loop detection shortens the
exported duration after tracing, the converter still performs a separate SPC
analysis render for the exact shortened duration.

Batch mode runs independent tracks in parallel.

## Batch Concurrency

Batch mode uses all detected host CPU cores by default.

```sh
build/spc2vgm --batch music --jobs 8
build/spc2vgm --batch music --jobs 1
```

The batch worker pool avoids nested per-track render parallelism when multiple
tracks already run concurrently. Sequential and parallel output has been
verified byte-for-byte identical.

## Debugging Workflow

Export all original and converted voices:

```sh
build/spc2vgm --debug song.spc
```

This writes:

```text
debug/song-spc-voice-0.wav
debug/song-vgm-voice-0.wav
...
debug/song-spc-voice-7.wav
debug/song-vgm-voice-7.wav
```

Useful focused commands:

```sh
build/spc_trace song.spc 60 > trace.csv
build/spc_render song.spc 60 original.wav
build/spc_render song.spc 60 voice-4.wav 4
build/spc2vgm song.spc --auto-playback --wav
```

When changing audio behavior, compare rendered WAVs, individual voices, and
specific timestamps. RMS equality alone does not prove matching decay,
instruments, pitch, or percussion.

For a claimed lossless optimization, render both VGMs through the same
`vgm2wav` implementation and compare hashes:

```sh
vgm2wav before.vgm before.wav
vgm2wav after.vgm after.wav
shasum -a 256 before.wav after.wav
```

## Additional VGM Compression

The repository includes:

```sh
scripts/optimize_vgm_directory.sh DIRECTORY
```

It runs `vgm_cmp` on top-level `.vgm` files and safely replaces originals only
when optimized output is produced.

VGZ reduces disk size but does not reduce VGMPlay MSX mapper-RAM requirements:
VGMPlay decompresses the full stream into mapper RAM before playback.

## MSX Playback Constraints

The included `vgmplay-msx-release-1.4` source buffers the entire uncompressed
VGM in 16 KiB MSX-DOS 2 mapper segments before playback.

Important consequences:

- Disk free space is unrelated to the player's out-of-memory errors.
- VGZ does not reduce playback RAM requirements.
- VGMPlay's default mapped buffer allows up to 256 segments, approximately
  4 MiB.
- Sparse sample blocks and command-stream optimization directly reduce mapper
  RAM usage.
- A MoonSound or compatible OPL4 device is required for the generated files.

## Safe Change Checklist

Before finalizing converter changes:

1. Build with CMake.
2. Run `git diff --check`.
3. Convert representative short, long, looped, echo-heavy, percussion-heavy,
   and noise-using tracks.
4. Confirm generated files render and loop.
5. For intended lossless changes, compare rendered WAV hashes.
6. For audio changes, compare per-voice debug WAVs at known problem timestamps.
7. Test batch mode with multiple `--jobs` values and confirm deterministic
   output.
8. Consider VGMPlay MSX support and total uncompressed file size.

## Areas Worth Improving

- Prove and remove additional inaudible music writes with timing-aware analysis.
- Improve SNES echo/FIR approximation.
- Improve pitch modulation and noise behavior.
- Add automated regression fixtures with expected VGM/WAV hashes.
- Consider a custom MSX format/player that uploads a shared sample bank once
  and stores music streams separately. This would save much more space than
  standalone VGM can.
