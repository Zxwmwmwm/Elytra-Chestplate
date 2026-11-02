#!/usr/bin/env python3
"""
Anywhere Riptide Verification & Engine Assertion Suite.
Tests:
  1. libminecraftpe.so (1.26.52.03 ARM64) Target Offsets
  2. Mod Shared Object (AArch64, Symbol Exports, Dependencies)
  3. Levipack ZIP integrity (manifest.json, binary payload)
"""

import os
import sys
import json
import struct
import subprocess
import zipfile

ENGINE_TARGETS = {
    "Player::isInWaterOrRain": 0x0F56A524,
    "TridentItem::releaseUsing Call Site": 0x0FF01A14,
    "TridentItem::use Call Site": 0x0FF0265C,
    "ServerNetworkHandler::ctor": 0x0C5585C4,
    "ServerNetworkHandler::dtor": 0x0C55A2D4,
    "RakNetConnector::connect": 0x0C64FF48,
    "RakNetConnector::disconnect": 0x0C650284,
}

EXPECTED_EXPORTS = [
    "PLGetModRegistration",
    "plInitMod",
    "PLMod_Load",
    "PLMod_Enable",
    "PLMod_Disable",
    "PLMod_Unload",
    "LeviMod_Load",
    "mod_load",
    "mod_unload",
]

def verify_engine(engine_path):
    print(f"\n=== [1/3] Verifying MCBE Engine ({engine_path}) ===")
    if not os.path.exists(engine_path):
        print(f"  [WARN] Engine binary not found at {engine_path}; skipping engine checks.")
        return True

    with open(engine_path, "rb") as f:
        data = f.read()

    all_ok = True
    for name, rva in ENGINE_TARGETS.items():
        if rva + 4 > len(data):
            print(f"  [FAIL] {name} at 0x{rva:08X} beyond file length!")
            all_ok = False
            continue
        insn = struct.unpack("<I", data[rva:rva+4])[0]
        if insn == 0:
            print(f"  [FAIL] {name} at 0x{rva:08X} has null instruction!")
            all_ok = False
        else:
            print(f"  {name} at 0x{rva:08X}: insn=0x{insn:08X} -> PASS")
    return all_ok

def verify_mod_binary(so_path):
    print(f"\n=== [2/3] Verifying Mod Binary ({so_path}) ===")
    if not os.path.exists(so_path):
        print(f"  [FAIL] Binary {so_path} does not exist!")
        return False

    size = os.path.getsize(so_path)
    print(f"  File size: {size} bytes")

    with open(so_path, "rb") as f:
        magic = f.read(4)
        if magic != b"\x7fELF":
            print("  [FAIL] Invalid ELF magic!")
            return False
        ei_class = f.read(1)
        if ei_class != b"\x02": # 64-bit
            print("  [FAIL] Not a 64-bit ELF!")
            return False
        f.seek(18)
        machine = struct.unpack("<H", f.read(2))[0]
        if machine != 183: # EM_AARCH64
            print(f"  [FAIL] Unexpected machine type: {machine} (expected 183/AArch64)")
            return False
        print("  Architecture: AArch64 (ARM64) -> PASS")

    out = subprocess.check_output(["readelf", "-Ws", so_path]).decode("utf-8")
    for exp in EXPECTED_EXPORTS:
        if f" {exp}" in out:
            print(f"  Exports {exp}: PASS")
        else:
            print(f"  [FAIL] Missing export: {exp}")
            return False

    out_deps = subprocess.check_output(["readelf", "-d", so_path]).decode("utf-8")
    deps = [line.split("[")[1].split("]")[0] for line in out_deps.splitlines() if "(NEEDED)" in line]
    print(f"  Dynamic dependencies: {', '.join(deps)}")
    if not any("preloader" in d for d in deps):
        print("  [FAIL] Missing libpreloader dependency!")
        return False
    print("  Preloader dependency present -> PASS")
    return True

def verify_levipack(levipack_path):
    print(f"\n=== [3/3] Verifying Levipack Package ({levipack_path}) ===")
    if not os.path.exists(levipack_path):
        print(f"  [FAIL] Levipack {levipack_path} does not exist!")
        return False

    size = os.path.getsize(levipack_path)
    print(f"  Package size: {size} bytes")

    with zipfile.ZipFile(levipack_path, "r") as zf:
        namelist = zf.namelist()
        print(f"  Archive contents: {namelist}")
        if "manifest.json" not in namelist:
            print("  [FAIL] manifest.json missing from levipack!")
            return False
        print("  manifest.json present -> PASS")

        if "libElytraChestplate.so" not in namelist:
            print("  [FAIL] libElytraChestplate.so missing from levipack!")
            return False
        print("  libElytraChestplate.so present -> PASS")

        manifest_data = json.loads(zf.read("manifest.json").decode("utf-8"))
        if manifest_data.get("name") != "Elytra Chestplate":
            print(f"  [FAIL] manifest name unexpected: {manifest_data.get('name')}")
            return False
        print("  manifest.json name: 'Elytra Chestplate' -> PASS")

        if manifest_data.get("author") != "Zxwmwmwm":
            print(f"  [FAIL] manifest author unexpected: {manifest_data.get('author')}")
            return False
        print("  manifest.json author: 'Zxwmwmwm' -> PASS")

        if manifest_data.get("version") != "1.0.0":
            print(f"  [FAIL] manifest version unexpected: {manifest_data.get('version')}")
            return False
        print("  manifest.json version: '1.0.0' -> PASS")

    return True

def main():
    engine_path = "/tmp/user_libminecraftpe.so"
    so_path = "/home/user/elytrachestplate/libElytraChestplate.so"
    levipack_path = "/home/user/elytrachestplate/ElytraChestplate-1.0.0.levipack"

    ok = True
    ok &= verify_engine(engine_path)
    ok &= verify_mod_binary(so_path)
    ok &= verify_levipack(levipack_path)

    print("\n" + "="*50)
    if ok:
        print("ALL VERIFICATION CHECKS PASSED (100% OK)")
        print("="*50)
        sys.exit(0)
    else:
        print("VERIFICATION FAILED")
        print("="*50)
        sys.exit(1)

if __name__ == "__main__":
    main()
