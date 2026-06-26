#!/usr/bin/env python3
"""
Generate combined flash image: rboot + ROM0 in a single .bin
so users flash with one esptool command instead of two.
"""
import sys
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RBOOT = ROOT / "firmware/rboot-bootloader/rboot.bin"
ROM0 = ROOT / "firmware/flash_images/rom0.bin"
OUTPUT = ROOT / "firmware/flash_images/combined.bin"

RBOOT_MAX_SIZE = 0x2000
RBOOT_CONFIG_OFFSET = 0x1000
ROM0_ADDRESS = 0x002000
ROM1_ADDRESS = 0x102000


def make_rboot_config():
    sector = bytearray(b"\xff" * 0x1000)
    sector[:16] = struct.pack("<BBBBBBBBII",
                              0xE1, 0x01, 0x00, 0x00,
                              0x00, 0x02, 0x00, 0x00,
                              ROM0_ADDRESS, ROM1_ADDRESS)
    return sector


def main():
    if not RBOOT.exists():
        print(f"ERROR: {RBOOT} not found. Build rboot first.")
        sys.exit(1)
    if not ROM0.exists():
        print(f"ERROR: {ROM0} not found. Run prepare_flash.py first.")
        sys.exit(1)

    rboot_data = RBOOT.read_bytes()
    rom0_data = ROM0.read_bytes()

    if len(rboot_data) > RBOOT_MAX_SIZE:
        print(f"ERROR: rboot.bin too large: {len(rboot_data)} > {RBOOT_MAX_SIZE}")
        sys.exit(1)

    prefix = bytearray(b"\xff" * RBOOT_MAX_SIZE)
    prefix[:len(rboot_data)] = rboot_data
    prefix[RBOOT_CONFIG_OFFSET:RBOOT_CONFIG_OFFSET + 0x1000] = make_rboot_config()

    with open(OUTPUT, "wb") as f:
        f.write(prefix)
        f.write(rom0_data)

    total = OUTPUT.stat().st_size
    print(f"  rboot:   {len(rboot_data):>7} bytes")
    print(f"  config:  {0x1000:>7} bytes (ROM0=0x2000, ROM1=0x102000)")
    print(f"  ROM0:    {len(rom0_data):>7} bytes")
    print(f"  TOTAL:   {total:>7} bytes ({total / 1024:.1f} KB)")
    print(f"  Layout:  0x0000 rboot | 0x1000 config | 0x2000 ROM0")
    print(f"  Flash:   esptool write-flash -fm dout 0x0 {OUTPUT.name}")


if __name__ == "__main__":
    main()
