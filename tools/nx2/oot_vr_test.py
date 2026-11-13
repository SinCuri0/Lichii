"""Ocarina of Time VR camera test: boots the game, loads the first save, and takes screenshots
with the test head pose straight, turned left and turned right (svc_nx2_oot_vr.cpp).

python -I tools/nx2/oot_vr_test.py --exe D:/lxc/bin/lichii.exe --game <file.nup> --out <dir>
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import debug_mcp  # noqa: E402
PORT = "127.0.0.1:6546"
TEST_POSE_OFFSET = 0x4E9FFF0
# Name, yaw (positive is left) and pitch (positive is up) of each cube face.
CAPTURE_FACES = (("front", 0.0, 0.0), ("left", 90.0, 0.0), ("back", 180.0, 0.0), ("right", -90.0, 0.0),
                 ("up", 0.0, 89.0), ("down", 0.0, -89.0))
# Name and head position (metres: x right, y up, z back) of each capture spot.
CAPTURE_RADIUS = 0.25
CAPTURE_SPOTS = (("centre", 0.0, 0.0, 0.0), ("right", CAPTURE_RADIUS, 0.0, 0.0), ("front", 0.0, 0.0, -CAPTURE_RADIUS),
                 ("left", -CAPTURE_RADIUS, 0.0, 0.0), ("back", 0.0, 0.0, CAPTURE_RADIUS))


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
    write_test_pose(base, struct.pack("<Iff", mode, yaw, pitch))


def set_position(base, x, y, z):
    """Test head position in metres; later set_pose calls keep it."""
    write_test_pose(base, struct.pack("<Ifff", 3, x, y, z))


def write_test_pose(base, packed):
    data = packed.hex()
    # debug_mcp.py's tool list has no write_memory; the server has it.
    emulator = debug_mcp.Emulator(PORT)
    try:
        emulator.call("write_memory", {"address": hex(base + TEST_POSE_OFFSET), "hex": data}, 30)
    finally:
        emulator.close()


def read_hud_params(base):
    emulator = debug_mcp.Emulator(PORT)
    try:
        r = emulator.call("read_memory", {"address": hex(base + 0x4E9F800), "size": 0x80, "as": "hex"}, 30)
        h = r["hex"] if isinstance(r, dict) and "hex" in r else r.get("data")
        raw = bytes.fromhex(h)
        return struct.unpack("<8f", raw[:32]), struct.unpack("<I", raw[32:36])[0], struct.unpack("<8f", raw[0x30:0x50])
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
    ap.add_argument("--pitch", type=float, default=0, help="degrees up during the sweep")
    ap.add_argument("--fake-tan", default="1.842,1.330", help="headset FOV tangents without a headset")
    ap.add_argument("--run", action="store_true", help="run forward and roll, with shots during it")
    ap.add_argument("--menu", default="", help="buttons to press after loading, e.g. Plus,R,Plus, with a shot after each")
    ap.add_argument("--hold-buttons", default="", help="buttons to hold for 3 s each after loading, e.g. L,R, with a shot during each")
    ap.add_argument("--capture", action="store_true",
                    help="frame dumps (colour and depth) in 6 directions from each spot into <out>/<spot>_<face>; use --fake-tan 1,1")
    ap.add_argument("--poke", default="", help="code bytes to write after loading, e.g. 33f9250=c0035fd6 (main offset=hex)")
    ap.add_argument("--play",action="store_true", help="load the save, then leave the game open: no shots, no test turns")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log_dir = os.path.join(os.path.dirname(a.exe), "user", "log")
    env = dict(os.environ, EDEN_NX2_DEBUG_SERVER=PORT, EDEN_NX2_OOT_VR="1",
               EDEN_NX2_OOT_VR_XR="1" if a.headset else "0", EDEN_NX2_OOT_VR_VIEWS=a.views,
               EDEN_NX2_OOT_VR_UI_TRACE="1")
    if not a.headset and a.fake_tan:
        env["EDEN_NX2_OOT_VR_FAKE_TAN"] = a.fake_tan
    if a.hold_buttons and not a.capture:
        os.makedirs(os.path.join(a.out, "dumps"), exist_ok=True)
        env.update(EDEN_NX2_FRAME_DUMP=os.path.join(a.out, "dumps"), EDEN_NX2_FRAME_DUMP_AT="file",
                   EDEN_NX2_FRAME_DUMP_FINAL="1")
    if a.capture:
        env.update(EDEN_NX2_FRAME_DUMP=os.path.join(a.out, "dumps"), EDEN_NX2_FRAME_DUMP_AT="file",
                   EDEN_NX2_FRAME_DUMP_CAP_MIB="16384", EDEN_NX2_OOT_VR_CAPTURE="1", EDEN_NX2_FRAME_DUMP_FINAL="1")
    env.pop("EDEN_NX2_OOT_VR_FAKE_YAW", None)
    env.pop("EDEN_NX2_OOT_VR_FAKE_PITCH", None)
    config = os.path.join(os.path.dirname(a.exe), "user", "config", "qt-config.ini")
    # A run stopped before its end leaves this file: its config still has the owner's controller.
    backup = config + ".vrtest-backup"
    if os.path.exists(backup):
        os.replace(backup, config)
        step("restored the controller settings an earlier stopped run left unmapped")
    if not a.play:
        shutil.copyfile(config, backup)
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
        for poke in filter(None, a.poke.split(",")):
            offset, data = poke.split("=")
            call("write_memory", {"address": hex(base + int(offset, 16)), "hex": data})
            step(f"wrote {data} at main+{offset}")
        first, t0 = presents(), time.time()
        time.sleep(5)
        step(f"frame rate: {(presents() - first) / (time.time() - t0):.1f} presents per second")
        if a.play:
            step("PLAY NOW: close the game window when you are done")
            proc.wait()
            return
        if a.capture:
            for spot, x, y, z in CAPTURE_SPOTS:
                set_position(base, x, y, z)
                wait_frames(5)
                for name, yaw, pitch in CAPTURE_FACES:
                    set_pose(base, 1, yaw, pitch)
                    wait_frames(20)
                    result = call("capture_frame", {"timeout_s": 300, "manifest_lines": 0}, timeout=400)
                    face = os.path.join(a.out, f"{spot}_{name}")
                    shutil.rmtree(face, ignore_errors=True)
                    shutil.move(result["directory"], face)
                    step(f"captured {spot} {name}: {face}")
            set_position(base, 0.0, 0.0, 0.0)
            set_pose(base, 1, 0.0)
            step("capture done")
            return
        call("screenshot", out=os.path.join(a.out, "vr_0_head_pose.png"))
        step("shot with the real head pose (or none without a headset)")
        if a.hold > 0:
            step(f"LOOK AROUND NOW: {a.hold:.0f} s with your real head pose")
            time.sleep(a.hold)
        if a.run:
            set_pose(base, 1, 0.0)
            steps =[{"left_stick": [0, 1], "frames": 40}, {"left_stick": [0, 1], "buttons": ["A"], "frames": 6},
                     {"left_stick": [0, 1], "frames": 40}]
            runner = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "debug_mcp.py"), "--server", PORT,
                                       "--call", json.dumps({"method": "input_sequence", "params": {"steps": steps}})])
            for n in range(10):
                time.sleep(0.3)
                call("screenshot", out=os.path.join(a.out, f"run_{n:02d}.png"))
            runner.wait(timeout=60)
            step("run done: 10 shots")
        for button in filter(None, a.hold_buttons.split(",")):
            steps = [{"buttons": [button], "frames": 600}, {"frames": 60}]
            holder = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "debug_mcp.py"), "--server", PORT,
                                       "--call", json.dumps({"method": "input_sequence", "params": {"steps": steps}})])
            # The debug server answers one request at a time, so a screenshot would wait for the hold to end: use a frame dump.
            time.sleep(3)
            open(os.path.join(a.out, "dumps", "capture.now"), "w").close()
            step(f"held {button}: frame dump requested")
            time.sleep(4)
            if os.environ.get("EDEN_NX2_OOT_VR_MENU_TEST_YAW"):
                # A second dump after Lichii turned the test head pose (120 frames into the menu).
                open(os.path.join(a.out, "dumps", "capture.now"), "w").close()
                step(f"held {button}: second frame dump requested (head turned)")
                time.sleep(4)
            holder.wait(timeout=60)
            time.sleep(2)
        if a.hold_buttons:
            # Draws in the frame dumped during each hold: a few hundred means the game stopped drawing the world.
            manifest = os.path.join(a.out, "dumps", "manifest.jsonl")
            if os.path.exists(manifest):
                for line in open(manifest, encoding="utf-8"):
                    entry = json.loads(line)
                    if entry.get("kind") == "present":
                        step(f"draws in the dumped frame: {entry.get('after_draws')}")
            step("done")
            return
        def sweep(prefix):
            steps = int(120 / a.sweep)
            for n in range(steps + 1):
                yaw = -60.0 + n * a.sweep
                set_pose(base, 1, yaw, a.pitch)
                wait_frames(6)
                call("screenshot", out=os.path.join(a.out, f"{prefix}{n:02d}_{yaw:+04.0f}.png"))
            set_pose(base, 1, 0.0)
            step(f"sweep done: {steps + 1} shots")

        if a.menu:
            if a.sweep > 0:
                sweep("before_")
            for n, button in enumerate(a.menu.split(",")):
                press(button, 90)
                time.sleep(3)
                call("screenshot", out=os.path.join(a.out, f"menu_{n}_{button}.png"))
                step(f"menu: pressed {button}")
            if a.sweep > 0:
                sweep("after_")
            step("menu done")
            return
        if a.sweep > 0:
            sweep("sweep_")
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
        if os.path.exists(backup):
            os.remove(backup)


if __name__ == "__main__":
    main()
