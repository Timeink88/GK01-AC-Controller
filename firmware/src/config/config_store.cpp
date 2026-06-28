#include "config/config_store.h"
#include "core/string_utils.h"
#include "core/constants.h"
#include <LittleFS.h>

ConfigStore configStore;

using namespace str;

namespace {
bool fsMounted = false;
}

bool ensureFSMounted(bool formatIfNeeded) {
    if (fsMounted) return true;

    fsMounted = LittleFS.begin();
    if (fsMounted) return true;
    if (!formatIfNeeded) return false;

    Serial.println(F("[FS] mount failed, formatting on demand..."));
    LittleFS.format();
    fsMounted = LittleFS.begin();
    if (!fsMounted) {
        Serial.println(F("[FS] mount still failed after format"));
    }
    return fsMounted;
}

bool ConfigStore::begin(bool formatIfNeeded) {
    mounted_ = ensureFSMounted(formatIfNeeded);
    Serial.println(F("[MODULE] config_store real impl loaded"));
    return mounted_;
}

bool ConfigStore::writeAtomic(const char* path, const char* bakPath) {
    String tmp = String(path) + ".tmp";
    LittleFS.remove(bakPath);
    bool hadOld = LittleFS.exists(path);
    if (hadOld && !LittleFS.rename(path, bakPath)) {
        Serial.println(F("[CFG] backup rename failed"));
        LittleFS.remove(tmp.c_str());
        return false;
    }

    if (!LittleFS.rename(tmp.c_str(), path)) {
        Serial.println(F("[CFG] commit rename failed"));
        if (hadOld) LittleFS.rename(bakPath, path);
        return false;
    }

    if (hadOld) LittleFS.remove(bakPath);
    return true;
}

bool ConfigStore::load() {
    if (!ensureFSMounted(false)) {
        Serial.println(F("[CFG] LittleFS unavailable, using defaults"));
        return false;
    }

    File f = LittleFS.open(FS_CONFIG_TXT, "r");
    if (!f) {
        f = LittleFS.open(FS_CONFIG_BAK, "r");
        if (f) Serial.println(F("[CFG] config.txt missing, loading backup"));
    }
    if (!f) return false;

    String line;
    while (f.available()) {
        line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);

        if      (key == "ap_ssid")             copyTo(cfg.ap_ssid, sizeof(cfg.ap_ssid), sanitizeConfigValue(val, sizeof(cfg.ap_ssid) - 1));
        else if (key == "ap_pass")             copyTo(cfg.ap_pass, sizeof(cfg.ap_pass), sanitizeConfigValue(val, sizeof(cfg.ap_pass) - 1));
        else if (key == "ssid")                copyTo(cfg.sta_ssid, sizeof(cfg.sta_ssid), sanitizeConfigValue(val, sizeof(cfg.sta_ssid) - 1));
        else if (key == "pass")                copyTo(cfg.sta_pass, sizeof(cfg.sta_pass), sanitizeConfigValue(val, sizeof(cfg.sta_pass) - 1));
        else if (key == "mqtt_host")           copyTo(cfg.mqtt_host, sizeof(cfg.mqtt_host), sanitizeConfigValue(val, sizeof(cfg.mqtt_host) - 1));
        else if (key == "mqtt_port")           cfg.mqtt_port = (uint16_t)val.toInt();
        else if (key == "mqtt_user")           copyTo(cfg.mqtt_user, sizeof(cfg.mqtt_user), sanitizeConfigValue(val, sizeof(cfg.mqtt_user) - 1));
        else if (key == "mqtt_pass")           copyTo(cfg.mqtt_pass, sizeof(cfg.mqtt_pass), sanitizeConfigValue(val, sizeof(cfg.mqtt_pass) - 1));
        else if (key == "mqtt_topic")          copyTo(cfg.mqtt_topic, sizeof(cfg.mqtt_topic), sanitizeConfigValue(val, sizeof(cfg.mqtt_topic) - 1));
        else if (key == "force_mode")          cfg.force_mode = (uint8_t)val.toInt();
        else if (key == "paired_master_bssid") copyTo(cfg.paired_master_bssid, sizeof(cfg.paired_master_bssid), sanitizeConfigValue(val, sizeof(cfg.paired_master_bssid) - 1));
        else if (key == "last_vendor")         copyTo(cfg.last_vendor, sizeof(cfg.last_vendor), sanitizeConfigValue(val, sizeof(cfg.last_vendor) - 1));
        else if (key == "last_mode")           copyTo(cfg.last_mode, sizeof(cfg.last_mode), sanitizeConfigValue(val, sizeof(cfg.last_mode) - 1));
        else if (key == "last_temp")           cfg.last_temp = (uint8_t)val.toInt();
        else if (key == "last_fan")            copyTo(cfg.last_fan, sizeof(cfg.last_fan), sanitizeConfigValue(val, sizeof(cfg.last_fan) - 1));
        else if (key == "last_power")          cfg.last_power = (val.toInt() != 0);
        else if (key == "last_turbo")          cfg.last_turbo = (val.toInt() != 0);
        else if (key == "last_econo")          cfg.last_econo = (val.toInt() != 0);
        else if (key == "last_sleep")          cfg.last_sleep = (val.toInt() != 0);
        else if (key == "last_light")          cfg.last_light = (val.toInt() != 0);
        else if (key == "last_clean")          cfg.last_clean = (val.toInt() != 0);
        else if (key == "device_name")         copyTo(cfg.device_name, sizeof(cfg.device_name), sanitizeMetadata(val, sizeof(cfg.device_name) - 1));
        else if (key == "device_icon")         copyTo(cfg.device_icon, sizeof(cfg.device_icon), sanitizeIconKey(val));
        else if (key == "device_floor")        copyTo(cfg.device_floor, sizeof(cfg.device_floor), sanitizeMetadata(val, sizeof(cfg.device_floor) - 1));
    }
    f.close();

    if (cfg.force_mode > FORCE_MODE_HOME) cfg.force_mode = FORCE_MODE_AUTO;
    if (cfg.mqtt_port == 0) cfg.mqtt_port = MQTT_DEFAULT_PORT;
    cfg.last_temp = constrain(cfg.last_temp, HVAC_MIN_TEMP_DEFAULT, HVAC_MAX_TEMP_DEFAULT);
    return strlen(cfg.sta_ssid) > 0;
}

bool ConfigStore::save() {
    if (!ensureFSMounted(true)) {
        Serial.println(F("[CFG] skipped save: LittleFS unavailable"));
        return false;
    }

    File f = LittleFS.open("/config.tmp", "w");
    if (!f) {
        Serial.println(F("[CFG] write failed"));
        return false;
    }
    f.printf("ap_ssid=%s\n",             cfg.ap_ssid);
    f.printf("ap_pass=%s\n",             cfg.ap_pass);
    f.printf("ssid=%s\n",                cfg.sta_ssid);
    f.printf("pass=%s\n",                cfg.sta_pass);
    f.printf("mqtt_host=%s\n",           cfg.mqtt_host);
    f.printf("mqtt_port=%d\n",           cfg.mqtt_port);
    f.printf("mqtt_user=%s\n",           cfg.mqtt_user);
    f.printf("mqtt_pass=%s\n",           cfg.mqtt_pass);
    f.printf("mqtt_topic=%s\n",          cfg.mqtt_topic);
    f.printf("force_mode=%d\n",          cfg.force_mode);
    f.printf("paired_master_bssid=%s\n", cfg.paired_master_bssid);
    f.printf("last_vendor=%s\n",         cfg.last_vendor);
    f.printf("last_mode=%s\n",           cfg.last_mode);
    f.printf("last_temp=%d\n",           cfg.last_temp);
    f.printf("last_fan=%s\n",            cfg.last_fan);
    f.printf("last_power=%d\n",          cfg.last_power ? 1 : 0);
    f.printf("last_turbo=%d\n",          cfg.last_turbo ? 1 : 0);
    f.printf("last_econo=%d\n",          cfg.last_econo ? 1 : 0);
    f.printf("last_sleep=%d\n",          cfg.last_sleep ? 1 : 0);
    f.printf("last_light=%d\n",          cfg.last_light ? 1 : 0);
    f.printf("last_clean=%d\n",          cfg.last_clean ? 1 : 0);
    f.printf("device_name=%s\n",         cfg.device_name);
    f.printf("device_icon=%s\n",         cfg.device_icon);
    f.printf("device_floor=%s\n",        cfg.device_floor);
    f.close();

    if (!writeAtomic(FS_CONFIG_TXT, FS_CONFIG_BAK)) return false;
    savePending_ = false;
    saveDueAt_   = 0;
    Serial.println(F("[CFG] saved"));
    return true;
}

void ConfigStore::scheduleSave() {
    savePending_ = true;
    saveDueAt_   = millis() + CONFIG_SAVE_DELAY_MS;
}

bool ConfigStore::saveIfDue() {
    if (!savePending_) return false;
    if ((long)(millis() - saveDueAt_) < 0) return false;
    return save();
}

void ConfigStore::factoryResetWipe() {
    if (!ensureFSMounted(true)) return;
    LittleFS.remove(FS_CONFIG_TXT);
    LittleFS.remove(FS_CONFIG_BAK);
    LittleFS.remove(FS_OTA_STATE);
    LittleFS.remove(FS_SLAVES_TXT);
    LittleFS.remove(FS_SLAVES_BAK);
}
