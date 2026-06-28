#include <Arduino.h>

#include "app/app_context.h"
#include "app/mode_factory.h"
#include "core/pins.h"
#include "core/constants.h"
#include "net/network_manager.h"
#include "net/udp_mesh.h"
#include "net/mqtt_service.h"
#include "config/config_store.h"
#include "hw/led_manager.h"
#include "hw/button_manager.h"
#include "hw/hvac/hvac_registry.h"
#include "hw/ir_service.h"
#include "ota/ota_manager.h"
#include "rboot.h"

static void onButtonShortPress() {
    bool power = !ctx.hvacState.power;
    hvac::Command c;
    c.vendor = OFFICE_PRESET_VENDOR;
    c.power  = power;
    c.mode   = OFFICE_PRESET_MODE;
    c.temp   = OFFICE_PRESET_TEMP;
    c.fan    = OFFICE_PRESET_FAN;
    c.swing  = OFFICE_PRESET_SWING;

    Serial.printf("[BTN] office preset: %s %s %dC\n",
                  c.vendor.c_str(), power ? "ON" : "OFF", c.temp);

    if (!hvacRegistry.send(c)) {
        Serial.println(F("[BTN] preset failed"));
        leds.blinkRestore(PIN_LED_RED, 3, 80, 80);
        return;
    }

    ctx.hvacState.applyFrom(c);
    ctx.hvacState.persistTo(configStore.cfg);
    configStore.scheduleSave();

    if (ctx.isApMaster()) {
        char msg[400];
        snprintf(msg, sizeof(msg), "HVAC:ALL:%s,%s,%s,%d,%s,%s",
                 c.vendor.c_str(), power ? "1" : "0", c.mode.c_str(), c.temp,
                 c.fan.c_str(), c.swing.c_str());
        udp.broadcast(msg);
        snprintf(msg, sizeof(msg), "HVAC2:ALL:%s,%s,%s,%d,%s,%s,%d,%d,%d,%d,%d",
                 c.vendor.c_str(), power ? "1" : "0", c.mode.c_str(), c.temp,
                 c.fan.c_str(), c.swing.c_str(),
                 c.turbo ? 1 : 0, c.econo ? 1 : 0, c.sleep ? 1 : 0,
                 c.light ? 1 : 0, c.clean ? 1 : 0);
        udp.broadcast(msg);
    }

    if (mqtt.connected()) mqtt.publishState();

    if (power) leds.blinkRestore(PIN_LED_BLUE, 2, 80, 80);
    else       leds.blinkRestore(PIN_LED_YELLOW, 2, 80, 80);
}

static void onButtonLongPress() {
    Serial.println(F("[BTN] long -> factory reset"));
    for (int i = 0; i < 10; i++) {
        leds.setYellow(true); delay(50);
        leds.setYellow(false); delay(50);
    }
    configStore.factoryResetWipe();
    delay(100);
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println(F("[BOOT] IR Mini V105 — architecture overhaul"));

    pinMode(PIN_LED_BLUE,   OUTPUT);
    pinMode(PIN_LED_RED,    OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    leds.setAll(false, false, false);
    leds.selfTest();
    leds.setAll(false, false, true);

    NetworkManager::setNoneSleep();

    ctx.begin();
    button.begin(onButtonShortPress, onButtonLongPress);

    if (!configStore.isMounted()) {
        Serial.println(F("[FS] LittleFS unavailable at boot; continuing with defaults"));
        leds.blinkRestore(PIN_LED_RED, 2, 80, 80);
    }

#if IRAC_RBOOT_OTA
    Serial.printf("[BOOT] ROM %d (rboot dual-partition)\n", rboot_get_current_rom());
#else
    Serial.println(F("[BOOT] factory firmware (rboot OTA disabled)"));
#endif

    DeviceModeId id = ctx.decideBootMode();
    Serial.printf("[BOOT] mode: %s\n", modeString(id));
    ctx.changeMode(id);
}

void loop() {
    ctx.loop();
}
