#include "web/api/web_api.h"
#include "web/web_server.h"
#include "app/app_context.h"
#include "config/config_store.h"
#include "core/string_utils.h"
#include "core/constants.h"
#include "core/json_builder.h"
#include "hw/ir_service.h"
#include "hw/hvac/hvac_registry.h"
#include "hw/sensor_service.h"
#include "hw/led_manager.h"
#include "net/mqtt_service.h"
#include "net/udp_mesh.h"
#include <ESP8266WebServer.h>

namespace api {

static ESP8266WebServer& S() { return web.raw(); }

static void handleHvac() {
    String vendor = S().arg("vendor");
    if (vendor.length() == 0) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"missing_vendor\"}");
        return;
    }
    if (!hvacRegistry.isKnownVendor(vendor)) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_vendor\"}");
        return;
    }
    int temp = S().arg("temp").toInt();
    if (temp < hvacRegistry.minTempFor(vendor) || temp > hvacRegistry.maxTempFor(vendor)) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"temp_out_of_range\"}");
        return;
    }
    hvac::Command c;
    c.vendor = vendor;
    c.power  = S().arg("power") == "On";
    c.mode   = S().arg("mode");
    c.temp   = temp;
    c.fan    = S().arg("fan");
    c.swing  = S().arg("swing");
    c.turbo  = S().arg("turbo") == "1";
    c.econo  = S().arg("econo") == "1";
    c.sleep  = S().arg("sleep") == "1";
    c.light  = S().arg("light") != "0";
    c.clean  = S().arg("clean") == "1";

    bool ok = hvacRegistry.send(c);
    if (ok) {
        ctx.hvacState.applyFrom(c);
        ctx.hvacState.persistTo(configStore.cfg);
        configStore.scheduleSave();

        if (ctx.isApMaster()) {
            String target = S().arg("target");
            if (target.length() == 0) target = "ALL";
            char msg[400];
            snprintf(msg, sizeof(msg), "HVAC:%s:%s,%s,%s,%d,%s,%s",
                     target.c_str(), vendor.c_str(),
                     c.power ? "1" : "0", c.mode.c_str(), temp,
                     c.fan.c_str(), c.swing.c_str());
            udp.broadcast(msg);
            snprintf(msg, sizeof(msg), "HVAC2:%s:%s,%s,%s,%d,%s,%s,%d,%d,%d,%d,%d",
                     target.c_str(), vendor.c_str(),
                     c.power ? "1" : "0", c.mode.c_str(), temp,
                     c.fan.c_str(), c.swing.c_str(),
                     c.turbo ? 1 : 0, c.econo ? 1 : 0, c.sleep ? 1 : 0,
                     c.light ? 1 : 0, c.clean ? 1 : 0);
            udp.broadcast(msg);
        }
        if (mqtt.connected()) mqtt.publishState();
        S().send(200, "application/json", "{\"ok\":true}");
    } else {
        S().send(500, "application/json", "{\"ok\":false,\"error\":\"send_failed\"}");
    }
}

static void handleHvacState() {
    char buf[320];
    JsonBuilder jb(buf, sizeof(buf));
    jb.beginObject();
    jb.kvString("vendor", configStore.cfg.last_vendor);
    jb.kvString("mode",   configStore.cfg.last_mode);
    jb.kvInt("temp",      configStore.cfg.last_temp);
    jb.kvString("fan",    configStore.cfg.last_fan);
    jb.kvBool("power",    configStore.cfg.last_power);
    jb.kvBool("turbo",    configStore.cfg.last_turbo);
    jb.kvBool("econo",    configStore.cfg.last_econo);
    jb.kvBool("sleep",    configStore.cfg.last_sleep);
    jb.kvBool("light",    configStore.cfg.last_light);
    jb.kvBool("clean",    configStore.cfg.last_clean);
    jb.endObject();
    S().send(200, "application/json", jb.result());
}

static void handleSend() {
    String raw = S().arg("raw");
    if (raw.length() == 0) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"empty\"}");
        return;
    }
    static uint16_t buf[512];
    int len = str::parseRawTimings(raw, buf, 512);
    if (len == 0) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"parse_error\"}");
        return;
    }
    ir.sendRaw(buf, len);
    leds.flashRed(30);

    if (ctx.isApMaster()) {
        String msg = "RAW:" + String(len) + ":" + raw;
        if (msg.length() < 1024) udp.broadcast(msg.c_str());
    }
    S().send(200, "application/json", "{\"ok\":true}");
}

static void handleCapture() {
    IrService::Capture cap;
    if (!ir.consumeCapture(cap)) {
        S().send(200, "application/json", "{\"raw\":\"\"}");
        return;
    }
    size_t len = cap.rawLen + sizeof(cap.proto) + 64;
    char* json = (char*)malloc(len);
    if (!json) {
        S().send(500, "application/json", "{\"error\":\"oom\"}");
        return;
    }
    snprintf(json, len, "{\"raw\":\"%s\",\"proto\":\"%s\",\"bits\":%d}",
             cap.raw, cap.proto, cap.bits);
    S().send(200, "application/json", json);
    free(json);
}

static void handleWifiScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    json.reserve(768);
    for (int i = 0; i < n && i < 20; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + str::jsonEscape(WiFi.SSID(i)) +
                "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                ",\"enc\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? 1 : 0) + "}";
    }
    WiFi.scanDelete();
    json += "]";
    S().send(200, "application/json", json);
}

static void handleWifiConnect() {
    String ssid = str::sanitizeConfigValue(S().arg("ssid"), sizeof(configStore.cfg.sta_ssid) - 1);
    String pass = str::sanitizeConfigValue(S().arg("pass"), sizeof(configStore.cfg.sta_pass) - 1);
    if (ssid.length() == 0) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"empty_ssid\"}");
        return;
    }
    str::copyTo(configStore.cfg.sta_ssid, sizeof(configStore.cfg.sta_ssid), ssid);
    str::copyTo(configStore.cfg.sta_pass, sizeof(configStore.cfg.sta_pass), pass);
    configStore.save();
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handleWifiStatus() {
    String json = "{";
    json.reserve(192);
    json += "\"mode\":\"" + String(ctx.isStaHome() ? "sta" : (ctx.isStaSlave() ? "slave" : "ap")) + "\",";
    if (ctx.isStaHome()) {
        json += "\"ssid\":\"" + str::jsonEscape(String(configStore.cfg.sta_ssid)) + "\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    } else if (ctx.isApMaster()) {
        json += "\"ssid\":\"" + str::jsonEscape(String(getApSsid())) + "\",";
        json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
        json += "\"rssi\":0,";
    } else {
        json += "\"ssid\":\"" + str::jsonEscape(String(getApSsid())) + "\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    }
    json += "\"mqtt\":" + String(mqtt.connected() ? "true" : "false") + ",";
    json += "\"mqtt_host\":\"" + str::jsonEscape(String(configStore.cfg.mqtt_host)) + "\"";
    json += "}";
    S().send(200, "application/json", json);
}

static void handleWifiForget() {
    configStore.cfg.sta_ssid[0] = '\0';
    configStore.cfg.sta_pass[0] = '\0';
    configStore.save();
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handleApConfig() {
    if (S().method() == HTTP_GET) {
        String json = "{\"ssid\":\"" + str::jsonEscape(String(configStore.cfg.ap_ssid)) +
                      "\",\"pass\":\"" + str::jsonEscape(String(configStore.cfg.ap_pass)) + "\"}";
        S().send(200, "application/json", json);
        return;
    }
    String ssid = str::sanitizeConfigValue(S().arg("ssid"), sizeof(configStore.cfg.ap_ssid) - 1);
    String pass = str::sanitizeConfigValue(S().arg("pass"), sizeof(configStore.cfg.ap_pass) - 1);
    if (ssid.length() == 0 || ssid.length() > 32) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_ssid\"}");
        return;
    }
    if (pass.length() > 0 && pass.length() < 8) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"password_min_8\"}");
        return;
    }
    if (pass.length() > 32) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"password_max_32\"}");
        return;
    }
    str::copyTo(configStore.cfg.ap_ssid, sizeof(configStore.cfg.ap_ssid), ssid);
    str::copyTo(configStore.cfg.ap_pass, sizeof(configStore.cfg.ap_pass), pass);
    configStore.save();
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handleMqttConfig() {
    if (S().method() == HTTP_GET) {
        String json = "{\"host\":\"" + str::jsonEscape(String(configStore.cfg.mqtt_host)) +
                      "\",\"port\":" + String(configStore.cfg.mqtt_port) +
                      ",\"user\":\"" + str::jsonEscape(String(configStore.cfg.mqtt_user)) +
                      "\",\"topic\":\"" + str::jsonEscape(String(configStore.cfg.mqtt_topic)) + "\"}";
        S().send(200, "application/json", json);
        return;
    }
    String host = str::sanitizeConfigValue(S().arg("host"), sizeof(configStore.cfg.mqtt_host) - 1);
    str::copyTo(configStore.cfg.mqtt_host, sizeof(configStore.cfg.mqtt_host), host);
    String port = str::sanitizeConfigValue(S().arg("port"), 5);
    if (port.length() > 0) {
        long newPort = port.toInt();
        if (newPort < 1 || newPort > 65535) {
            S().send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_port\"}");
            return;
        }
        configStore.cfg.mqtt_port = (uint16_t)newPort;
    }
    String user = str::sanitizeConfigValue(S().arg("user"), sizeof(configStore.cfg.mqtt_user) - 1);
    str::copyTo(configStore.cfg.mqtt_user, sizeof(configStore.cfg.mqtt_user), user);
    if (S().hasArg("pass")) {
        String pass = str::sanitizeConfigValue(S().arg("pass"), sizeof(configStore.cfg.mqtt_pass) - 1);
        str::copyTo(configStore.cfg.mqtt_pass, sizeof(configStore.cfg.mqtt_pass), pass);
    }
    String topic = str::sanitizeConfigValue(S().arg("topic"), sizeof(configStore.cfg.mqtt_topic) - 1);
    if (topic.length() > 0) str::copyTo(configStore.cfg.mqtt_topic, sizeof(configStore.cfg.mqtt_topic), topic);
    configStore.save();
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handleDeviceConfig() {
    if (S().method() == HTTP_GET) {
        String json = "{\"name\":\"" + str::jsonEscape(String(configStore.cfg.device_name)) +
                      "\",\"icon\":\"" + str::jsonEscape(String(configStore.cfg.device_icon)) +
                      "\",\"floor\":\"" + str::jsonEscape(String(configStore.cfg.device_floor)) + "\"}";
        S().send(200, "application/json", json);
        return;
    }
    if (S().hasArg("name"))  str::copyTo(configStore.cfg.device_name,  sizeof(configStore.cfg.device_name),  str::sanitizeMetadata(S().arg("name"),  sizeof(configStore.cfg.device_name) - 1));
    if (S().hasArg("icon"))  str::copyTo(configStore.cfg.device_icon,  sizeof(configStore.cfg.device_icon),  str::sanitizeIconKey(S().arg("icon")));
    if (S().hasArg("floor")) str::copyTo(configStore.cfg.device_floor, sizeof(configStore.cfg.device_floor), str::sanitizeMetadata(S().arg("floor"), sizeof(configStore.cfg.device_floor) - 1));
    configStore.save();
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handleSystemInfo() {
    String json = "{";
    json.reserve(256);
    json += "\"mac\":\"" + str::jsonEscape(WiFi.macAddress()) + "\",";
    json += "\"apMac\":\"" + str::jsonEscape(WiFi.softAPmacAddress()) + "\",";
    json += "\"chipId\":\"" + str::jsonEscape(String(ESP.getChipId(), HEX)) + "\",";
    json += "\"flashSize\":" + String(ESP.getFlashChipSize()) + ",";
    json += "\"mode\":\"" + str::jsonEscape(String(ctx.modeName())) + "\",";
    json += "\"forceMode\":" + String(configStore.cfg.force_mode) + ",";
    json += "\"uptime\":" + String(millis()) + ",";
    json += "\"deviceName\":\"" + str::jsonEscape(String(configStore.cfg.device_name)) + "\",";
    json += "\"deviceIcon\":\"" + str::jsonEscape(String(configStore.cfg.device_icon)) + "\"";
    json += "}";
    S().send(200, "application/json", json);
}

static void handleForceMode() {
    String mode = S().arg("mode");
    uint8_t newMode = FORCE_MODE_AUTO;
    if      (mode == "ap")    newMode = FORCE_MODE_AP;
    else if (mode == "slave") newMode = FORCE_MODE_SLAVE;
    else if (mode == "home")  newMode = FORCE_MODE_HOME;
    else if (mode == "auto")  newMode = FORCE_MODE_AUTO;
    else {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_mode\"}");
        return;
    }
    if (newMode == FORCE_MODE_HOME && strlen(configStore.cfg.sta_ssid) == 0) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"missing_sta_config\"}");
        return;
    }
    configStore.cfg.force_mode = newMode;
    configStore.save();
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handleSlaves() {
    if (!ctx.isApMaster()) {
        S().send(200, "application/json", "{\"slaves\":[]}");
        return;
    }
    ctx.slaves.refreshFromApStations();
    uint32_t now = millis();
    String json = "{\"slaves\":[";
    json.reserve(512);
    bool first = true;
    for (size_t i = 0; i < ctx.slaves.size(); i++) {
        const SlaveInfo& s = ctx.slaves.at(i);
        if (s.mac[0] != '\0' && s.lastSeen != 0 &&
            (uint32_t)(now - s.lastSeen) < SLAVE_OFFLINE_TIMEOUT_MS) {
            if (!first) json += ",";
            json += "{\"mac\":\"" + String(s.mac) + "\",";
            json += "\"id\":\"" + str::slaveIdFromMac(s.mac) + "\",";
            json += "\"ip\":\"" + IPAddress(s.ip).toString() + "\",";
            json += "\"name\":\"" + str::jsonEscape(String(s.name)) + "\",";
            json += "\"icon\":\"" + str::jsonEscape(String(s.icon)) + "\",";
            json += "\"floor\":\"" + str::jsonEscape(String(s.floor)) + "\",";
            json += "\"ago\":" + String(now - s.lastSeen);
            json += "}";
            first = false;
        }
    }
    json += "],\"pairing\":" + String(ctx.pairing.active() ? 1 : 0);
    if (ctx.pairing.active()) {
        json += ",\"pairingLeft\":" + String(ctx.pairing.remainingMs() / 1000);
    }
    json += "}";
    S().send(200, "application/json", json);
}

static void handleSlaveConfig() {
    String targetId = S().arg("id");
    String cmd = S().arg("cmd");
    String val = S().arg("val");
    if (targetId.length() == 0) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"missing_id\"}");
        return;
    }
    int slot = ctx.slaves.findById(targetId);
    if (slot < 0 || ctx.slaves.at(slot).ip == 0) {
        S().send(404, "application/json", "{\"ok\":false,\"error\":\"slave_not_found\"}");
        return;
    }
    bool valueCmd = (cmd == "name" || cmd == "icon" || cmd == "floor" ||
                     cmd == "ap_ssid" || cmd == "ap_pass");
    bool actionCmd = (cmd == "reboot" || cmd == "disconnect");
    if (!valueCmd && !actionCmd) {
        S().send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_cmd\"}");
        return;
    }
    String safeVal = val;
    if      (cmd == "name")    safeVal = str::sanitizeMetadata(val, sizeof(ctx.slaves.at(0).name) - 1);
    else if (cmd == "icon")    safeVal = str::sanitizeIconKey(val);
    else if (cmd == "floor")   safeVal = str::sanitizeMetadata(val, sizeof(ctx.slaves.at(0).floor) - 1);
    else if (cmd == "ap_ssid") safeVal = str::sanitizeConfigValue(val, sizeof(configStore.cfg.ap_ssid) - 1);
    else if (cmd == "ap_pass") safeVal = str::sanitizeConfigValue(val, sizeof(configStore.cfg.ap_pass) - 1);

    String msg = "CONFIG:" + targetId + ":" + cmd;
    if (valueCmd) msg += "=" + safeVal;
    udp.sendTo(IPAddress(ctx.slaves.at(slot).ip), msg.c_str());

    bool shouldSave = false;
    if (cmd == "name")      { str::copyTo(ctx.slaves.at(slot).name,  sizeof(ctx.slaves.at(slot).name),  safeVal); shouldSave = true; }
    else if (cmd == "icon") { str::copyTo(ctx.slaves.at(slot).icon,  sizeof(ctx.slaves.at(slot).icon),  safeVal); shouldSave = true; }
    else if (cmd == "floor"){ str::copyTo(ctx.slaves.at(slot).floor, sizeof(ctx.slaves.at(slot).floor), safeVal); shouldSave = true; }
    else if (cmd == "disconnect") { ctx.slaves.clearSlot(slot); shouldSave = true; }
    if (shouldSave) ctx.slaves.save();

    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"sent\"}");
}

static void handlePairStart() {
    ctx.pairing.start();
    S().send(200, "application/json", "{\"ok\":true,\"duration\":60}");
}

static void handlePairStop() {
    ctx.pairing.stop();
    S().send(200, "application/json", "{\"ok\":true}");
}

static void handleSensorStatus() {
    String tempStr = (sensors.present() && sensors.temperatureC() > SENSOR_TEMP_INVALID)
                     ? String(sensors.temperatureC(), 1) : String("null");
    String json = "{\"temp\":" + tempStr +
                  ",\"motion\":" + String(sensors.motionDetected() ? "true" : "false") +
                  ",\"sensor_present\":" + String(sensors.present() ? "true" : "false") + "}";
    S().send(200, "application/json", json);
}

static void handleFactoryReset() {
    S().send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
    delay(500);
    configStore.factoryResetWipe();
    ESP.restart();
}

void registerHvacRoutes() {
    S().on("/api/hvac",       HTTP_POST, handleHvac);
    S().on("/api/hvac/state", HTTP_GET,  handleHvacState);
    S().on("/api/send",       HTTP_POST, handleSend);
    S().on("/api/capture",    HTTP_GET,  handleCapture);
}

void registerWifiRoutes() {
    S().on("/api/wifi/scan",    HTTP_GET,  handleWifiScan);
    S().on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
    S().on("/api/wifi/status",  HTTP_GET,  handleWifiStatus);
    S().on("/api/wifi/forget",  HTTP_POST, handleWifiForget);
}

void registerMqttRoutes() {
    S().on("/api/mqtt/config", HTTP_ANY, handleMqttConfig);
}

void registerApRoutes() {
    S().on("/api/ap/config", HTTP_ANY, handleApConfig);
}

void registerDeviceRoutes() {
    S().on("/api/device/config", HTTP_ANY, handleDeviceConfig);
    S().on("/api/system/info",   HTTP_GET, handleSystemInfo);
    S().on("/api/mode/force",    HTTP_POST, handleForceMode);
}

void registerSlavesRoutes() {
    S().on("/api/slaves",       HTTP_GET,  handleSlaves);
    S().on("/api/slave/config", HTTP_POST, handleSlaveConfig);
    S().on("/api/pair/start",   HTTP_POST, handlePairStart);
    S().on("/api/pair/stop",    HTTP_POST, handlePairStop);
}

void registerSensorRoutes() {
    S().on("/api/sensor", HTTP_GET, handleSensorStatus);
}

void registerFactoryRoutes() {
    S().on("/api/factory/reset", HTTP_POST, handleFactoryReset);
}

void registerAllRoutes() {
    registerHvacRoutes();
    registerWifiRoutes();
    registerMqttRoutes();
    registerApRoutes();
    registerDeviceRoutes();
    registerSlavesRoutes();
    registerSensorRoutes();
    registerFactoryRoutes();
}

}
