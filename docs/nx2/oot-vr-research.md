# OoT VR research notes

How we find game code for the VR mod: methods that worked, methods that did not, and the tools. Game facts (addresses,
offsets) belong in the recomp's `docs/vr-port-notes.md`; this file is about how to get them. Status and settings are in
`oot-vr.md`.

## Finding what a game state changes (the item menu case, 2026-11-13)

Goal: the L/R item menu stops the world draw; find the switch. The steps that worked, in order:

1. **Measure the symptom.** The frame dump's `after_draws` (`oot_vr_test.py --hold-buttons L`, prints "draws in the
   dumped frame") separates the states: about 135 with the menu, 500-2800 without.
2. **Memory diff across repeated toggles.** `EDEN_NX2_OOT_VR_MENU_DIFF=1` (`ItemMenuDiff` in `svc_nx2_oot_vr.cpp`)
   snapshots main data/BSS and the first 1 GiB of heap 40 frames after each close and compares 90 frames after each
   open, keeping words that hold the same two values every time. Candidates after 1, 2, 4, 8 openings: 2.6 M, 14 k,
   4 k, 1.2 k. The candidates go to `D:/lxc/vrdiff/menu_diff.txt` with the nearest vtable below each word. Most of
   what survived was command buffers and generic objects (event delegates); it did not name the switch by itself, but
   it showed the view-side approach was needed.
3. **Find where the drawn path branches.** The recomp profile showed the camera's set-view (`main+0x33a240`) is called
   from the view job `main+0x798a0`; its decompile showed the call is guarded by `view+0x54dc` bit 0.
4. **Log the guard from the emulator.** The set-view svc gets the view in x0; the HUD svc (0xF8, every frame) logs
   `view+0x54dc` changes. It went `0x0fb7` -> `0x0fb2` with the menu.
5. **Write watch: name the writer.** A host-side write watch (below) named `main+0x3437c0` as the code rewriting it.
   Its decompile gave the inputs; logging them showed camera `+0x91` flips. A second watch on camera `+0x91` named the
   camera manager `main+0x6531c0`. Its display gate (`+0x7d0` bit 0) stayed set; the other gate, camera `+0x88` bits
   0-1, was it: the menu clears bit 0 once when it opens. Setting it back once (from the HUD svc) brought back the
   full render (64 passes instead of 14) with the menu at the HUD slot.

Lesson: the flags read every frame (`view+0x54dc`, camera `+0x91`) are recomputed every frame, so writing them does
nothing; follow them up to a flag that is set once by the state change (camera `+0x88`), and write that one.

From a flag that gates the wanted code, a write watch plus a decompile of the writer moves one level up per
run (a few minutes each). This was far faster than the static searches and profiles tried before it.

## Methods that did not work (and why)

- **Recomp profile A/B (menu vs normal).** It shows which functions get quieter, not why. The world update runs in
  generic job runners (`main+0x93ce0`, `main+0x798a0`, `main+0x5fe60`) whose share drops without naming a condition.
  Useful only to get the call path above a known function (`prof_chains`-style listing of chains containing it).
- **Static search for an offset.** Large offsets (`0x54dc`) are reached through `mov wN, #imm` plus a register offset,
  or a base `add` and an index, so a scan for one store pattern misses writers. The write watch found them at once.
- **Writing the flag back once a frame.** The game recomputes these flags every frame before they are read, so a write
  from the HUD code has no effect. Patch the code that computes them instead.
- **Full heap reads with `ReadBlock`.** They flush GPU-written memory for every block: the game fell to 4 fps and
  crashed. Use `ReadBlockUnsafe` and cap the size (the guest commits about 9 GiB).
- **Desktop screenshots for headset issues.** They cannot show stereo or head-tracking problems.

## Tools

- **Write watch** (`Common::OotVr::WriteWatch()` in `src/common/oot_vr.h`, checked in
  `DynarmicCallbacks64::CheckMemoryAccess`): set it to a guest address and mark the page debug
  (`memory.MarkRegionDebug(page, 0x1000, true)`); every JIT write to it logs `Write watch ...` once per writer with the
  block PC, x30, the frame record's return address and the old value, and stores x19-x28 in `WriteWatchRegs()` (to find
  the writer's objects). Works without the GDB debugger. Subtract `main at` from the log to get main offsets. The PC is
  the block start, so it lands inside or just before the writer.
- **Ghidra headless** (WSL): `MSYS_NO_PATHCONV=1 wsl -d Ubuntu -- bash /mnt/d/Games/Emulators/recomp/tools/ghidra/analyze.sh
  --script <Script>.java <out> <args>`. Scripts in `recomp/tools/ghidra/scripts`: `DecompileAt` (functions containing
  addresses), `DisasmRange` (hex ranges, no `0x`), `RefsTo`, `FindConst32` (mov/movk pairs only, hex without `0x`),
  `StringsMatching` (regex; quote it twice through WSL: `"'a|b'"`), `VtableDump`, `RttiName` (this game has no RTTI).
- **Recomp profiles**: `tools/prof_diff.py` (inclusive share per function, two runs), `tools/prof_gfx_callers.py`.
  Input scripts land unreliably on the save load: repeat the press over a frame range.
- **Test runner**: `tools/nx2/oot_vr_test.py --hold-buttons L,L` holds each button 600 frames with a frame dump during
  it and ends after the holds; env vars before it select the debug features.
