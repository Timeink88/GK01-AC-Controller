/*
 * rboot 配置结构与 Flash API
 * 
 * 适配 ESP8266 Arduino 框架，使用 SPI flash 操作实现
 * 双分区切换。基于 rboot 开源引导程序。
 * 
 * Flash 布局 (Big Flash 模式, 4MB):
 *   0x000000  rboot 引导程序 (4KB)
 *   0x001000  rboot 配置     (4KB)
 *   0x002000  ROM 0           (1016KB, bank 0 内偏移 0x2000)
 *   0x102000  ROM 1           (1016KB, bank 1 内偏移 0x2000)
 *   0x200000  LittleFS        (1004KB)
 *   0x3FC000  系统区
 *
 * 注意: ROM 1 必须在 bank 1 内与 ROM 0 在 bank 0 内的偏移相同 (0x2000)，
 * 否则 Big Flash 模式的 cache 映射与链接脚本虚拟地址不匹配。
 */

#ifndef RBOOT_H
#define RBOOT_H

#include <Arduino.h>
#include <spi_flash.h>

// rboot 配置魔数
#define RBOOT_CONFIG_MAGIC     0xE1
#define RBOOT_CONFIG_VERSION   0x01

// 最大 ROM 数量
#define RBOOT_MAX_ROMS         2

// rboot 配置所在 flash sector (sector 1 = 0x1000)
#define RBOOT_CONFIG_SECTOR    0x01
#define RBOOT_CONFIG_ADDRESS   0x1000

// ROM 物理地址
#define ROM0_ADDRESS           0x002000
#define ROM1_ADDRESS           0x102000

// LittleFS 物理地址和大小
#define RBOOT_FS_ADDRESS       0x200000
#define RBOOT_FS_SIZE          0x0FB000   // ~1004KB

// 引导模式
#define RBOOT_MODE_STANDARD    0
#define RBOOT_MODE_GPIO_ROM    1
#define RBOOT_MODE_GPIO_SKIP   2

// GPIO 回退引脚 (IO0 = GPIO0, 低电平有效)
#define RBOOT_GPIO_PIN         0
#define RBOOT_GPIO_ROM         0  // GPIO 触发时启动 ROM 0

#ifdef __cplusplus
extern "C" {
#endif

// rboot 配置结构体 (存储在 flash sector 1)
//
// 与 rboot bootloader 的 rboot_config 严格一致: bootloader 编译时
// BOOT_CONFIG_CHKSUM 未启用 (见 build_rboot.py),所以这里也不包含
// chksum 字段。magic+version+count 三个字段已足以在 rboot_config_valid()
// 中识别有效 config。
typedef struct {
    uint8_t  magic;           // 必须为 0xE1
    uint8_t  version;         // 配置版本 0x01
    uint8_t  mode;            // 引导模式
    uint8_t  current_rom;     // 当前/下次启动的 ROM (0 or 1)
    uint8_t  gpio_rom;        // GPIO 触发时启动的 ROM
    uint8_t  count;           // ROM 数量
    uint8_t  unused[2];
    uint32_t roms[RBOOT_MAX_ROMS];  // ROM 物理地址
} rboot_config;

rboot_config rboot_get_config();
bool rboot_set_config(const rboot_config& conf);
uint8_t rboot_get_current_rom();
bool rboot_set_current_rom(uint8_t rom);
uint8_t rboot_get_target_rom();
uint32_t rboot_get_rom_address(uint8_t rom);
bool rboot_write_flash(uint32_t start_addr, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
