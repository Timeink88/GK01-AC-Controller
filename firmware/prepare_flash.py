#!/usr/bin/env python3
"""
Post-build script for rboot dual-partition firmware.

Strips eboot from PlatformIO firmware.bin and generates
ready-to-flash binary images.

Usage: python prepare_flash.py [rom0|rom1|both]
"""

import sys
import struct
from pathlib import Path

FIRMWARE_DIR = Path(__file__).parent
BUILD_ROM0 = FIRMWARE_DIR / ".pio/build/rom0/firmware.bin"
BUILD_ROM1 = FIRMWARE_DIR / ".pio/build/rom1/firmware.bin"
BUILD_DIAG = FIRMWARE_DIR / ".pio/build/diag/firmware.bin"
OUTPUT_DIR = FIRMWARE_DIR / "flash_images"
RBOOT_BIN = FIRMWARE_DIR / "rboot-bootloader/rboot.bin"

EBOOT_SIZE = 0x1000
RBOOT_PAD_SIZE = 0x2000
RBOOT_CONFIG_OFFSET = 0x1000
ROM0_ADDRESS = 0x002000
ROM1_ADDRESS = 0x102000
ESP_IMAGE_MAGIC = 0xE9
ESP_FLASH_MODE_DOUT = 0x03


def make_rboot_config():
    # rboot_config with MAX_ROMS=2, BOOT_CONFIG_CHKSUM disabled:
    # magic, version, mode, current_rom, gpio_rom, count, unused[2], roms[2]
    sector = bytearray(b"\xff" * 0x1000)
    sector[:16] = struct.pack("<BBBBBBBBII",
                              0xE1, 0x01, 0x00, 0x00,
                              0x00, 0x02, 0x00, 0x00,
                              ROM0_ADDRESS, ROM1_ADDRESS)
    return sector


def strip_eboot(src, dst):
    if not src.exists():
        print(f"ERROR: {src} not found. Build first with: pio run -e rom0")
        return False
    data = src.read_bytes()
    app_data = bytearray(data[EBOOT_SIZE:])
    if len(app_data) < 4 or app_data[0] != ESP_IMAGE_MAGIC:
        print(f"ERROR: stripped image has invalid ESP8266 header: {src}")
        return False
    if app_data[2] != ESP_FLASH_MODE_DOUT:
        print(f"  forcing {dst.name} flash mode {app_data[2]} -> DOUT")
        app_data[2] = ESP_FLASH_MODE_DOUT
    dst.write_bytes(app_data)
    print(f"  {src.name} ({len(data)}B) -> {dst.name} ({len(app_data)}B, stripped eboot)")
    return True


def make_combined(rom0_path, name="combined.bin"):
    dst = OUTPUT_DIR / name
    rboot_data = RBOOT_BIN.read_bytes()
    rom0_data = rom0_path.read_bytes()

    if len(rboot_data) > RBOOT_PAD_SIZE:
        print(f"ERROR: rboot.bin too large: {len(rboot_data)} > {RBOOT_PAD_SIZE}")
        return False

    prefix = bytearray(b"\xff" * RBOOT_PAD_SIZE)
    prefix[:len(rboot_data)] = rboot_data
    prefix[RBOOT_CONFIG_OFFSET:RBOOT_CONFIG_OFFSET + 0x1000] = make_rboot_config()

    with dst.open("wb") as f:
        f.write(prefix)
        f.write(rom0_data)

    print(
        f"  {name} ({dst.stat().st_size}B, "
        "0x0000 rboot + 0x1000 config + 0x2000 ROM0)"
    )
    return True


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "both"

    OUTPUT_DIR.mkdir(exist_ok=True)

    if not RBOOT_BIN.exists():
        print(f"ERROR: {RBOOT_BIN} not found. Build first: python rboot-bootloader/build_rboot.py")
        sys.exit(1)

    print(f"rboot.bin: {RBOOT_BIN.stat().st_size} bytes")

    rom0_path = OUTPUT_DIR / "rom0.bin"
    rom0_ready = False

    if mode in ("rom0", "both"):
        print("Preparing ROM 0...")
        rom0_ready = strip_eboot(BUILD_ROM0, rom0_path)

    if mode in ("rom1", "both"):
        print("Preparing ROM 1...")
        strip_eboot(BUILD_ROM1, OUTPUT_DIR / "rom1.bin")

    if mode in ("diag",):
        print("Preparing diagnostic ROM...")
        diag_path = OUTPUT_DIR / "diag.bin"
        if strip_eboot(BUILD_DIAG, diag_path):
            print("Preparing diagnostic combined image...")
            make_combined(diag_path, "diag-combined.bin")

    if rom0_ready:
        print("Preparing combined first-flash image...")
        make_combined(rom0_path)

    print()
    print(f"Output: {OUTPUT_DIR}/")
    for f in OUTPUT_DIR.iterdir():
        print(f"  {f.name}: {f.stat().st_size} bytes")
    print()
    print("Flash commands (USB-TTL, IO0 shorted to GND):")
    print()
    print("  # Recovery / first flash (recommended)")
    print("  # 1. Erase entire flash")
    print("  python -m esptool --port COMx --baud 115200 --before no-reset erase-flash")
    print()
    print("  # 2. Flash combined image at 0x0 (includes rboot config)")
    print(
        "  python -m esptool --port COMx --baud 115200 --before no-reset "
        "write-flash -fm dout 0x0 flash_images/combined.bin"
    )
    print()
    print("  # Advanced split flash only (erase first, or stale rboot config may boot a bad slot)")
    print("  # Flash rboot bootloader at 0x0000")
    print(
        "  python -m esptool --port COMx --baud 115200 --before no-reset "
        "write-flash -fm dout 0x0000 rboot-bootloader/rboot.bin"
    )
    print()
    print("  # Flash ROM 0 application at 0x2000")
    print(
        "  python -m esptool --port COMx --baud 115200 --before no-reset "
        "write-flash -fm dout 0x2000 flash_images/rom0.bin"
    )
    print()
    print("  # Optional: flash ROM 1 for testing at 0x102000")
    print(
        "  python -m esptool --port COMx --baud 115200 --before no-reset "
        "write-flash -fm dout 0x102000 flash_images/rom1.bin"
    )
    print()
    print("Do NOT flash .pio/build/.../firmware.bin to 0x0 for this rboot layout.")
    print("Do NOT upload combined.bin, rboot.bin, or .pio/build/.../firmware.bin in WebUI OTA.")
    print("For WebUI OTA, upload the alternate ROM shown by the WebUI: flash_images/rom0.bin or flash_images/rom1.bin.")
    print()
    print("combined.bin includes rboot config: ROM0=0x2000, ROM1=0x102000.")
    print("OTA updates will write to the alternate ROM automatically.")


if __name__ == "__main__":
    main()
