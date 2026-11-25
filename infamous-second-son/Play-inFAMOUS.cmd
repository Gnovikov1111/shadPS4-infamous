@echo off
rem inFAMOUS Second Son launcher for this build.
rem Every performance change in this build is OFF unless its environment variable is set, so this
rem launcher is what turns them on. The settings apply to this run only (your config is unchanged).
rem
rem   1. Put this file next to shadPS4.exe (or change SHADPS4 below).
rem   2. Set GAME to the eboot.bin of your own dumped copy of the game.
set "SHADPS4=%~dp0shadPS4.exe"
set "GAME=C:\path\to\CUSA00004\eboot.bin"

rem Readbacks Precise + "Copy GPU command buffers" with 128 MB: smoke and fire mostly render.
set SHADPS4_READBACKS=2
set SHADPS4_COPY_GPU_BUFFERS=1
set SHADPS4_COPY_RESERVE_MB=128
rem Release 16 pages per CPU write trap (about 8x fewer traps).
set SHADPS4_FAULT_WIDEN_PAGES=16
rem GPU command thread speed-ups (see infamous-second-son\README.md).
set SHADPS4_BARRIER_MEMO=1
set SHADPS4_FIND_IMAGE_CACHE=1
set SHADPS4_FAST_ACCESS_LISTS=1
set SHADPS4_LAZY_READ_RANGES=1
set SHADPS4_IMAGE_DESC_CACHE=1
set SHADPS4_STREAM_DEDUPE=1
rem Copy worker thread (+6%). Its wake-up bug is fixed but the fix has had little play time: set 1 to try it.
set SHADPS4_COPY_WORKER=0

start "" "%SHADPS4%" "%GAME%"
