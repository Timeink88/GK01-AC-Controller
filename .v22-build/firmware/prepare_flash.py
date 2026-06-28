#!/usr/bin/env python3
"""
Post-build script for rboot dual-partition firmware.

Strips eboot from PlatformIO firmware.bin and generates
ready-to-flash binary images.

Usage: python prepare_flash.py [rom0|rom1|both]
"""

import sys
from pathlib import Path

FIRMWARE_DIR = Path(__file__).parent
BUILD_ROM0 = FIRMWARE_DIR / ".pio/build/rom0/firmware.bin"
BUILD_ROM1 = FIRMWARE_DIR / ".pio/build/rom1/firmware.bin"
OUTPUT_DIR = FIRMWARE_DIR / "flash_images"
RBOOT_BIN = FIRMWARE_DIR / "rboot-bootloader/rboot.bin"

EBOOT_SIZE = 0x1000


def strip_eboot(src, dst):
    if not src.exists():
        print(f"ERROR: {src} not found. Build first with: pio run -e rom0")
        return False
    data = src.read_bytes()
    app_data = data[EBOOT_SIZE:]
    dst.write_bytes(app_data)
    print(f"  {src.name} ({len(data)}B) -> {dst.name} ({len(app_data)}B, stripped eboot)")
    return True


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "both"

    OUTPUT_DIR.mkdir(exist_ok=True)

    if not RBOOT_BIN.exists():
        print(f"ERROR: {RBOOT_BIN} not found. Build first: python rboot-bootloader/build_rboot.py")
        sys.exit(1)

    print(f"rboot.bin: {RBOOT_BIN.stat().st_size} bytes")

    if mode in ("rom0", "both"):
        print("Preparing ROM 0...")
        strip_eboot(BUILD_ROM0, OUTPUT_DIR / "rom0.bin")

    if mode in ("rom1", "both"):
        print("Preparing ROM 1...")
        strip_eboot(BUILD_ROM1, OUTPUT_DIR / "rom1.bin")

    print()
    print(f"Output: {OUTPUT_DIR}/")
    for f in OUTPUT_DIR.iterdir():
        print(f"  {f.name}: {f.stat().st_size} bytes")
    print()
    print("Flash commands (USB-TTL, IO0 shorted to GND):")
    print()
    print("  # 1. Erase entire flash (required for first time)")
    print("  python -m esptool --port COMx --baud 115200 --before no-reset erase-flash")
    print()
    print("  # 2. Flash rboot bootloader at 0x0000")
    print("  python -m esptool --port COMx --baud 115200 --before no-reset write-flash -fm dout 0x0000 rboot-bootloader/rboot.bin")
    print()
    print("  # 3. Flash ROM 0 application at 0x2000")
    print("  python -m esptool --port COMx --baud 115200 --before no-reset write-flash -fm dout 0x2000 flash_images/rom0.bin")
    print()
    print("  # 4. (Optional) Flash ROM 1 for testing")
    print("  python -m esptool --port COMx --baud 115200 --before no-reset write-flash -fm dout 0x102000 flash_images/rom1.bin")
    print()
    print("After first boot, rboot will auto-create the default config.")
    print("OTA updates will write to the alternate ROM automatically.")


if __name__ == "__main__":
    main()
