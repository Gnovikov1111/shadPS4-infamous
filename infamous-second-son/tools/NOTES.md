# Ultrawide patch: research notes (inFAMOUS Second Son, CUSA00004, 01.07)

The game's executable is not stored here. To check or extend the patch you need your own copy of
the game and a tool that turns the PS4 SELF `eboot.bin` into a plain ELF ("unwrapped eboot").

    python -I tools\dis.py <work>\eboot.elf 0x40 0x40 @0x1eff47            # disassemble at an address
    python -I tools\imports.py <work>\eboot.elf sceVideoOutSetBufferAttribute
    python -I tools\verify_patch.py <work>\eboot.elf patches\Ultrawide\InfamousSecondSon_Ultrawide.xml

`dis.py` and `verify_patch.py` need GNU `objdump` (binutils) on PATH, or its path in the `OBJDUMP`
environment variable. `verify_patch.py` applies every patch entry the way shadPS4 applies "mask"
lines and reports whether each pattern matches exactly once.

## Findings (unwrapped eboot, text segment file 0x4000 -> VA 0)

- 0x1eff00 startup: res struct [0]/[4] = output W/H, [0xc]/[0x10] = render W/H (all 1920x1080).
  Output W/H feed sceVideoOutSetBufferAttribute (calls 0x1efd76, 0x1f0f9f) -> display buffer size.
- 0x1f0d16 video-mode switch: base mode uses esi/edi (1920/1080); 4K mode 3840x2160 out / 3200x1800 render.
- 0x1f0113 sceKernelIsNeoMode -> 0x1f0120 Pro 4K path (checkerboard 1600x1800). Not patched: keep Neo off.
- 0x1f03e3 base path: global display size W 0x1d0ccc0 / H 0x1d0ccc4 (107 refs; HUD scale, safe area).
- 0x5a6119: something clamped to <= 1920x1080 (candidate if a buffer looks wrong at higher res).
- No literal 16:9 float in the eboot; 0.5625 appears 24x. Camera aspect source not yet confirmed.
- No 60 FPS patch is needed: the game calls sceVideoOutSetFlipRate(handle, 1) (30 FPS) at 0x1efe0b
  only when the flag at [res+0xdf0ef8] is set; otherwise it flips at 60.
