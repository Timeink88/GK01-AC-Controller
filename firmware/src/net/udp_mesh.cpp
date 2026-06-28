#include "net/udp_mesh.h"
#include "config/config_store.h"
#include "app/app_context.h"
#include "hw/ir_service.h"
#include "hw/hvac/hvac_registry.h"
#include "core/string_utils.h"

UdpMesh udp;

namespace {
uint32_t lastHelloSent = 0;

void slaveExecRaw(const String& data) {
    static uint16_t buf[512];
    int len = str::parseRawTimings(data, buf, 512);
    if (len == 0) return;
    ir.sendRaw(buf, len);
    leds.flashRed(30);
    Serial.printf("[SLAVE] RAW len=%d\n", len);
}

void slaveExecHvac(const String& data) {
    int c1 = data.indexOf(',');
    if (c1 < 0) return;
    String vendor = data.substring(0, c1);

    int c2 = data.indexOf(',', c1 + 1);
    bool power = (c2 > c1) ? data.substring(c1 + 1, c2) == "1" : false;
    if (c2 < 0) { hvac::Command c{vendor, power, "Cool", 26, "Auto", "Off"}; hvacRegistry.send(c); return; }

    int c3 = data.indexOf(',', c2 + 1);
    String mode = (c3 >= 0) ? data.substring(c2 + 1, c3) : data.substring(c2 + 1);
    if (mode.length() == 0) mode = "Cool";
    if (c3 < 0) { hvac::Command c{vendor, power, mode, 26, "Auto", "Off"}; hvacRegistry.send(c); return; }

    int c4 = data.indexOf(',', c3 + 1);
    String tempStr = (c4 >= 0) ? data.substring(c3 + 1, c4) : data.substring(c3 + 1);
    int temp = tempStr.toInt();
    if (temp == 0) temp = 26;
    if (c4 < 0) { hvac::Command c{vendor, power, mode, temp, "Auto", "Off"}; hvacRegistry.send(c); return; }

    int c5 = data.indexOf(',', c4 + 1);
    String fan = (c5 >= 0) ? data.substring(c4 + 1, c5) : data.substring(c4 + 1);
    if (fan.length() == 0) fan = "Auto";
    String swing = (c5 >= 0) ? data.substring(c5 + 1) : "Off";

    hvac::Command c{vendor, power, mode, temp, fan, swing};
    hvacRegistry.send(c);
    ctx.hvacState.applyFrom(c);
    ctx.hvacState.persistTo(configStore.cfg);
    configStore.scheduleSave();
    Serial.printf("[SLAVE] %s %s %dC\n", vendor.c_str(), power ? "ON" : "OFF", temp);
}

void slaveExecHvac2(const String& data) {
    int c[11] = {0};
    int pos = 0;
    for (int i = 0; i < 10; i++) {
        c[i] = data.indexOf(',', pos);
        if (c[i] < 0) { slaveExecHvac(data); return; }
        pos = c[i] + 1;
    }

    hvac::Command cmd;
    cmd.vendor = data.substring(0, c[0]);
    cmd.power  = data.substring(c[0]+1, c[1]) == "1";
    cmd.mode   = data.substring(c[1]+1, c[2]);
    cmd.temp   = data.substring(c[2]+1, c[3]).toInt();
    cmd.fan    = data.substring(c[3]+1, c[4]);
    cmd.swing  = data.substring(c[4]+1, c[5]);
    cmd.turbo  = data.substring(c[5]+1, c[6]) == "1";
    cmd.econo  = data.substring(c[6]+1, c[7]) == "1";
    cmd.sleep  = data.substring(c[7]+1, c[8]) == "1";
    cmd.light  = data.substring(c[8]+1, c[9]) != "0";
    cmd.clean  = data.substring(c[9]+1, c[10]) == "1";

    if (cmd.temp == 0) cmd.temp = 26;
    if (cmd.mode.length() == 0) cmd.mode = "Cool";
    if (cmd.fan.length() == 0) cmd.fan = "Auto";

    hvacRegistry.send(cmd);
    ctx.hvacState.applyFrom(cmd);
    ctx.hvacState.persistTo(configStore.cfg);
    configStore.scheduleSave();
    Serial.printf("[SLAVE2] %s %s %dC T:%d E:%d S:%d L:%d C:%d\n",
                  cmd.vendor.c_str(), cmd.power ? "ON" : "OFF", cmd.temp,
                  cmd.turbo, cmd.econo, cmd.sleep, cmd.light, cmd.clean);
}

void handleSlaveConfig(const String& cmd, const IPAddress& masterIp) {
    int colon = cmd.indexOf(':');
    if (colon <= 0) return;
    String target = cmd.substring(0, colon);
    String body   = cmd.substring(colon + 1);
    if (target != ctx.slaveId()) return;

    String ack = "CONFIGACK:" + ctx.slaveId() + ":";
    bool doReboot = false;
    bool doDisconnect = false;

    int eq = body.indexOf('=');
    if (eq > 0) {
        String key = body.substring(0, eq);
        String val = body.substring(eq + 1);
        bool ok = true;
        if      (key == "name")     str::copyTo(configStore.cfg.device_name, sizeof(configStore.cfg.device_name), str::sanitizeMetadata(val, sizeof(configStore.cfg.device_name) - 1));
        else if (key == "icon")     str::copyTo(configStore.cfg.device_icon, sizeof(configStore.cfg.device_icon), str::sanitizeIconKey(val));
        else if (key == "floor")    str::copyTo(configStore.cfg.device_floor, sizeof(configStore.cfg.device_floor), str::sanitizeMetadata(val, sizeof(configStore.cfg.device_floor) - 1));
        else if (key == "ap_ssid")  str::copyTo(configStore.cfg.ap_ssid, sizeof(configStore.cfg.ap_ssid), str::sanitizeConfigValue(val, sizeof(configStore.cfg.ap_ssid) - 1));
        else if (key == "ap_pass")  str::copyTo(configStore.cfg.ap_pass, sizeof(configStore.cfg.ap_pass), str::sanitizeConfigValue(val, sizeof(configStore.cfg.ap_pass) - 1));
        else { ack += "unknown_key"; ok = false; }
        if (ok) { configStore.save(); ack += "ok"; }
    } else if (body == "reboot") {
        ack += "ok"; doReboot = true;
    } else if (body == "disconnect") {
        ack += "ok"; doDisconnect = true;
    } else {
        ack += "unknown_cmd";
    }

    udp.sendTo(masterIp, ack.c_str());
    Serial.printf("[SLAVE] CONFIG %s → %s\n", body.c_str(), ack.substring(ack.lastIndexOf(':') + 1).c_str());

    if (doDisconnect) {
        delay(100);
        configStore.cfg.force_mode = FORCE_MODE_AP;
        configStore.cfg.paired_master_bssid[0] = '\0';
        configStore.save();
        WiFi.disconnect();
        delay(500);
        ESP.restart();
    }
    if (doReboot) {
        delay(100);
        ESP.restart();
    }
}

void handleMasterHello(const String& msg, const IPAddress& remoteIp) {
    const int MAC_LEN = 17;
    const int HDR_LEN = 6;
    if ((int)msg.length() < HDR_LEN + MAC_LEN) return;

    String mac = msg.substring(HDR_LEN, HDR_LEN + MAC_LEN);
    mac.trim();
    if (!str::isValidMacString(mac)) {
        Serial.printf("[MASTER] bad HELLO: %s\n", msg.substring(0, 80).c_str());
        return;
    }

    String name = "", icon = "ac", floor = "";
    int fieldStart = HDR_LEN + MAC_LEN;
    if (fieldStart < (int)msg.length() && msg.charAt(fieldStart) == ':') fieldStart++;
    String rest = msg.substring(fieldStart);
    int c1 = rest.indexOf(':');
    if (c1 >= 0) {
        name = rest.substring(0, c1);
        int c2 = rest.indexOf(':', c1 + 1);
        if (c2 >= 0) {
            icon  = rest.substring(c1 + 1, c2);
            floor = rest.substring(c2 + 1);
        } else {
            icon = rest.substring(c1 + 1);
        }
    } else {
        name = rest;
    }
    name  = str::sanitizeMetadata(name,  sizeof(ctx.slaves.at(0).name)  - 1);
    icon  = str::sanitizeIconKey(icon);
    floor = str::sanitizeMetadata(floor, sizeof(ctx.slaves.at(0).floor) - 1);

    int slot = ctx.slaves.findByMac(mac.c_str());
    bool alreadyKnown = (slot >= 0);
    if (!alreadyKnown && ctx.pairing.active()) {
        slot = ctx.slaves.findFree();
    }
    if (slot < 0) {
        Serial.printf("[MASTER] ignoring unpaired slave %s\n", mac.c_str());
        return;
    }

    SlaveInfo& s = ctx.slaves.at(slot);
    bool changed = !alreadyKnown;
    str::copyTo(s.mac, sizeof(s.mac), mac);
    s.ip       = remoteIp;
    s.lastSeen = millis();
    if (name.length()  > 0 && strcmp(s.name,  name.c_str())  != 0) { str::copyTo(s.name,  sizeof(s.name),  name);  changed = true; }
    if (icon.length()  > 0 && strcmp(s.icon,  icon.c_str())  != 0) { str::copyTo(s.icon,  sizeof(s.icon),  icon);  changed = true; }
    if (floor.length() > 0 && strcmp(s.floor, floor.c_str()) != 0) { str::copyTo(s.floor, sizeof(s.floor), floor); changed = true; }
    if (changed) ctx.slaves.save();

    udp.sendTo(remoteIp, "ACK:");
}
}

void UdpMesh::beginMaster() {
    udp_.begin(UDP_PORT);
    Serial.println(F("[MODULE] udp_mesh master real impl loaded"));
}

void UdpMesh::beginSlave() {
    udp_.begin(UDP_PORT);
    lastHelloSent = 0;
}

void UdpMesh::broadcast(const char* msg) {
    udp_.beginPacket(IPAddress(10, 1, 1, 255), UDP_PORT);
    udp_.write((const uint8_t*)msg, strlen(msg));
    udp_.endPacket();
}

void UdpMesh::sendTo(IPAddress ip, const char* msg) {
    udp_.beginPacket(ip, UDP_PORT);
    udp_.write((const uint8_t*)msg, strlen(msg));
    udp_.endPacket();
}

bool UdpMesh::receivePacket() {
    int packetSize = udp_.parsePacket();
    if (!packetSize) return false;
    int len = udp_.read(rcvBuf_, sizeof(rcvBuf_) - 1);
    if (len <= 0) return false;
    rcvBuf_[len]   = '\0';
    rcvLen_        = (size_t)len;
    remoteIp_      = udp_.remoteIP();
    return true;
}

void UdpMesh::loopMaster() {
    if (!receivePacket()) return;

    String msg = String(rcvBuf_);
    Serial.printf("[MASTER] recv: %s\n", msg.substring(0, 80).c_str());

    if (msg.startsWith("HELLO:")) {
        handleMasterHello(msg, remoteIp_);
    } else if (msg.startsWith("CONFIGACK:")) {
        Serial.printf("[MASTER] CONFIGACK: %s\n", msg.substring(10).c_str());
    }
}

void UdpMesh::loopSlave() {
    if ((uint32_t)(millis() - lastHelloSent) >= HELLO_INTERVAL_MS) {
        lastHelloSent = millis();
        String hello = "HELLO:" + WiFi.macAddress() + ":" +
                       String(configStore.cfg.device_name) + ":" +
                       String(configStore.cfg.device_icon) + ":" +
                       String(configStore.cfg.device_floor);
        sendTo(IPAddress(10, 1, 1, 1), hello.c_str());
    }

    if (!receivePacket()) goto slaveCheckWifi;
    {
        String msg = String(rcvBuf_);
        Serial.printf("[SLAVE] recv: %s\n", msg.substring(0, 80).c_str());

        if (msg.startsWith("RAW:")) {
            int firstColon = msg.indexOf(':', 4);
            if (firstColon > 0) slaveExecRaw(msg.substring(firstColon + 1));
        } else if (msg.startsWith("HVAC2:")) {
            String payload = msg.substring(6);
            int colon = payload.indexOf(':');
            if (colon > 0) {
                String target = payload.substring(0, colon);
                String data   = payload.substring(colon + 1);
                if (target == "ALL" ||
                    ("," + target + ",").indexOf("," + ctx.slaveId() + ",") >= 0) {
                    slaveExecHvac2(data);
                }
            }
        } else if (msg.startsWith("HVAC:")) {
            String payload = msg.substring(5);
            int colon = payload.indexOf(':');
            if (colon > 0) {
                String target = payload.substring(0, colon);
                String data   = payload.substring(colon + 1);
                if (target == "ALL" ||
                    ("," + target + ",").indexOf("," + ctx.slaveId() + ",") >= 0) {
                    slaveExecHvac(data);
                }
            } else {
                slaveExecHvac(payload);
            }
        } else if (msg.startsWith("CONFIG:")) {
            handleSlaveConfig(msg.substring(7), remoteIp_);
        } else if (msg.startsWith("ACK:")) {
            String bssid = WiFi.BSSIDstr();
            if (bssid.length() > 0 &&
                strcmp(configStore.cfg.paired_master_bssid, bssid.c_str()) != 0) {
                str::copyTo(configStore.cfg.paired_master_bssid,
                            sizeof(configStore.cfg.paired_master_bssid), bssid);
                configStore.save();
                Serial.printf("[SLAVE] Paired with master BSSID: %s\n", bssid.c_str());
            }
        }
    }

slaveCheckWifi:
    network.maintain();
}
