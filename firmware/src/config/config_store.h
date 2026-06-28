#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <LittleFS.h>

#define FORCE_MODE_AUTO   0
#define FORCE_MODE_AP     1
#define FORCE_MODE_SLAVE  2
#define FORCE_MODE_HOME   3

struct Config {
    char     ap_ssid[33] = "";
    char     ap_pass[33] = "";
    char     sta_ssid[64] = "";
    char     sta_pass[64] = "";
    char     mqtt_host[64] = "";
    uint16_t mqtt_port = 1883;
    char     mqtt_user[32] = "";
    char     mqtt_pass[32] = "";
    char     mqtt_topic[32] = "ir_ac";
    uint8_t  force_mode = FORCE_MODE_AUTO;
    char     paired_master_bssid[18] = "";
    char     last_vendor[16] = "GREE";
    char     last_mode[8]   = "Cool";
    uint8_t  last_temp      = 26;
    char     last_fan[8]    = "Auto";
    bool     last_power     = false;
    bool     last_turbo     = false;
    bool     last_econo     = false;
    bool     last_sleep     = false;
    bool     last_light     = true;
    bool     last_clean     = false;
    char     device_name[33] = "";
    char     device_icon[12] = "ac";
    char     device_floor[33] = "";
};

bool ensureFSMounted(bool formatIfNeeded);

class ConfigStore {
public:
    Config cfg;

    bool begin(bool formatIfNeeded = false);
    bool load();
    bool save();
    void scheduleSave();
    bool saveIfDue();

    bool isMounted() const { return mounted_; }

    void factoryResetWipe();

private:
    bool     mounted_      = false;
    bool     savePending_  = false;
    uint32_t saveDueAt_    = 0;

    bool writeAtomic(const char* path, const char* bakPath);
};

extern ConfigStore configStore;
