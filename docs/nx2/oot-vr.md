# Ocarina of Time VR in Lichii

This file says what the VR mod is, where its parts live, how far it is, and how to keep the repositories in sync.
Last checked: 2026-11-13.

## The repositories

| Local folder | Branch | Remote name | GitHub | Role |
|---|---|---|---|---|
| `D:\Games\Emulators\lichii repo` | `feat/oot-vr` | `eglantine` | `Eglantine-Community/Eglantine` (the same repo as `Djihads80/Eglantine`) | Upstream emulator. Read only. |
| | | `origin` | `Frizti/Fritchii` | The fork `feat/oot-vr` starts from (`origin/oot-gather-fix`). Read only. |
| | | `zortos` | `zortos293/Lichii` | The repo SinCuri0/Lichii is forked from on GitHub. Our PRs to upstream Lichii go here, against `nx2`. Read only. |
| | | `sincuri0` | `SinCuri0/Lichii` | **Our repo. All our work goes here.** |
| `D:\Games\Emulators\recomp` | `feat/vr-openxr` | `origin` | `sostenesfreitas/ocarina-recomp` | The OoT recompilation; its development moved to Hookshot. Read only. |
| | | `vr` | `SinCuri0/Curios-recomp-VR` | Our recomp fork with the VR research. |

We cannot commit or push to zortos293/Lichii, Eglantine, Fritchii or ocarina-recomp. We only fetch and merge from them. Their push URLs
are set to `no_push_read_only` in the local git config, so a push to them fails. `remote.pushDefault` is `sincuri0`
here and `vr` in the recomp.

The game is Ocarina of Time 1.1.1, program id `0400486024DEC000`, `main` build id
`e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348`.

## How the recomp and Ghidra feed the Lichii mod

1. **Ghidra finds the code.** Ghidra 12.1.4 runs headless (no window) in WSL Ubuntu on this PC:
   `/opt/ghidra_12.1.4_PUBLIC`, with OpenJDK 21. The project is `/home/alexander/ghidra-oot/oot.gpr`. It holds
   `main.elf`, the game's `main` module as an ELF file, loaded at `0x7000000000` (so `0x700033a410` is `main+0x33a410`).
   How `main.elf` was made from the NSO is not known; no tool in the recomp repo writes it.
   Run a script from Windows:
   `wsl -d Ubuntu -- bash /mnt/d/Games/Emulators/recomp/tools/ghidra/analyze.sh --script DecompileAt.java <out.txt> <address>...`.
   The scripts are in `recomp\tools\ghidra\scripts\`: `DecompileAt`, `DisasmRange`, `RefsTo`, `FindConst32`,
   `HashCallers`, `SeadVtables`, `CameraStrings`, `StringsMatching`. Their output files come from the game: write them
   to `recomp\build\ghidra\` (ignored by git), not to a tracked folder.
2. **The recomp tests the idea fast.** The recomp turns the game's ARM code into C, so a hook is a C function and a
   build. Hardware write breakpoints (DR0) in the recomp found the function that writes the displayed view
   (`main+0x33a410`). Each game fact that works goes into `D:\Games\Emulators\recomp\docs\vr-port-notes.md`, with a
   status: verified, read, or not tested.
3. **Lichii gets the port.** An emulator cannot add instructions in place. Lichii replaces one game instruction with an
   `svc` (a system call), and the emulator does that instruction's work plus the VR work in C++. The patch records are in
   `src/core/file_sys/patch_manager.cpp`. The handlers are in `src/core/hle/kernel/svc/svc_nx2_oot_vr.cpp`.

Rule: find a fact in the recomp or Ghidra first, write it in `vr-port-notes.md`, then port it. Do not guess offsets in
Lichii.

### Hooks in Lichii

| Site | `svc` | Game function | What the handler gets |
|---|---|---|---|
| `main+0x33A428` | `0xF4` | renderer set-view `main+0x33A410` | the view index, the 3x4 view, the projection object |
| `main+0x4C5068` | `0xF5` | camera controller update `main+0x4C48C0` | the controller (`x19`) |
| `main+0x32A4DE4` | `0xF6` | camera target update `main+0x32A4C40` | the actor the camera follows (Link) |
| `main+0x14E0220` | `0xF7` | skinning matrices `main+0x14E0200` | a skeleton |
| `main+0x340A4B4` | (branch) | HUD layout update | VR HUD code at `main+0x3C46900` (`tools/nx2/oot_vr_hud.s`) |

The recomp hooks the camera controller at `main+0x4c48f4`. Lichii uses `main+0x4C5068`, after the controller writes the
view object.

## Status

From the commits on `feat/oot-vr` (2026-11-12) and the code. I have not run these myself.

Committed:

- OpenXR mode, `EDEN_NX2_OOT_VR=1`. The head pose turns the game camera. The headset field of view goes into the main
  projection, so culling and shadows fit the headset view.
- The headset pose is recorded once per drawn frame at set-view. This stopped the stutter when you turn your head.
- Stereo by alternate eyes, with an eye distance scale (default x1.5).
- First person at Link's head, with the head hidden.
- VR HUD: the HUD, prompts, talk box and area title move toward the centre, shifted per eye so they look 1.5 m away.
- Menus and the big map show on a flat screen 1 m away.
- When the game stalls, the last picture is sent again, so the runtime does not drop to its own view.

Work in progress (committed as WIP on 2026-11-13):

- The VR room: a 3D backdrop drawn behind the flat screens. `tools/nx2/oot_vr_room.py` builds `room.bin` from a
  6-direction capture made with `tools/nx2/oot_vr_test.py --capture`. Lichii loads `EDEN_NX2_OOT_VR_ROOM`, or
  `<user dir>/oot_vr_room/room.bin`. `room.bin` and `room.png` come from the game: keep them out of git.
- Shaders `ocarina_vr_room.vert` and `.frag`, `vk_oot_vr_room.cpp`.

Not done or not known:

- Headset tests: one PlayStation VR2 test through SteamVR, from the recomp, before most of this work.
- Videos on the fixed screen (the recomp detects them; see `vr-port-notes.md`, "Videos").
- The recomp cutscene signal (the listener view) gives false cutscenes in Lichii.
- Adult Link and story cutscenes are not tested.

## Settings and keys

Environment variables (read in `vk_oot_vr.cpp` and `svc_nx2_oot_vr.cpp`): `EDEN_NX2_OOT_VR`, `_XR`, `_STEREO`, `_FIRST`,
`_EYE_SCALE`, `_EYE_FWD`, `_MOVE`, `_HUD`, `_HUD_IN`, `_TEXT`, `_MAP_Y`, `_PROMPT_Y`, `_TALK_Y`, `_MENU_DIST`,
`_OVERSCAN`, `_LAG`, `_VIEWS`, `_SOURCE`, `_ROOM`, `_ROOM_SHOT`, `_CAPTURE`, `_UI_TRACE`, and the test poses
`_FAKE_YAW`, `_FAKE_PITCH`, `_FAKE_TAN`, `_FAKE_IPD`. Each has the prefix `EDEN_NX2_OOT_VR`.

Keys while playing (Windows):

| Key | Does |
|---|---|
| 1 / 2 | HUD smaller / bigger |
| 3 / 4 | HUD out to the edges / in to the centre |
| 5 | Head zero to where the head is now |
| 6 / 7 | Minimap down / up |
| 8 / 9 | Talk box and cutscene text smaller / bigger |
| 0 | Swap the HUD's sideways move between the eyes |
| - / = | Eye distance smaller / bigger |
| [ / ] | Menu screen nearer / farther |
| ' / ; | Action prompt up / down |
| . / , | Talk box up / down |

## Build and test on this PC

- Build output: `D:\lxc\bin\lichii.exe`.
- `tools/nx2/oot_vr_build_test.ps1 [-Headset] [-Hold 10]` builds, then runs `oot_vr_test.py` against the game.
  It calls a build `.bat` in an old Claude scratchpad folder. Replace that path when the scratchpad is gone.
- `tools/nx2/oot_vr_test.py --sweep 5` tests head turns without a headset. `--play` runs without screenshots.
- `D:\Games\Emulators\oot_vr_play.ps1` and `oot_vr_room_capture.ps1` are launch scripts outside the repo.

## Keeping in sync and not losing work

- Commit at each working step. A WIP commit is better than lost work. Never leave a day's work uncommitted.
- Push `feat/oot-vr` to `sincuri0` and `feat/vr-openxr` to `vr` after each session. Never push anywhere else.
- Fetch all remotes at the start of each session: `git fetch --all --prune`.
- Check how far behind we are:
  `git rev-list --left-right --count HEAD...eglantine/nx2` and `HEAD...origin/oot-gather-fix`.
  On 2026-11-13: 358 behind `zortos/nx2`, 252 behind `eglantine/nx2`, 0 behind `origin/oot-gather-fix`.
- Check open PRs on our repo: `curl -s "https://api.github.com/repos/SinCuri0/Lichii/pulls"` (`gh` is not installed).
  Read a PR with `git fetch sincuri0 pull/<n>/head:refs/pr/<n>`. Do not build or run PR code before you read it.
- Merge upstream into the VR branch with `git merge`, not rebase, so pushed history stays the same. Commit or stash first.
- Before a new fix, check upstream for an existing one (see `AGENTS.md`, "Before writing a fix").
- Never commit `mods/` (third-party mod archives) or files made from the game (`room.bin`, captures, shader dumps).
  `mods/` is in `.git/info/exclude`.
