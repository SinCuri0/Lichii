@AGENTS.md

## This PC

The `AGENTS.md` sections "Live debug server", "Builds" and the `C:/lx` paths describe the owner's other PC
(DESKTOP-4UD89UT). On this PC the build output is `D:\lxc\bin\lichii.exe`.

## OoT VR

Current work is the Ocarina of Time VR mod on branch `feat/oot-vr`. Read `docs/nx2/oot-vr.md` first: repositories,
status, hooks, settings, keys and the sync rules. Game facts (addresses, offsets, units) come from the recomp in
`D:\Games\Emulators\recomp` and its `docs/vr-port-notes.md`. Find and write a fact there before you port it here.

## Sync and commits

- At the start of a session: `git status`, then `git fetch --all --prune`, then report how far `HEAD` is behind
  `eglantine/nx2` and `origin/oot-gather-fix`. Do the same in `D:\Games\Emulators\recomp` against `origin/main` and
  `vr/feat/vr-openxr`.
- Commit each working step. Do not end a session with uncommitted work; make a WIP commit if needed.
- Our repo is SinCuri0/Lichii (remote `sincuri0`), a GitHub fork of zortos293/Lichii (remote `zortos`). All our work
  goes to `sincuri0`. `zortos`, `eglantine` and `origin` (Frizti/Fritchii) are read only: fetch and merge from them,
  never push. Only open a PR upstream when the owner asks.
- Ask before you push.
- Merge upstream with `git merge`, never rebase pushed branches.
- Never commit `mods/` or files made from the game (`room.bin`, captures, dumps).
- When a VR fact or status changes, update `docs/nx2/oot-vr.md` in the same commit.

## Builds

Run every build and every other long command (more than about a minute) in a Terminal panel tab with `run_in_terminal`, so the user sees the live output. Do not run them hidden in the background. Link the tab in the reply. For a build, write its output to a log file and show only progress with `& 'D:\Games\Emulators\recomp\tools\build_progress.ps1' -Log <log>` (one progress line, errors in red), not the raw output.

## Tests

Keep in-game waits short: about 10 seconds in game is enough for a test. Make counters and log lines report within that time instead of waiting longer.
