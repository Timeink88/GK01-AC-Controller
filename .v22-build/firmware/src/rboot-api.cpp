#include "rboot.h"

static bool rboot_config_valid(const rboot_config& conf) {
    return conf.magic == RBOOT_CONFIG_MAGIC &&
           conf.version == RBOOT_CONFIG_VERSION &&
           conf.count >= 1 && conf.count <= RBOOT_MAX_ROMS;
}

rboot_config rboot_get_config() {
    rboot_config conf;
    memset(&conf, 0, sizeof(conf));
    noInterrupts();
    spi_flash_read(RBOOT_CONFIG_ADDRESS, reinterpret_cast<uint32_t*>(&conf),
                   sizeof(conf));
    interrupts();

    if (!rboot_config_valid(conf)) {
        // Config 损坏或首次启动 — 用我们的布局重建
        conf.magic = RBOOT_CONFIG_MAGIC;
        conf.version = RBOOT_CONFIG_VERSION;
        conf.mode = RBOOT_MODE_STANDARD;
        conf.current_rom = 0;
        conf.gpio_rom = RBOOT_GPIO_ROM;
        conf.count = RBOOT_MAX_ROMS;
        conf.unused[0] = 0;
        conf.unused[1] = 0;
        conf.roms[0] = ROM0_ADDRESS;
        conf.roms[1] = ROM1_ADDRESS;
        rboot_set_config(conf);
    } else if (conf.roms[0] != ROM0_ADDRESS || conf.roms[1] != ROM1_ADDRESS) {
        // rboot default_config 生成了与我们布局不同的地址（如 4MB flash
        // 默认 ROM1=0x202000，会覆盖 LittleFS），修正为我们实际的地址。
        Serial.printf("[RBOOT] Fixing ROM addresses: %u/%u -> %u/%u\n",
                      conf.roms[0], conf.roms[1], ROM0_ADDRESS, ROM1_ADDRESS);
        conf.roms[0] = ROM0_ADDRESS;
        conf.roms[1] = ROM1_ADDRESS;
        rboot_set_config(conf);
    }
    return conf;
}

bool rboot_set_config(const rboot_config& conf) {
    rboot_config copy = conf;
    noInterrupts();
    spi_flash_erase_sector(RBOOT_CONFIG_SECTOR);
    bool ok = (spi_flash_write(RBOOT_CONFIG_ADDRESS,
                               reinterpret_cast<uint32_t*>(&copy),
                               sizeof(copy)) == SPI_FLASH_RESULT_OK);
    interrupts();
    return ok;
}

uint8_t rboot_get_current_rom() {
    return rboot_get_config().current_rom;
}

bool rboot_set_current_rom(uint8_t rom) {
    if (rom >= RBOOT_MAX_ROMS) return false;
    rboot_config conf = rboot_get_config();
    if (rom >= conf.count) return false;
    conf.current_rom = rom;
    return rboot_set_config(conf);
}

uint8_t rboot_get_target_rom() {
    return rboot_get_current_rom() == 0 ? 1 : 0;
}

uint32_t rboot_get_rom_address(uint8_t rom) {
    rboot_config conf = rboot_get_config();
    if (rom >= conf.count) return 0;
    return conf.roms[rom];
}

// Write firmware data to target ROM slot.
//
// CONTRACT for callers:
//   - len MUST be a multiple of SPI_FLASH_SEC_SIZE (4KB) on every call EXCEPT
//     the final flush, which may be < 4KB.
//   - start_addr MUST be sector-aligned.
//   - Each call erases ALL sectors in [start_addr, start_addr+len) BEFORE
//     writing. Therefore a second call whose range overlaps the first will
//     destroy the previously-written bytes. Callers must stream sequentially
//     with monotonically increasing start_addr (current otaUploadHandler does
//     this — see otaAccBuf[4096] in main.cpp).
//   - Memory-mapped cache must be flushed by the caller if writing to the
//     currently-executing ROM (we never do that during OTA — we write to the
//     inactive slot).
bool rboot_write_flash(uint32_t start_addr, const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;

    uint32_t sector_start = (start_addr / SPI_FLASH_SEC_SIZE) * SPI_FLASH_SEC_SIZE;
    uint32_t sector_end = ((start_addr + len - 1) / SPI_FLASH_SEC_SIZE + 1) * SPI_FLASH_SEC_SIZE;

    // 逐 sector 擦除
    for (uint32_t sec = sector_start; sec < sector_end; sec += SPI_FLASH_SEC_SIZE) {
        uint32_t sec_num = sec / SPI_FLASH_SEC_SIZE;
        if (spi_flash_erase_sector(sec_num) != SPI_FLASH_RESULT_OK) {
            Serial.printf("[RBOOT] erase sector %u failed\n", sec_num);
            return false;
        }
        yield();
    }

    // 写入数据（spi_flash_write 要求 4 字节对齐）
    size_t written = 0;
    while (written < len) {
        uint32_t addr = start_addr + written;
        size_t remaining = len - written;
        // 每次最多写 4KB
        size_t chunk = (remaining > SPI_FLASH_SEC_SIZE) ? SPI_FLASH_SEC_SIZE : remaining;

        // 对齐到 4 字节
        size_t aligned = (chunk + 3) & ~3;
        uint8_t* buf = nullptr;
        bool needs_free = false;

        if (aligned != chunk) {
            buf = (uint8_t*)malloc(aligned);
            if (!buf) return false;
            memset(buf, 0xFF, aligned);
            memcpy(buf, data + written, chunk);
            needs_free = true;
        } else {
            buf = (uint8_t*)(data + written);
        }

        noInterrupts();
        bool ok = (spi_flash_write(addr, reinterpret_cast<uint32_t*>(buf), aligned)
                   == SPI_FLASH_RESULT_OK);
        interrupts();

        if (needs_free) free(buf);
        if (!ok) {
            Serial.printf("[RBOOT] write at 0x%06X failed\n", addr);
            return false;
        }

        written += chunk;
        yield();
    }

    return true;
}
