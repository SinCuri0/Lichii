"""Ocarina of Time VR camera test: boots the game, loads the first save, and takes screenshots
with the test head pose straight, turned left and turned right (svc_nx2_oot_vr.cpp).

python -I tools/nx2/oot_vr_test.py --exe D:/lxc/bin/lichii.exe --game <file.nup> --out <dir>
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import debug_mcp  # noqa: E402
PORT = "127.0.0.1:6546"
TEST_POSE_OFFSET = 0x4E9FFF0


def call(method, params=None, out=None, timeout=300):
    cmd = [sys.executable, "-I", os.path.join(HERE, "debug_mcp.py"), "--server", PORT,
           "--call", json.dumps({"method": method, "params": params or {}})]
    if out:
        cmd += ["--out", out]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"{method}: {r.stderr.strip() or r.stdout.strip()}")
    return json.loads(r.stdout)


def presents():
    return call("status")["presents"]


def wait_frames(n):
    call("input_sequence", {"steps": [{"frames": n}]})


def press(button, then=120):
    call("input_sequence", {"steps": [{"buttons": [button], "frames": 6}, {"frames": then}]})


def main_base(log_dir):
    paths = [os.path.join(log_dir, n) for n in ("eden_log.txt", "eden_log.busy.txt")]
    paths = [p for p in paths if os.path.exists(p)]
    if not paths:
        return None
    text = open(max(paths, key=os.path.getmtime), encoding="utf-8", errors="replace").read()
    found = re.findall(r"OoT VR camera: set-view hook live, pc \S+, main at (0x[0-9a-f]+)", text)
    return int(found[-1], 16) if found else None


def in_game(log_dir):
    path = os.path.join(log_dir, "eden_log.txt")
    return os.path.exists(path) and "camera-source hook live" in open(
        path, encoding="utf-8", errors="replace").read()


def set_pose(base, mode, yaw=0.0, pitch=0.0):
    data = struct.pack("<Iff", mode, yaw, pitch).hex()
    # debug_mcp.py's tool list has no write_memory; the server has it.
    emulator = debug_mcp.Emulator(PORT)
    try:
        emulator.call("write_memory", {"address": hex(base + TEST_POSE_OFFSET), "hex": data}, 30)
    finally:
        emulator.close()


def unmap_pads(config):
    """The test copy must not read the owner's controller (injected input adds to it)."""
    text = open(config, encoding="utf-8").read()
    unmapped = re.sub(r'^(player_\d+_[a-z0-9_]+)="[^"]*engine:sdl[^"]*"', r'\1=""', text, flags=re.M)
    open(config, "w", encoding="utf-8", newline="").write(unmapped)
    return text


def step(text):
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--headset", action="store_true", help="also start OpenXR (SteamVR)")
    ap.add_argument("--views", default="1", help="EDEN_NX2_OOT_VR_VIEWS: bit n turns view n")
    ap.add_argument("--hold", type=float, default=0, help="seconds with the real head pose before the test turns")
    ap.add_argument("--sweep", type=float, default=0, help="slow turn: degrees per shot from -60 to 60")
    ap.add_argument("--fake-tan", default="1.842,1.330", help="headset FOV tangents without a headset")
    ap.add_argument("--play", action="store_true", help="load the save, then leave the game open: no shots, no test turns")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log_dir = os.path.join(os.path.dirname(a.exe), "user", "log")
    env = dict(os.environ, EDEN_NX2_DEBUG_SERVER=PORT, EDEN_NX2_OOT_VR="1",
               EDEN_NX2_OOT_VR_XR="1" if a.headset else "0", EDEN_NX2_OOT_VR_VIEWS=a.views)
    if not a.headset and a.fake_tan:
        env["EDEN_NX2_OOT_VR_FAKE_TAN"] = a.fake_tan
    env.pop("EDEN_NX2_OOT_VR_FAKE_YAW", None)
    env.pop("EDEN_NX2_OOT_VR_FAKE_PITCH", None)
    config = os.path.join(os.path.dirname(a.exe), "user", "config", "qt-config.ini")
    original = open(config, encoding="utf-8").read() if a.play else unmap_pads(config)
    step("starting the emulator" if a.play else "starting the emulator (its controller unmapped until the test ends)")
    proc = subprocess.Popen([a.exe, "-g", a.game], env=env)
    try:
        for _ in range(120):
            try:
                call("status", timeout=10)
                break
            except Exception:
                time.sleep(2)
        step("waiting for the game's first frame")
        while presents() == 0:
            time.sleep(1)
        step("game running: waiting 10 s for the title")
        time.sleep(10)
        for n in range(4):
            step(f"A press {n + 1} of 4")
            press("A", 60)
        step("loading the save: waiting for the game camera")
        for n in range(60):
            if in_game(log_dir):
                break
            if n % 10 == 9:
                step("not in the game yet: pressing A again")
                press("A", 30)
            time.sleep(1)
        else:
            raise RuntimeError("the game camera did not start in 60 s")
        step("IN THE GAME NOW")
        base = main_base(log_dir)
        if base is None:
            raise RuntimeError("no 'set-view hook live' line in the log: the VR patch did not run")
        step(f"main at {base:#x}")
        first, t0 = presents(), time.time()
        time.sleep(5)
        step(f"frame rate: {(presents() - first) / (time.time() - t0):.1f} presents per second")
        if a.play:
            step("PLAY NOW: close the game window when you are done")
            proc.wait()
            return
        call("screenshot", out=os.path.join(a.out, "vr_0_head_pose.png"))
        step("shot with the real head pose (or none without a headset)")
        if a.hold > 0:
            step(f"LOOK AROUND NOW: {a.hold:.0f} s with your real head pose")
            time.sleep(a.hold)
        if a.sweep > 0:
            steps = int(120 / a.sweep)
            for n in range(steps + 1):
                yaw = -60.0 + n * a.sweep
                set_pose(base, 1, yaw)
                wait_frames(6)
                call("screenshot", out=os.path.join(a.out, f"sweep_{n:02d}_{yaw:+04.0f}.png"))
            step(f"sweep done: {steps + 1} shots")
        for name, mode, yaw in (("1_straight", 1, 0.0), ("2_left60", 1, 60.0), ("3_straight", 1, 0.0),
                                ("4_right60", 1, -60.0), ("5_straight", 1, 0.0), ("6_off", 2, 0.0)):
            set_pose(base, mode, yaw)
            wait_frames(20)
            shot = os.path.join(a.out, f"vr_{name}.png")
            call("screenshot", out=shot)
            step(f"shot {shot}")
        step("done")
    finally:
        proc.terminate()
        proc.wait(timeout=30)
        open(config, "w", encoding="utf-8", newline="").write(original)


if __name__ == "__main__":
    main()
