#include "net/mqtt_service.h"
#include "config/config_store.h"
#include "app/app_context.h"
#include "hw/sensor_service.h"
#include "hw/hvac/hvac_registry.h"
#include "core/string_utils.h"

MqttService mqtt;

namespace {
MqttService* self = nullptr;

void onMessage(char* topic, byte* payload, unsigned int length) {
    if (!self) return;
    char pbuf[256];
    unsigned int copyLen = (length < sizeof(pbuf) - 1) ? length : sizeof(pbuf) - 1;
    memcpy(pbuf, payload, copyLen);
    pbuf[copyLen] = '\0';
    String t = String(topic);
    String p = String(pbuf);
    Serial.printf("[MQTT] %s -> %s\n", t.c_str(), p.c_str());

    const String& base = self->topicBase();
    String vendor = ctx.hvacState.vendor.length() > 0 ? ctx.hvacState.vendor
                   : (strlen(configStore.cfg.last_vendor) > 0 ? String(configStore.cfg.last_vendor) : String("GREE"));

    hvac::Command c;
    c.vendor = vendor;
    c.mode   = ctx.hvacState.mode;
    c.temp   = ctx.hvacState.temp;
    c.fan    = ctx.hvacState.fan;
    c.swing  = "Off";

    bool ok = false;
    bool sendCmd = true;

    if (t == base + "/mode/set") {
        if      (p == "off")      { c.power = false; ok = hvacRegistry.send(c); }
        else if (p == "cool")     { c.power = true; c.mode = "Cool"; ok = hvacRegistry.send(c); }
        else if (p == "heat")     { c.power = true; c.mode = "Heat"; ok = hvacRegistry.send(c); }
        else if (p == "fan_only") { c.power = true; c.mode = "Fan";  ok = hvacRegistry.send(c); }
        else if (p == "dry")      { c.power = true; c.mode = "Dry";  ok = hvacRegistry.send(c); }
        else if (p == "auto")     { c.power = true; c.mode = "Auto"; ok = hvacRegistry.send(c); }
        else sendCmd = false;
    } else if (t == base + "/temperature/set") {
        c.power = ctx.hvacState.power;
        c.temp  = constrain(p.toInt(), HVAC_MIN_TEMP_DEFAULT, HVAC_MAX_TEMP_DEFAULT);
        ok = hvacRegistry.send(c);
    } else if (t == base + "/fan/set") {
        c.power = ctx.hvacState.power;
        if (p.length() > 0) { p[0] = toupper(p[0]); c.fan = p; }
        ok = hvacRegistry.send(c);
    } else {
        sendCmd = false;
    }

    if (sendCmd && ok) {
        ctx.hvacState.applyFrom(c);
        ctx.hvacState.persistTo(configStore.cfg);
        configStore.scheduleSave();
        self->publishState();
    }
}
}

void MqttService::begin() {
    self = this;
    mqtt_.setClient(net_);
    mqtt_.setBufferSize(MQTT_MAX_PACKET_SIZE);
    mqtt_.setCallback(onMessage);
    enabled_ = strlen(configStore.cfg.mqtt_host) > 0;
    topicBase_ = String(configStore.cfg.mqtt_topic);
    Serial.println(F("[MODULE] mqtt_service real impl loaded"));
}

bool MqttService::connect() {
    if (strlen(configStore.cfg.mqtt_host) == 0) return false;

    mqtt_.setServer(configStore.cfg.mqtt_host, configStore.cfg.mqtt_port);

    String clientId = String("IR-AC-") + String(ESP.getChipId(), HEX);
    bool ok;
    if (strlen(configStore.cfg.mqtt_user) > 0) {
        ok = mqtt_.connect(clientId.c_str(),
                           configStore.cfg.mqtt_user,
                           configStore.cfg.mqtt_pass);
    } else {
        ok = mqtt_.connect(clientId.c_str());
    }

    if (ok) {
        Serial.println(F("[MQTT] Connected"));
        mqtt_.subscribe((topicBase_ + "/mode/set").c_str());
        mqtt_.subscribe((topicBase_ + "/temperature/set").c_str());
        mqtt_.subscribe((topicBase_ + "/fan/set").c_str());
        publishDiscovery();
        publishState();
        return true;
    }
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt_.state());
    return false;
}

void MqttService::disconnect() { mqtt_.disconnect(); }

void MqttService::loop() {
    if (!enabled_) return;
    if (!mqtt_.connected()) {
        uint32_t now = millis();
        if ((uint32_t)(now - lastReconnect_) >= MQTT_RECONNECT_INTERVAL_MS) {
            lastReconnect_ = now;
            connect();
        }
    } else {
        mqtt_.loop();
    }
}

void MqttService::publishState() {
    if (!mqtt_.connected()) return;
    mqtt_.publish((topicBase_ + "/state").c_str(),
                  ctx.hvacState.power ? "on" : "off", true);

    String modeLower = ctx.hvacState.mode; modeLower.toLowerCase();
    mqtt_.publish((topicBase_ + "/mode_state").c_str(), modeLower.c_str(), true);

    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", ctx.hvacState.temp);
    mqtt_.publish((topicBase_ + "/temperature_state").c_str(), tmp, true);

    String fanLower = ctx.hvacState.fan; fanLower.toLowerCase();
    mqtt_.publish((topicBase_ + "/fan_state").c_str(), fanLower.c_str(), true);

    if (sensors.present() && sensors.temperatureC() > SENSOR_TEMP_INVALID) {
        snprintf(tmp, sizeof(tmp), "%.1f", sensors.temperatureC());
        mqtt_.publish((topicBase_ + "/current_temperature").c_str(), tmp, true);
    }

    mqtt_.publish((topicBase_ + "/motion").c_str(),
                  sensors.motionDetected() ? "ON" : "OFF", true);

    const char* action = "off";
    if (ctx.hvacState.power) {
        if      (ctx.hvacState.mode == "Cool") action = "cooling";
        else if (ctx.hvacState.mode == "Heat") action = "heating";
        else if (ctx.hvacState.mode == "Dry")  action = "drying";
        else if (ctx.hvacState.mode == "Fan")  action = "fan";
        else                                    action = "idle";
    }
    mqtt_.publish((topicBase_ + "/action").c_str(), action, true);

    mqtt_.publish((topicBase_ + "/turbo_state").c_str(),
                  ctx.hvacState.turbo ? "ON" : "OFF", true);
    mqtt_.publish((topicBase_ + "/econo_state").c_str(),
                  ctx.hvacState.econo ? "ON" : "OFF", true);
    mqtt_.publish((topicBase_ + "/sleep_state").c_str(),
                  ctx.hvacState.sleep ? "ON" : "OFF", true);
    mqtt_.publish((topicBase_ + "/light_state").c_str(),
                  ctx.hvacState.light ? "ON" : "OFF", true);
    mqtt_.publish((topicBase_ + "/clean_state").c_str(),
                  ctx.hvacState.clean ? "ON" : "OFF", true);
}

void MqttService::publishDiscovery() {
    if (!mqtt_.connected()) return;

    String chipId = String(ESP.getChipId(), HEX);
    String devName = strlen(configStore.cfg.device_name) > 0
                     ? str::jsonEscape(String(configStore.cfg.device_name))
                     : String("IR AC");
    String motionName = strlen(configStore.cfg.device_name) > 0
                        ? str::jsonEscape(String(configStore.cfg.device_name)) + " Motion"
                        : String("IR AC Motion");

    char disc[MQTT_DISCOVERY_BUF_SIZE];
    snprintf(disc, sizeof(disc),
        "{\"name\":\"%s\",\"unique_id\":\"ir-ac-%s\",\"icon\":\"mdi:air-conditioner\","
        "\"availability_topic\":\"%s/availability\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"mode_command_topic\":\"%s/mode/set\",\"mode_state_topic\":\"%s/mode_state\","
        "\"action_topic\":\"%s/action\","
        "\"modes\":[\"off\",\"cool\",\"heat\",\"fan_only\",\"dry\",\"auto\"],"
        "\"temperature_command_topic\":\"%s/temperature/set\","
        "\"temperature_state_topic\":\"%s/temperature_state\","
        "\"min_temp\":16,\"max_temp\":30,\"temp_step\":1,"
        "\"fan_mode_command_topic\":\"%s/fan/set\","
        "\"fan_mode_state_topic\":\"%s/fan_state\","
        "\"fan_modes\":[\"auto\",\"low\",\"medium\",\"high\"],"
        "\"current_temperature_topic\":\"%s/current_temperature\","
        "\"precision\":1.0,"
        "\"device\":{\"identifiers\":[\"ir-ac-%s\"],\"name\":\"%s\",\"manufacturer\":\"DIY\",\"model\":\"IR Mini V105\",\"sw_version\":\"2.0\"}}",
        devName.c_str(), chipId.c_str(),
        topicBase_.c_str(), topicBase_.c_str(), topicBase_.c_str(),
        topicBase_.c_str(), topicBase_.c_str(), topicBase_.c_str(),
        topicBase_.c_str(), topicBase_.c_str(), topicBase_.c_str(),
        chipId.c_str(), devName.c_str());

    String climateTopic = "homeassistant/climate/ir-ac-" + chipId + "/config";
    mqtt_.publish(climateTopic.c_str(), disc, true);

    char motionDisc[384];
    snprintf(motionDisc, sizeof(motionDisc),
        "{\"name\":\"%s\",\"unique_id\":\"ir-ac-motion-%s\","
        "\"state_topic\":\"%s/motion\",\"device_class\":\"motion\","
        "\"availability_topic\":\"%s/availability\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"ir-ac-%s\"]}}",
        motionName.c_str(), chipId.c_str(),
        topicBase_.c_str(), topicBase_.c_str(), chipId.c_str());

    String motionTopic = "homeassistant/binary_sensor/ir-ac-" + chipId + "/config";
    mqtt_.publish(motionTopic.c_str(), motionDisc, true);

    mqtt_.publish((topicBase_ + "/availability").c_str(), "online", true);
}

void MqttService::publishAvailability(bool online) {
    if (!mqtt_.connected()) return;
    mqtt_.publish((topicBase_ + "/availability").c_str(), online ? "online" : "offline", true);
}

void MqttService::publishMotion(bool detected) {
    if (!mqtt_.connected()) return;
    mqtt_.publish((topicBase_ + "/motion").c_str(), detected ? "ON" : "OFF", true);
}

void MqttService::onHvacCommandReceived() {
    publishState();
}

void MqttService::setCallback(MqttCallback cb) {
    mqtt_.setCallback(cb);
}
