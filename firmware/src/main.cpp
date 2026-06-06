#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRac.h>
#include <IRutils.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "webui.h"

#define IR_TX 14
#define IR_RX 5
#define SENSOR_TEMP 2   // DS18B20 (GPIO2/D4) — 1-Wire 数据
#define SENSOR_PIR  16  // AM312 PIR  (GPIO16/D0) — 数字输入
#define LED_BLUE 2    // 蓝色 LED — 系统状态（低电平点亮）
#define LED_RED 12    // 红色 LED — IR 活动（低电平点亮）
#define LED_YELLOW 13 // 黄色 LED — 状态指示（低电平点亮）

// LED 低电平有效：LOW=亮, HIGH=灭
#define LED_ON(pin)  digitalWrite(pin, LOW)
#define LED_OFF(pin) digitalWrite(pin, HIGH)

// AP 模式网络参数（主机模式使用）
#define AP_SSID "IR-AC"
#define AP_PASS "12345678"
#define AP_IP          10, 1, 1, 1
#define AP_GATEWAY     10, 1, 1, 1
#define AP_SUBNET      255, 255, 255, 0
#define AP_IP_STR      "10.1.1.1"
#define AP_BROADCAST   "10.1.1.255"
#define UDP_PORT       8888

// ===== 设备模式 =====
enum DeviceMode { MODE_AP_MASTER, MODE_STA_SLAVE, MODE_STA_HOME };
DeviceMode deviceMode = MODE_AP_MASTER;

// ===== 全局对象 =====
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater(true);  // true = 启用认证
DNSServer dnsServer;
IRsend irSend(IR_TX);
IRrecv irRecv(IR_RX, 1024, 50, false);
IRac ac(IR_TX);
WiFiUDP udp;
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

// ===== 配置（LittleFS 持久化）=====
struct Config {
  char sta_ssid[64] = "";
  char sta_pass[64] = "";
  char mqtt_host[64] = "";
  uint16_t mqtt_port = 1883;
  char mqtt_user[32] = "";
  char mqtt_pass[32] = "";
  char mqtt_topic[32] = "ir_ac";
} cfg;

// ===== 捕获状态 =====
String capturedRaw;
String capturedProto;
int capturedBits = 0;
bool hasNewCapture = false;

// ===== 非阻塞 LED =====
unsigned long ledOffTime = 0;
bool ledBlinking = false;

// ===== 重连计时器 =====
unsigned long lastReconnectAttempt = 0;
unsigned long lastMqttReconnect = 0;
bool mqttEnabled = false;

// ===== 运行状态（用于 MQTT 状态上报）=====
String currentVendor = "";
bool currentPower = false;
String currentMode = "Cool";
int currentTemp = 26;
String currentFan = "Auto";

// ===== 传感器 =====
OneWire oneWire(SENSOR_TEMP);
DallasTemperature dallas(&oneWire);
bool sensorPresent = false;
float roomTempC = -127.0;
bool pirDetected = false;
unsigned long lastSensorRead = 0;
#define SENSOR_INTERVAL 10000

// ===== 华凌自定义编码器 =====
#define WAHIN_HDR_MARK    4380
#define WAHIN_HDR_SPACE   4420
#define WAHIN_BIT_MARK    460
#define WAHIN_ONE_SPACE   1640
#define WAHIN_ZERO_SPACE  620
#define WAHIN_FRAME_GAP   5230

void wahinSendByte(uint8_t data) {
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    irSend.mark(WAHIN_BIT_MARK);
    irSend.space((data & mask) ? WAHIN_ONE_SPACE : WAHIN_ZERO_SPACE);
  }
}

void wahinSendFrame(const uint8_t data[3]) {
  irSend.mark(WAHIN_HDR_MARK);
  irSend.space(WAHIN_HDR_SPACE);
  for (int i = 0; i < 3; i++) {
    wahinSendByte(data[i]);
    wahinSendByte(~data[i]);
  }
  irSend.mark(WAHIN_BIT_MARK);
  irSend.space(WAHIN_FRAME_GAP);
}

// 华凌温度 Gray 码查表 (index = temp - 17, 范围 17-30°C)
static const uint8_t WAHIN_TEMP_GRAY[] = {
  0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4,
  0xC, 0xD, 0x9, 0x8, 0xA, 0xB
};

void sendWahin(bool power, String mode, int temp, String fan, String swing) {
  temp = constrain(temp, 17, 30);

  uint8_t data[3] = { 0xB2, 0x00, 0x00 };

  uint8_t fanBits = 0xB;
  if (fan == "Low")         fanBits = 0x9;
  else if (fan == "Medium") fanBits = 0x5;
  else if (fan == "High" || fan == "Max") fanBits = 0x3;
  data[1] = (fanBits << 4) | (power ? 0xF : 0xB);

  uint8_t modeBits = 0x0;  // Cool
  if (mode == "Heat")      modeBits = 0x3;
  else if (mode == "Fan")  modeBits = 0x1;
  else if (mode == "Auto") modeBits = 0x2;
  else if (mode == "Dry")  modeBits = 0x2;
  uint8_t tempGray = WAHIN_TEMP_GRAY[temp - 17];
  data[2] = (tempGray << 4) | (modeBits << 2);

  irRecv.disableIRIn();
  irSend.enableIROut(38);
  wahinSendFrame(data);
  wahinSendFrame(data);
  irRecv.enableIRIn();

  Serial.printf("[WAHIN] Pwr:%s Mode:%s T:%d Fan:%s Data=[%02X %02X %02X]\n",
    power ? "ON" : "OFF", mode.c_str(), temp, fan.c_str(),
    data[0], data[1], data[2]);
}

// ===== Gree YBOFB 自定义编码器 =====
#define GREE_HDR_MARK    9000
#define GREE_HDR_SPACE   4500
#define GREE_BIT_MARK    620
#define GREE_ONE_SPACE   1600
#define GREE_ZERO_SPACE  540
#define GREE_BLOCK_GAP   19980
#define GREE_FRAME_GAP   7300

#define GREE_MODE_AUTO   0x00
#define GREE_MODE_COOL   0x01
#define GREE_MODE_DRY    0x02
#define GREE_MODE_FAN    0x03
#define GREE_MODE_HEAT   0x04
#define GREE_POWER_BIT   0x08

#define GREE_FAN_AUTO    0x00
#define GREE_FAN_LOW     0x10
#define GREE_FAN_MED     0x20
#define GREE_FAN_HIGH    0x30

#define GREE_MSG_A       0x50
#define GREE_MSG_B       0x70

uint8_t greeCalcChecksum(const uint8_t data[8]) {
  uint8_t sum = 10;
  sum += data[0] & 0x0F;
  sum += data[1] & 0x0F;
  sum += data[2] & 0x0F;
  sum += data[3] & 0x0F;
  sum += data[4] >> 4;
  sum += data[5] >> 4;
  sum += data[6] >> 4;
  return (sum & 0x0F) << 4;
}

void greeSendByte(uint8_t data) {
  for (uint8_t mask = 1; mask; mask <<= 1) {
    irSend.mark(GREE_BIT_MARK);
    irSend.space((data & mask) ? GREE_ONE_SPACE : GREE_ZERO_SPACE);
  }
}

void greeSendFrame(const uint8_t frame[8], uint32_t endSpace) {
  irSend.mark(GREE_HDR_MARK);
  irSend.space(GREE_HDR_SPACE);

  for (int i = 0; i < 4; i++) greeSendByte(frame[i]);

  irSend.mark(GREE_BIT_MARK); irSend.space(GREE_ZERO_SPACE);
  irSend.mark(GREE_BIT_MARK); irSend.space(GREE_ONE_SPACE);
  irSend.mark(GREE_BIT_MARK); irSend.space(GREE_ZERO_SPACE);

  irSend.mark(GREE_BIT_MARK);
  irSend.space(GREE_BLOCK_GAP);

  for (int i = 4; i < 8; i++) greeSendByte(frame[i]);

  irSend.mark(GREE_BIT_MARK);
  irSend.space(endSpace);
}

void sendGreeYBOFB(bool power, String mode, int temp, String fan) {
  uint8_t frameA[8] = {0x00, 0x00, 0x20, GREE_MSG_A, 0x00, 0x00, 0x00, 0x00};

  uint8_t modeVal = GREE_MODE_COOL;
  if (mode == "Heat")      modeVal = GREE_MODE_HEAT;
  else if (mode == "Dry")  modeVal = GREE_MODE_DRY;
  else if (mode == "Fan")  modeVal = GREE_MODE_FAN;
  else if (mode == "Auto") modeVal = GREE_MODE_AUTO;

  uint8_t fanVal = GREE_FAN_AUTO;
  if (fan == "Low")        fanVal = GREE_FAN_LOW;
  else if (fan == "Medium") fanVal = GREE_FAN_MED;
  else if (fan == "High")  fanVal = GREE_FAN_HIGH;

  temp = constrain(temp, 16, 30);
  frameA[0] = fanVal | modeVal;
  if (power) frameA[0] |= GREE_POWER_BIT;

  frameA[1] = (uint8_t)(temp - 16) & 0x0F;
  frameA[7] = greeCalcChecksum(frameA);

  uint8_t frameB[8];
  memcpy(frameB, frameA, 8);
  frameB[3] = GREE_MSG_B;
  frameB[6] |= fanVal;
  frameB[7] = greeCalcChecksum(frameB);

  irRecv.disableIRIn();
  irSend.enableIROut(38);
  greeSendFrame(frameA, GREE_FRAME_GAP);
  greeSendFrame(frameB, 20000);
  irRecv.enableIRIn();

  Serial.printf("[GREE] Pwr:%s Mode:%s T:%d Fan:%s\n",
    power ? "ON" : "OFF", mode.c_str(), temp, fan.c_str());
  Serial.printf("[GREE] A: %02X %02X %02X %02X %02X %02X %02X %02X\n",
    frameA[0], frameA[1], frameA[2], frameA[3],
    frameA[4], frameA[5], frameA[6], frameA[7]);
  Serial.printf("[GREE] B: %02X %02X %02X %02X %02X %02X %02X %02X\n",
    frameB[0], frameB[1], frameB[2], frameB[3],
    frameB[4], frameB[5], frameB[6], frameB[7]);
}

// ===== 工具函数 =====
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else out += c;
  }
  return out;
}

int parseRaw(String& s, uint16_t* buf, int maxLen) {
  int count = 0, start = 0;
  while (count < maxLen) {
    int comma = s.indexOf(',', start);
    String part = (comma >= 0) ? s.substring(start, comma) : s.substring(start);
    part.trim();
    if (part.length() == 0) break;
    buf[count++] = (uint16_t)part.toInt();
    if (comma < 0) break;
    start = comma + 1;
  }
  return count;
}

stdAc::opmode_t strToMode(String s) {
  if (s == "Heat") return stdAc::opmode_t::kHeat;
  if (s == "Dry")  return stdAc::opmode_t::kDry;
  if (s == "Fan")  return stdAc::opmode_t::kFan;
  if (s == "Auto") return stdAc::opmode_t::kAuto;
  return stdAc::opmode_t::kCool;
}

stdAc::fanspeed_t strToFan(String s) {
  if (s == "Low")     return stdAc::fanspeed_t::kLow;
  if (s == "Medium")  return stdAc::fanspeed_t::kMedium;
  if (s == "High")    return stdAc::fanspeed_t::kHigh;
  if (s == "Highest") return stdAc::fanspeed_t::kMax;
  return stdAc::fanspeed_t::kAuto;
}

stdAc::swingv_t strToSwing(String s) {
  if (s == "Auto")    return stdAc::swingv_t::kAuto;
  if (s == "Highest") return stdAc::swingv_t::kHighest;
  if (s == "Low")     return stdAc::swingv_t::kLow;
  return stdAc::swingv_t::kOff;
}

void broadcastUdp(const char* msg) {
  udp.beginPacket(AP_BROADCAST, UDP_PORT);
  udp.print(msg);
  udp.endPacket();
}

// ===== 前向声明 =====
void mqttPublishState();

// ===== 配置持久化 =====
bool loadConfig() {
  File f = LittleFS.open("/config.txt", "r");
  if (!f) return false;
  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    if (key == "ssid") { strncpy(cfg.sta_ssid, val.c_str(), sizeof(cfg.sta_ssid) - 1); cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1] = '\0'; }
    else if (key == "pass") { strncpy(cfg.sta_pass, val.c_str(), sizeof(cfg.sta_pass) - 1); cfg.sta_pass[sizeof(cfg.sta_pass) - 1] = '\0'; }
    else if (key == "mqtt_host") { strncpy(cfg.mqtt_host, val.c_str(), sizeof(cfg.mqtt_host) - 1); cfg.mqtt_host[sizeof(cfg.mqtt_host) - 1] = '\0'; }
    else if (key == "mqtt_port") cfg.mqtt_port = val.toInt();
    else if (key == "mqtt_user") { strncpy(cfg.mqtt_user, val.c_str(), sizeof(cfg.mqtt_user) - 1); cfg.mqtt_user[sizeof(cfg.mqtt_user) - 1] = '\0'; }
    else if (key == "mqtt_pass") { strncpy(cfg.mqtt_pass, val.c_str(), sizeof(cfg.mqtt_pass) - 1); cfg.mqtt_pass[sizeof(cfg.mqtt_pass) - 1] = '\0'; }
    else if (key == "mqtt_topic") { strncpy(cfg.mqtt_topic, val.c_str(), sizeof(cfg.mqtt_topic) - 1); cfg.mqtt_topic[sizeof(cfg.mqtt_topic) - 1] = '\0'; }
  }
  f.close();
  Serial.printf("[CFG] ssid=%s mqtt=%s:%d topic=%s\n",
    cfg.sta_ssid, cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_topic);
  return strlen(cfg.sta_ssid) > 0;
}

void saveConfig() {
  File f = LittleFS.open("/config.txt", "w");
  if (!f) { Serial.println("[CFG] write failed"); return; }
  f.printf("ssid=%s\n", cfg.sta_ssid);
  f.printf("pass=%s\n", cfg.sta_pass);
  f.printf("mqtt_host=%s\n", cfg.mqtt_host);
  f.printf("mqtt_port=%d\n", cfg.mqtt_port);
  f.printf("mqtt_user=%s\n", cfg.mqtt_user);
  f.printf("mqtt_pass=%s\n", cfg.mqtt_pass);
  f.printf("mqtt_topic=%s\n", cfg.mqtt_topic);
  f.close();
  Serial.println("[CFG] saved");
}

// ===== HTTP 页面处理 =====
void handleRoot() {
  const size_t total = strlen_P(INDEX_HTML);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.setContentLength(total);
  server.send(200, "text/html; charset=UTF-8", "");
  size_t pos = 0;
  while (pos < total) {
    size_t chunk = min((size_t)1400, total - pos);
    server.sendContent_P(INDEX_HTML + pos, chunk);
    pos += chunk;
    yield();
  }
  server.sendContent("");
}

boolean captivePortal() {
  String host = server.hostHeader();
  if (host.indexOf("10.1.1.1") >= 0 || host.indexOf("ir-ac") >= 0) return false;
  server.sendHeader("Location", "http://10.1.1.1/", true);
  server.send(302, "text/plain", "");
  server.client().stop();
  return true;
}

void handleCaptive() {
  if (captivePortal()) return;
  handleRoot();
}

// ===== 空调控制（核心）=====
bool sendHvacCommand(String vendor, bool power, String mode, int temp, String fan, String swing) {
  currentVendor = vendor;
  currentPower = power;
  currentMode = mode;
  currentTemp = temp;
  currentFan = fan;

  if (vendor == "GREE") {
    sendGreeYBOFB(power, mode, temp, fan);
    return true;
  }
  if (vendor == "WAHIN") {
    sendWahin(power, mode, temp, fan, swing);
    return true;
  }

  decode_type_t proto = strToDecodeType(vendor.c_str());
  if (proto == decode_type_t::UNKNOWN) return false;

  stdAc::state_t st = {};
  st.protocol = proto;
  st.model = 1;
  st.power = power;
  st.mode = strToMode(mode);
  st.degrees = temp;
  st.celsius = true;
  st.fanspeed = strToFan(fan);
  st.swingv = strToSwing(swing);
  st.light = true;

  irRecv.disableIRIn();
  bool ok = ac.sendAc(st);
  irRecv.enableIRIn();
  return ok;
}

void handleHvac() {
  String vendor = server.arg("vendor");
  if (vendor.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_vendor\"}");
    return;
  }

  decode_type_t proto = strToDecodeType(vendor.c_str());
  if (proto == decode_type_t::UNKNOWN && vendor != "WAHIN") {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_vendor\"}");
    return;
  }

  int temp = server.arg("temp").toInt();
  if (temp < 16 || temp > 30) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"temp_out_of_range\"}");
    return;
  }

  bool power = server.arg("power") == "On";
  String mode = server.arg("mode");
  String fan = server.arg("fan");
  String swing = server.arg("swing");

  bool ok = sendHvacCommand(vendor, power, mode, temp, fan, swing);

  if (deviceMode == MODE_AP_MASTER) {
    char msg[256];
    snprintf(msg, sizeof(msg), "HVAC:%s,%s,%s,%d,%s,%s",
      vendor.c_str(), power ? "1" : "0", mode.c_str(), temp,
      fan.c_str(), swing.c_str());
    broadcastUdp(msg);
  }

  if (mqttEnabled && mqtt.connected()) {
    mqttPublishState();
  }

  if (ok) {
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"send_failed\"}");
  }
}

void handleSend() {
  String raw = server.arg("raw");
  if (raw.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty\"}");
    return;
  }

  uint16_t buf[512];
  int len = parseRaw(raw, buf, 512);
  if (len == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parse_error\"}");
    return;
  }

  irRecv.disableIRIn();
  irSend.sendRaw(buf, len, 38);
  irRecv.enableIRIn();

  LED_ON(LED_RED);
  ledOffTime = millis() + 30;
  ledBlinking = true;

  if (deviceMode == MODE_AP_MASTER) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "RAW:%d:", len);
    String msg = String(prefix) + raw;
    if (msg.length() < 1024) broadcastUdp(msg.c_str());
  }

  Serial.printf("[SEND] raw len=%d\n", len);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCapture() {
  if (!hasNewCapture) {
    server.send(200, "application/json", "{\"raw\":\"\"}");
    return;
  }

  noInterrupts();
  String raw = capturedRaw;
  String proto = capturedProto;
  int bits = capturedBits;
  hasNewCapture = false;
  interrupts();

  size_t len = raw.length() + proto.length() + 64;
  char* json = (char*)malloc(len);
  if (!json) {
    server.send(500, "application/json", "{\"error\":\"oom\"}");
    return;
  }
  snprintf(json, len, "{\"raw\":\"%s\",\"proto\":\"%s\",\"bits\":%d}",
    raw.c_str(), proto.c_str(), bits);
  server.send(200, "application/json", json);
  free(json);
}

// ===== WiFi 管理 API =====
void handleWifiScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"enc\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? 1 : 0) + "}";
  }
  WiFi.scanDelete();
  json += "]";
  server.send(200, "application/json", json);
}

void handleWifiConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty_ssid\"}");
    return;
  }
  strncpy(cfg.sta_ssid, ssid.c_str(), sizeof(cfg.sta_ssid) - 1);
  cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1] = '\0';
  strncpy(cfg.sta_pass, pass.c_str(), sizeof(cfg.sta_pass) - 1);
  cfg.sta_pass[sizeof(cfg.sta_pass) - 1] = '\0';
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleWifiStatus() {
  String json = "{";
  json += "\"mode\":\"" + String(deviceMode == MODE_STA_HOME ? "sta" :
          (deviceMode == MODE_STA_SLAVE ? "slave" : "ap")) + "\",";
  if (deviceMode == MODE_STA_HOME) {
    json += "\"ssid\":\"" + jsonEscape(String(cfg.sta_ssid)) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  } else if (deviceMode == MODE_AP_MASTER) {
    json += "\"ssid\":\"" + String(AP_SSID) + "\",";
    json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
    json += "\"rssi\":0,";
  } else {
    json += "\"ssid\":\"" + String(AP_SSID) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  }
  json += "\"mqtt\":" + String(mqttEnabled && mqtt.connected() ? "true" : "false") + ",";
  json += "\"mqtt_host\":\"" + jsonEscape(String(cfg.mqtt_host)) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleWifiForget() {
  cfg.sta_ssid[0] = '\0';
  cfg.sta_pass[0] = '\0';
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleMqttConfig() {
  if (server.method() == HTTP_GET) {
    String json = "{";
    json += "\"host\":\"" + jsonEscape(String(cfg.mqtt_host)) + "\",";
    json += "\"port\":" + String(cfg.mqtt_port) + ",";
    json += "\"user\":\"" + jsonEscape(String(cfg.mqtt_user)) + "\",";
    json += "\"topic\":\"" + jsonEscape(String(cfg.mqtt_topic)) + "\"";
    json += "}";
    server.send(200, "application/json", json);
    return;
  }
  // POST: 保存 MQTT 配置
  String host = server.arg("host");
  strncpy(cfg.mqtt_host, host.c_str(), sizeof(cfg.mqtt_host) - 1);
  cfg.mqtt_host[sizeof(cfg.mqtt_host) - 1] = '\0';
  String port = server.arg("port");
  if (port.length() > 0) cfg.mqtt_port = port.toInt();
  String user = server.arg("user");
  strncpy(cfg.mqtt_user, user.c_str(), sizeof(cfg.mqtt_user) - 1);
  cfg.mqtt_user[sizeof(cfg.mqtt_user) - 1] = '\0';
  String pass = server.arg("pass");
  if (pass.length() > 0) strncpy(cfg.mqtt_pass, pass.c_str(), sizeof(cfg.mqtt_pass) - 1);
  cfg.mqtt_pass[sizeof(cfg.mqtt_pass) - 1] = '\0';
  String topic = server.arg("topic");
  if (topic.length() > 0) { strncpy(cfg.mqtt_topic, topic.c_str(), sizeof(cfg.mqtt_topic) - 1); cfg.mqtt_topic[sizeof(cfg.mqtt_topic) - 1] = '\0'; }
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

// ===== 注册所有 API 路由 =====
void handleSensorStatus() {
  String json = "{";
  json += "\"temp\":" + (sensorPresent && roomTempC > -100.0 ? String(roomTempC, 1) : "null") + ",";
  json += "\"motion\":" + String(pirDetected ? "true" : "false") + ",";
  json += "\"sensor_present\":" + String(sensorPresent ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void updateSensors() {
  unsigned long now = millis();
  if (now - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = now;

  pirDetected = (digitalRead(SENSOR_PIR) == HIGH);

  if (sensorPresent) {
    dallas.requestTemperatures();
    float t = dallas.getTempCByIndex(0);
    if (t > -100.0 && t < 85.0) {
      roomTempC = t;
    }
  }
}

void registerApiRoutes() {
  httpUpdater.setup(&server, "admin", "12345678");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/hvac", HTTP_POST, handleHvac);
  server.on("/api/send", HTTP_POST, handleSend);
  server.on("/api/capture", HTTP_GET, handleCapture);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/api/mqtt/config", HTTP_ANY, handleMqttConfig);
  server.on("/api/sensor", HTTP_GET, handleSensorStatus);
}

// ===== MQTT =====
String mqttTopicBase() {
  return String(cfg.mqtt_topic);
}

void mqttPublishState() {
  String base = mqttTopicBase();
  mqtt.publish((base + "/state").c_str(), currentPower ? "on" : "off", true);
  String modeLower = currentMode; modeLower.toLowerCase();
  mqtt.publish((base + "/mode_state").c_str(), modeLower.c_str(), true);
  mqtt.publish((base + "/temperature_state").c_str(), String(currentTemp).c_str(), true);
  String fanLower = currentFan; fanLower.toLowerCase();
  mqtt.publish((base + "/fan_state").c_str(), fanLower.c_str(), true);
  if (sensorPresent && roomTempC > -100.0) {
    mqtt.publish((base + "/current_temperature").c_str(), String(roomTempC, 1).c_str(), true);
  }
  mqtt.publish((base + "/motion").c_str(), pirDetected ? "ON" : "OFF", true);
}

void mqttPublishDiscovery() {
  String base = mqttTopicBase();
  String disc = String()
    + "{"
    + "\"name\":\"IR AC\","
    + "\"unique_id\":\"ir-ac-" + String(ESP.getChipId(), HEX) + "\","
    + "\"icon\":\"mdi:air-conditioner\","
    + "\"availability_topic\":\"" + base + "/availability\","
    + "\"payload_available\":\"online\","
    + "\"payload_not_available\":\"offline\","
    + "\"mode_command_topic\":\"" + base + "/mode/set\","
    + "\"mode_state_topic\":\"" + base + "/mode_state\","
    + "\"modes\":[\"off\",\"cool\",\"heat\",\"fan_only\",\"dry\",\"auto\"],"
    + "\"temperature_command_topic\":\"" + base + "/temperature/set\","
    + "\"temperature_state_topic\":\"" + base + "/temperature_state\","
    + "\"min_temp\":16,\"max_temp\":30,\"temp_step\":1,"
    + "\"fan_mode_command_topic\":\"" + base + "/fan/set\","
    + "\"fan_mode_state_topic\":\"" + base + "/fan_state\","
    + "\"fan_modes\":[\"auto\",\"low\",\"medium\",\"high\"],"
    + "\"current_temperature_topic\":\"" + base + "/current_temperature\","
    + "\"precision\":1.0,"
    + "\"device\":{"
        + "\"identifiers\":[\"ir-ac-" + String(ESP.getChipId(), HEX) + "\"],"
        + "\"name\":\"IR AC\","
        + "\"manufacturer\":\"DIY\","
        + "\"model\":\"IR Mini V105\","
        + "\"sw_version\":\"2.0\""
    + "}"
    + "}";
  mqtt.publish(("homeassistant/climate/ir-ac-" + String(ESP.getChipId(), HEX) + "/config").c_str(),
                disc.c_str(), true);

  String chipId = String(ESP.getChipId(), HEX);
  String motionDisc = String()
    + "{"
    + "\"name\":\"IR AC Motion\","
    + "\"unique_id\":\"ir-ac-motion-" + chipId + "\","
    + "\"state_topic\":\"" + base + "/motion\","
    + "\"device_class\":\"motion\","
    + "\"availability_topic\":\"" + base + "/availability\","
    + "\"payload_available\":\"online\","
    + "\"payload_not_available\":\"offline\","
    + "\"device\":{"
        + "\"identifiers\":[\"ir-ac-" + chipId + "\"]"
    + "}"
    + "}";
  mqtt.publish(("homeassistant/binary_sensor/ir-ac-" + chipId + "/config").c_str(),
                motionDisc.c_str(), true);

  mqtt.publish((base + "/availability").c_str(), "online", true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char pbuf[length + 1];
  memcpy(pbuf, payload, length);
  pbuf[length] = '\0';
  String t = String(topic);
  String p = String(pbuf);
  Serial.printf("[MQTT] %s -> %s\n", t.c_str(), p.c_str());

  String base = mqttTopicBase();
  String vendor = currentVendor.length() > 0 ? currentVendor : "GREE";

  if (t == base + "/mode/set") {
    if (p == "off") {
      sendHvacCommand(vendor, false, currentMode, currentTemp, currentFan, "Off");
    } else if (p == "cool") {
      sendHvacCommand(vendor, true, "Cool", currentTemp, currentFan, "Off");
    } else if (p == "heat") {
      sendHvacCommand(vendor, true, "Heat", currentTemp, currentFan, "Off");
    } else if (p == "fan_only") {
      sendHvacCommand(vendor, true, "Fan", currentTemp, currentFan, "Off");
    } else if (p == "dry") {
      sendHvacCommand(vendor, true, "Dry", currentTemp, currentFan, "Off");
    } else if (p == "auto") {
      sendHvacCommand(vendor, true, "Auto", currentTemp, currentFan, "Off");
    }
    mqttPublishState();
  } else if (t == base + "/temperature/set") {
    currentTemp = p.toInt();
    currentTemp = constrain(currentTemp, 16, 30);
    sendHvacCommand(vendor, currentPower, currentMode, currentTemp, currentFan, "Off");
    mqttPublishState();
  } else if (t == base + "/fan/set") {
    if (p.length() > 0) {
      currentFan = p;
      currentFan[0] = toupper(currentFan[0]);
    }
    sendHvacCommand(vendor, currentPower, currentMode, currentTemp, currentFan, "Off");
    mqttPublishState();
  }
}

bool mqttConnect() {
  if (strlen(cfg.mqtt_host) == 0) return false;

  mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);

  String clientId = String("IR-AC-") + String(ESP.getChipId(), HEX);
  bool ok;
  if (strlen(cfg.mqtt_user) > 0) {
    ok = mqtt.connect(clientId.c_str(), cfg.mqtt_user, cfg.mqtt_pass);
  } else {
    ok = mqtt.connect(clientId.c_str());
  }

  if (ok) {
    Serial.println("[MQTT] Connected");
    mqttEnabled = true;
    String base = mqttTopicBase();
    mqtt.subscribe((base + "/mode/set").c_str());
    mqtt.subscribe((base + "/temperature/set").c_str());
    mqtt.subscribe((base + "/fan/set").c_str());
    mqttPublishDiscovery();
    mqttPublishState();
    return true;
  }
  Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
  return false;
}

// ===== 从机逻辑 =====
void slaveExecRaw(String data) {
  uint16_t buf[512];
  int len = parseRaw(data, buf, 512);
  if (len == 0) return;

  irRecv.disableIRIn();
  irSend.sendRaw(buf, len, 38);
  irRecv.enableIRIn();

  LED_ON(LED_RED);
  ledOffTime = millis() + 30;
  ledBlinking = true;
  Serial.printf("[SLAVE] RAW len=%d\n", len);
}

void slaveExecHvac(String data) {
  int p[6], pi = 0;
  for (int i = 0; i < (int)data.length() && pi < 6; ) {
    int comma = data.indexOf(',', i);
    if (comma < 0) { p[pi++] = i; break; }
    p[pi++] = i;
    i = comma + 1;
  }

  String vendor = (pi > 0) ? data.substring(p[0], data.indexOf(',', p[0])) : "";
  bool power = (pi > 1) ? data.charAt(data.indexOf(',', p[0]) + 1) == '1' : false;
  String mode = (pi > 2) ? data.substring(p[2], data.indexOf(',', p[2])) : "Cool";
  int temp = (pi > 3) ? data.substring(p[3], data.indexOf(',', p[3])).toInt() : 26;
  String fan = (pi > 4) ? data.substring(p[4], data.indexOf(',', p[4])) : "Auto";
  String swing = (pi > 5) ? data.substring(p[5]) : "Off";

  sendHvacCommand(vendor, power, mode, temp, fan, swing);
  Serial.printf("[SLAVE] %s %s %dC\n", vendor.c_str(), power ? "ON" : "OFF", temp);
}

void slaveLoop() {
  int packetSize = udp.parsePacket();
  if (!packetSize) return;

  char buf[1024];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  String msg = String(buf);
  Serial.printf("[SLAVE] recv: %s\n", msg.substring(0, 80).c_str());

  if (msg.startsWith("RAW:")) {
    int firstColon = msg.indexOf(':', 4);
    if (firstColon > 0) {
      slaveExecRaw(msg.substring(firstColon + 1));
    }
  } else if (msg.startsWith("HVAC:")) {
    slaveExecHvac(msg.substring(5));
  }

  if (WiFi.status() != WL_CONNECTED) {
    LED_OFF(LED_BLUE);
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      Serial.println("[SLAVE] WiFi lost, reconnecting...");
      WiFi.reconnect();
    }
  } else {
    LED_ON(LED_BLUE);
  }
}

// ===== 启动 =====
void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  LED_OFF(LED_BLUE);
  LED_OFF(LED_RED);
  LED_OFF(LED_YELLOW);

  wifi_set_sleep_type(NONE_SLEEP_T);
  irSend.begin();

  pinMode(SENSOR_PIR, INPUT);
  dallas.begin();
  sensorPresent = (dallas.getDeviceCount() > 0);
  if (sensorPresent) {
    dallas.setResolution(12);
    dallas.requestTemperatures();
    Serial.printf("[SENSOR] DS18B20 found, count=%d\n", dallas.getDeviceCount());
  } else {
    Serial.println("[SENSOR] No DS18B20 detected");
  }

  // ===== 初始化文件系统 =====
  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS mount failed, formatting...");
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("[FS] LittleFS still failed after format!");
    }
  }

  // ===== 加载配置 =====
  bool hasSTA = loadConfig();
  Serial.printf("[BOOT] STA config: %s\n", hasSTA ? cfg.sta_ssid : "(none)");

  // ===== 优先尝试 STA Home 模式 =====
  if (hasSTA) {
    Serial.printf("[STA] Connecting to %s...\n", cfg.sta_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.sta_ssid, cfg.sta_pass);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      deviceMode = MODE_STA_HOME;
      Serial.printf("\n[STA] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

      registerApiRoutes();
      server.begin();
      irRecv.enableIRIn();

      // 尝试 MQTT
      if (strlen(cfg.mqtt_host) > 0) {
        mqttConnect();
      }

      LED_ON(LED_YELLOW);
      LED_ON(LED_BLUE);
      Serial.println("=== STA Home Ready ===");
      return;
    }
    Serial.println("\n[STA] Failed, falling back to AP mode");
  }

  // ===== 原有逻辑：扫描 IR-AC → 从机或主机 =====
  Serial.println("[BOOT] Scanning WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  bool foundMaster = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == AP_SSID) {
      foundMaster = true;
      Serial.printf("[BOOT] Found existing AP: %s (RSSI: %d)\n", AP_SSID, WiFi.RSSI(i));
      break;
    }
  }
  WiFi.scanDelete();

  if (foundMaster) {
    deviceMode = MODE_STA_SLAVE;
    WiFi.mode(WIFI_STA);
    WiFi.begin(AP_SSID, AP_PASS);
    Serial.printf("[SLAVE] Connecting to %s...\n", AP_SSID);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[SLAVE] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      udp.begin(UDP_PORT);
      irRecv.enableIRIn();

      delay(200);
      udp.beginPacket(AP_IP_STR, UDP_PORT);
      udp.print("JOIN:");
      udp.print(WiFi.macAddress());
      udp.endPacket();

      LED_ON(LED_YELLOW);
      LED_ON(LED_BLUE);
      Serial.println("=== Slave Ready ===");
    } else {
      Serial.println("\n[SLAVE] Connect failed, switching to Master");
      deviceMode = MODE_AP_MASTER;
    }
  }

  if (deviceMode == MODE_AP_MASTER) {
    WiFi.mode(WIFI_AP);
    IPAddress local_IP(AP_IP);
    IPAddress gateway(AP_GATEWAY);
    IPAddress subnet(AP_SUBNET);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(AP_SSID, AP_PASS, 0);
    Serial.printf("[MASTER] AP: %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    dnsServer.start(53, "*", WiFi.softAPIP());
    irRecv.enableIRIn();

    registerApiRoutes();
    // Captive Portal 路由
    server.on("/generate_204", handleCaptive);
    server.on("/hotspot-detect.html", handleCaptive);
    server.on("/connecttest.txt", handleCaptive);
    server.on("/fwlink", handleCaptive);
    server.onNotFound(handleCaptive);
    server.begin();

    udp.begin(UDP_PORT);
    delay(100);
    udp.beginPacket(AP_BROADCAST, UDP_PORT);
    udp.print("BOOT:");
    udp.endPacket();

    for (int i = 0; i < 3; i++) {
      LED_ON(LED_BLUE); delay(100);
      LED_OFF(LED_BLUE);  delay(100);
    }
    LED_ON(LED_YELLOW);
    Serial.println("=== Master Ready ===");
  }
}

// ===== 主循环 =====
void loop() {
  // LED 定时关闭（所有模式通用）
  if (ledBlinking && millis() >= ledOffTime) {
    LED_OFF(LED_RED);
    ledBlinking = false;
  }

  // 从机模式：UDP 监听
  if (deviceMode == MODE_STA_SLAVE) {
    slaveLoop();
    yield();
    return;
  }

  // STA Home 模式：WebServer + MQTT + IR
  if (deviceMode == MODE_STA_HOME) {
    server.handleClient();
    updateSensors();

    if (mqttEnabled) {
      if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastMqttReconnect > 5000) {
          lastMqttReconnect = now;
          mqttConnect();
        }
      } else {
        mqtt.loop();
      }
    }

    // WiFi 断线重连
    if (WiFi.status() != WL_CONNECTED) {
      LED_OFF(LED_BLUE);
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = now;
        Serial.println("[STA] WiFi lost, reconnecting...");
        WiFi.reconnect();
      }
    } else {
      LED_ON(LED_BLUE);
    }
  }

  // AP 主机模式
  if (deviceMode == MODE_AP_MASTER) {
    server.handleClient();
    dnsServer.processNextRequest();
  }

  // IR 捕获（主机和 STA Home 模式）
  if (deviceMode == MODE_AP_MASTER || deviceMode == MODE_STA_HOME) {
    decode_results results;
    if (irRecv.decode(&results)) {
      irRecv.disableIRIn();

      noInterrupts();
      capturedRaw = "";
      capturedRaw.reserve(results.rawlen * 6);
      for (uint16_t i = 1; i < results.rawlen; i++) {
        if (i > 1) capturedRaw += ",";
        capturedRaw += String(results.rawbuf[i] * kRawTick);
      }
      capturedProto = String(typeToString(results.decode_type));
      capturedBits = results.bits;
      hasNewCapture = true;
      interrupts();

      Serial.printf("[IR] %s %dbit %s\n",
        capturedProto.c_str(), capturedBits, capturedRaw.substring(0, 60).c_str());

      LED_ON(LED_RED);
      ledOffTime = millis() + 30;
      ledBlinking = true;

      irRecv.resume();
      irRecv.enableIRIn();
    }
  }

  yield();
}
