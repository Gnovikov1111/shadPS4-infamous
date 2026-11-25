<div align="center">

# inFAMOUS Second Son on shadPS4

**Performance work and a 21:9 ultrawide patch for CUSA00004, game version 01.07**

[Quick start](#quick-start) &nbsp;&middot;&nbsp;
[Results](#results) &nbsp;&middot;&nbsp;
[Ultrawide patch](#ultrawide-219-patch) &nbsp;&middot;&nbsp;
[AYOUB1080p build tips](#tips-for-the-ayoub1080p-build) &nbsp;&middot;&nbsp;
[Performance changes](#performance-changes) &nbsp;&middot;&nbsp;
[Building](#building) &nbsp;&middot;&nbsp;
[Credits](#credits)

</div>

> [!NOTE]
> Unofficial and not affiliated with Sony Interactive Entertainment, Sucker Punch Productions or the
> shadPS4 team. No game files, keys, firmware or shader caches are included. Use your own dumped
> copy of the game.

## Quick start

| I want | Do this |
| --- | --- |
| **The best frame rate** | Install **[shadPS4-AYOUB1080p](https://github.com/weshayoub92/shadPS4-AYOUB1080p)** (its README explains how), apply the [shader freeze fix](#tips-for-the-ayoub1080p-build), and add the [ultrawide patch](#ultrawide-219-patch) if you have a 21:9 screen. |
| **This branch** | [Build it](#building), put [`Play-inFAMOUS.cmd`](Play-inFAMOUS.cmd) next to `shadPS4.exe`, set `GAME` in it to your `eboot.bin`, and run it. |
| **Only ultrawide** | Copy the [patch](#ultrawide-219-patch) into the `patches` folder of the shadPS4 build you already use. |

## Results

Frame rates at 3440x1440 on an i9-12900KF + RTX 5070:

| Scene | Before (switches off) | This branch | AYOUB1080p build |
| --- | :---: | :---: | :---: |
| Heavy outdoor bridge view | 6.5-7 | ~21 | **usually 30-60** |
| Long tunnel views | 8-12 | ~25 | - |
| Combat | - | 26-32 | **up to ~50** |

<sub>This branch was measured before we found the AYOUB1080p build; his numbers are from playing
it since. "-" means not measured.</sub>

## Credits

- **[shadPS4-AYOUB1080p](https://github.com/weshayoub92/shadPS4-AYOUB1080p) by AYOUB1080p**: a
  shadPS4 fork tuned for this game. It goes further than our work with the same idea, splitting the
  GPU command processing over several threads (a Vulkan command recording thread, an upload worker,
  background pipeline compiles), plus many caches and fixes. **If you just want to play, use his
  build.**
- **The [shadPS4](https://github.com/shadps4-emu/shadPS4) team and contributors** for the emulator.
- **junminlee2004** for the "Disable motion blur" and "Disable depth of field" patches, copied
  unchanged from [shadps4-emu/ps4_cheats](https://github.com/shadps4-emu/ps4_cheats) into the
  ultrawide patch file.

## Tips for the AYOUB1080p build

> [!TIP]
> **Shader freezes the first time you see an effect?** Add this line to `perf_tuning.ini` in your
> shadPS4 user folder. The build rereads the file while the game runs, so no restart is needed.
>
> ```ini
> async_pipelines = 2
> ```

- **Why it helps:** his game profile compiles *graphics* pipelines in the background
  (`async_pipelines = 1`) but still builds *compute* pipelines on the GPU thread, which freezes the
  game for up to about a second while a new effect compiles. With `2`, a new effect may be missing
  for a few frames instead. The build also skips later GPU work that reads what a skipped dispatch
  would have written. Set it back to `1` to undo.
- **Shader caches from other shadPS4 builds are not reused.** His build uses a different cache
  format, so it moves an old cache aside (`cache\CUSA00004.incompatible`) and starts over. The
  stutter fades as the new cache fills.
- **The ultrawide patch below works with his build too.** His own patches target game version 01.00
  and are skipped on 01.07.

## Ultrawide 21:9 patch

For game version **01.07** in base PS4 mode. Tested in game at 2560x1080 and 3440x1440 with no HUD
problems seen.

1. Copy the [`patches/Ultrawide`](patches/Ultrawide) folder into `patches` in your shadPS4 user
   folder: `%APPDATA%\shadPS4\patches\Ultrawide`, or `user\patches\Ultrawide` for a portable
   install.
2. Keep **PS4 Pro mode (`neo_mode`) off**.
3. Enable **one** "Ultrawide" entry (`isEnabled="true"`). The file ships with full 3440x1440 on.

| Entry | Renders at | Shows at |
| --- | :---: | :---: |
| Ultrawide 21:9 3440x1440 (full resolution) | 3440x1440 | 3440x1440 |
| Ultrawide 21:9 3440x1440, 80% render | 2752x1152 | 3440x1440 |
| Ultrawide 21:9 3440x1440, 75% render | 2560x1072 | 3440x1440 |
| Ultrawide 21:9 2560x1080 | 2560x1080 | 2560x1080 |

> [!IMPORTANT]
> Enable only one Ultrawide entry at a time. The motion blur and depth of field entries can be
> combined with any of them.

The research notes and the scripts used to find and check the patch locations are in
[`tools`](tools).

## Performance changes

The profile (Windows Performance Recorder) showed that shadPS4 is limited by **one thread**, the GPU
command processor, which reads the game's GPU commands and records them for Vulkan. It was 96% busy
while the RTX 5070 sat at 5-25% load. Every change below makes that thread do less work.

All of them are **off unless their environment variable is set**. [`Play-inFAMOUS.cmd`](Play-inFAMOUS.cmd)
sets them for one run and leaves your shadPS4 config unchanged.

| Switch | What it does | Measured |
| --- | --- | --- |
| `SHADPS4_BARRIER_MEMO=1` | skips re-walking a 6-mip x 2048-layer texture array on every draw when its state is unchanged | tunnel view 12 &rarr; 21 FPS |
| `SHADPS4_FIND_IMAGE_CACHE=1` | caches exact-match texture lookups | together with the next two: bridge view 6.5 &rarr; 18.6 FPS |
| `SHADPS4_FAST_ACCESS_LISTS=1` | tracks buffer access ranges in a B-tree | (see above) |
| `SHADPS4_LAZY_READ_RANGES=1` | sorts access ranges only when a hazard check needs them | (see above) |
| `SHADPS4_IMAGE_DESC_CACHE=1` | reuses the texture description per 32-byte texture descriptor | +12% |
| `SHADPS4_STREAM_DEDUPE=1` | reuses a small read-only buffer's upload while guest memory cannot have changed | +3-4% |
| `SHADPS4_COPY_WORKER=1` | moves guest-memory copies into upload buffers to a worker thread | +6% |
| `SHADPS4_FAULT_WIDEN_PAGES=16` | releases 16 pages per CPU write trap | ~8x fewer traps |
| `SHADPS4_READBACKS=2`<br>`SHADPS4_COPY_GPU_BUFFERS=1`<br>`SHADPS4_COPY_RESERVE_MB=128` | settings that make the smoke and fire render (Precise readbacks) | correctness |

- Perf switches also accept `2`, an A/B test: on for 10 s, then off for 10 s.
- `SHADPS4_TRACE_BARRIERS=1` logs FPS, draws per frame and counters every 2 seconds.
- All switches are documented in [`src/video_core/readback_trace.h`](../src/video_core/readback_trace.h).

> [!NOTE]
> The launcher leaves `SHADPS4_COPY_WORKER` at `0`. Its wake-up bug is fixed, but the fix has had
> little play time yet. Set it to `1` to try it.

### Also in this branch

- **Shader cache fix** (`vk_pipeline_serialization.cpp`): on load, cached pipelines whose
  permutation indices changed are remapped instead of being dropped and recompiled every session.
- **Copy worker notes** (`src/video_core/buffer_cache/copy_worker.h`): write-combined GPU memory
  needs `_mm_sfence` before another thread may submit it, and only the worker may clear its own
  "sleeping" flag. An earlier version lost wake-ups and stalled for up to 0.5 s.
- **Watchdog:** logs what the GPU command thread waits on if it stops making progress.

## Building

Follow shadPS4's [Windows build guide](../documents/building-windows.md). With the MSVC 14.52 build
tools, OpenAL Soft fails to compile, so apply the included one-line fix first:

```sh
git -C externals/openal-soft apply ../../infamous-second-son/build/openal-soft-msvc-14.52.patch
```

## License

GPL-2.0-or-later, like shadPS4.
