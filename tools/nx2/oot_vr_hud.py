"""Assembles oot_vr_hud.s for main+0x3C46900 into the byte arrays in patch_manager.cpp (needs MSYS2 clang, ld.lld)."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CAVE = 0x3C46900
HOOK = 0x340A4B4
SYMBOLS = {
    "task_flush": 0x338B4C0,
    "ui_screen_by_hash": 0x837240,
    "ui_hud_shown": 0xA687F0,
    "find_pane": 0x43E1E0,
    "set_translate": 0x20A3E0,
    "set_scale": 0xB29B20,
    "ui_manager_ptr": 0x4C7E098,
    "hud_params": 0x4E9F800,
}
TOOLS = r"C:\msys64\ucrt64\bin"


def main():
    with tempfile.TemporaryDirectory() as tmp:
        obj, out = os.path.join(tmp, "hud.o"), os.path.join(tmp, "hud.bin")
        subprocess.run([os.path.join(TOOLS, "clang.exe"), "--target=aarch64-none-elf", "-c",
                        os.path.join(HERE, "oot_vr_hud.s"), "-o", obj], check=True)
        link = [os.path.join(TOOLS, "ld.lld.exe"), "-m", "aarch64elf", "--oformat=binary", f"-Ttext={CAVE:#x}", "-e", "hud",
                obj, "-o", out]
        link += [f"--defsym={name}={address:#x}" for name, address in SYMBOLS.items()]
        subprocess.run(link, check=True)
        code = open(out, "rb").read()
    call = 0x94000000 | (((CAVE - HOOK) >> 2) & 0x3FFFFFF)
    print(f"// bl at main+{HOOK:#x}: {', '.join(f'0x{b:02X}' for b in call.to_bytes(4, 'little'))}")
    print(f"// {len(code)} bytes at main+{CAVE:#x}")
    for i in range(0, len(code), 16):
        print("    " + " ".join(f"0x{b:02X}," for b in code[i:i + 16]))
    sys.stderr.write(f"{len(code)} bytes\n")


if __name__ == "__main__":
    main()
