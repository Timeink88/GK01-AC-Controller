#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
extern "C" {
#include <user_interface.h>
}
#include "rboot.h"
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
DNSServer dnsServer;
IRsend irSend(IR_TX);
IRrecv irRecv(IR_RX, 1024, 50, false);
IRac ac(IR_TX);
WiFiUDP udp;
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

// ===== 配置（LittleFS 持久化）=====
#define FORCE_MODE_AUTO   0
#define FORCE_MODE_AP     1
#define FORCE_MODE_SLAVE  2
#define FORCE_MODE_HOME   3

struct Config {
  char ap_ssid[33] = "";
  char ap_pass[33] = "";
  char sta_ssid[64] = "";
  char sta_pass[64] = "";
  char mqtt_host[64] = "";
  uint16_t mqtt_port = 1883;
  char mqtt_user[32] = "";
  char mqtt_pass[32] = "";
  char mqtt_topic[32] = "ir_ac";
  uint8_t force_mode = FORCE_MODE_AUTO;
  char paired_master_bssid[18] = "";
} cfg;

#define MAX_SLAVES 4
struct SlaveInfo {
  char mac[18] = "";
  uint32_t ip = 0;
  unsigned long lastSeen = 0;
};
SlaveInfo slaves[MAX_SLAVES];

bool pairingMode = false;
unsigned long pairingUntil = 0;
#define PAIRING_DURATION_MS 60000

const char* getApSsid() {
  if (strlen(cfg.ap_ssid)) return cfg.ap_ssid;
  static char defaultSsid[16];
  snprintf(defaultSsid, sizeof(defaultSsid), "IR-AC-%04X", (uint16_t)(ESP.getChipId() & 0xFFFF));
  return defaultSsid;
}
const char* getApPass() { return strlen(cfg.ap_pass) ? cfg.ap_pass : AP_PASS; }

const char* modeString() {
  switch (deviceMode) {
    case MODE_AP_MASTER: return "ap";
    case MODE_STA_SLAVE: return "slave";
    case MODE_STA_HOME: return "home";
    default: return "unknown";
  }
}

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

// ===== GPIO13 按键（与黄色 LED 复用）=====
unsigned long btnPressStart = 0;
bool btnPressed = false;
unsigned long lastBtnCheck = 0;
#define BTN_CHECK_INTERVAL 50   // 按键轮询间隔 (ms)
#define BTN_LONG_PRESS_MS 5000  // 长按恢复出厂阈值 (ms)
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

// ===== OTA 双分区状态 =====
// magic + version allow future-proofing: if the struct layout changes, an
// older or newer build will see a mismatched magic and reset to defaults
// instead of misinterpreting random bytes as OTA state.
#define OTA_STATE_MAGIC   0x4F544153  // "OTAS"
#define OTA_STATE_VERSION 0x01
struct OTAState {
    uint32_t magic;          // OTA_STATE_MAGIC
    uint8_t  version;        // OTA_STATE_VERSION
    bool     pending;        // OTA 待验证
    uint8_t  previous_rom;   // 上一个 ROM 槽号
    uint32_t timestamp;      // OTA 时间戳
};
OTAState otaState = {OTA_STATE_MAGIC, OTA_STATE_VERSION, false, 0, 0};
static size_t otaUploadSize = 0;
static uint32_t otaTargetAddr = 0;
static bool otaWriteOk = true;
static uint8_t otaAccBuf[4096];
static size_t otaAccLen = 0;

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
    if (key == "ap_ssid") { strncpy(cfg.ap_ssid, val.c_str(), sizeof(cfg.ap_ssid) - 1); cfg.ap_ssid[sizeof(cfg.ap_ssid) - 1] = '\0'; }
    else if (key == "ap_pass") { strncpy(cfg.ap_pass, val.c_str(), sizeof(cfg.ap_pass) - 1); cfg.ap_pass[sizeof(cfg.ap_pass) - 1] = '\0'; }
    else if (key == "ssid") { strncpy(cfg.sta_ssid, val.c_str(), sizeof(cfg.sta_ssid) - 1); cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1] = '\0'; }
    else if (key == "pass") { strncpy(cfg.sta_pass, val.c_str(), sizeof(cfg.sta_pass) - 1); cfg.sta_pass[sizeof(cfg.sta_pass) - 1] = '\0'; }
    else if (key == "mqtt_host") { strncpy(cfg.mqtt_host, val.c_str(), sizeof(cfg.mqtt_host) - 1); cfg.mqtt_host[sizeof(cfg.mqtt_host) - 1] = '\0'; }
    else if (key == "mqtt_port") cfg.mqtt_port = val.toInt();
    else if (key == "mqtt_user") { strncpy(cfg.mqtt_user, val.c_str(), sizeof(cfg.mqtt_user) - 1); cfg.mqtt_user[sizeof(cfg.mqtt_user) - 1] = '\0'; }
    else if (key == "mqtt_pass") { strncpy(cfg.mqtt_pass, val.c_str(), sizeof(cfg.mqtt_pass) - 1); cfg.mqtt_pass[sizeof(cfg.mqtt_pass) - 1] = '\0'; }
    else if (key == "mqtt_topic") { strncpy(cfg.mqtt_topic, val.c_str(), sizeof(cfg.mqtt_topic) - 1); cfg.mqtt_topic[sizeof(cfg.mqtt_topic) - 1] = '\0'; }
    else if (key == "force_mode") cfg.force_mode = (uint8_t)val.toInt();
    else if (key == "paired_master_bssid") { strncpy(cfg.paired_master_bssid, val.c_str(), sizeof(cfg.paired_master_bssid) - 1); cfg.paired_master_bssid[sizeof(cfg.paired_master_bssid) - 1] = '\0'; }
  }
  f.close();
  Serial.printf("[CFG] ap=%s sta=%s mqtt=%s:%d topic=%s\n",
    getApSsid(), cfg.sta_ssid, cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_topic);
  return strlen(cfg.sta_ssid) > 0;
}

void saveConfig() {
  File f = LittleFS.open("/config.txt", "w");
  if (!f) { Serial.println("[CFG] write failed"); return; }
  f.printf("ap_ssid=%s\n", cfg.ap_ssid);
  f.printf("ap_pass=%s\n", cfg.ap_pass);
  f.printf("ssid=%s\n", cfg.sta_ssid);
  f.printf("pass=%s\n", cfg.sta_pass);
  f.printf("mqtt_host=%s\n", cfg.mqtt_host);
  f.printf("mqtt_port=%d\n", cfg.mqtt_port);
  f.printf("mqtt_user=%s\n", cfg.mqtt_user);
  f.printf("mqtt_pass=%s\n", cfg.mqtt_pass);
  f.printf("mqtt_topic=%s\n", cfg.mqtt_topic);
  f.printf("force_mode=%d\n", cfg.force_mode);
  f.printf("paired_master_bssid=%s\n", cfg.paired_master_bssid);
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
    json += "\"ssid\":\"" + String(getApSsid()) + "\",";
    json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
    json += "\"rssi\":0,";
  } else {
    json += "\"ssid\":\"" + String(getApSsid()) + "\",";
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

void handleApConfig() {
  if (server.method() == HTTP_GET) {
    String json = "{";
    json += "\"ssid\":\"" + jsonEscape(String(cfg.ap_ssid)) + "\",";
    json += "\"pass\":\"" + jsonEscape(String(cfg.ap_pass)) + "\"";
    json += "}";
    server.send(200, "application/json", json);
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_ssid\"}");
    return;
  }
  if (pass.length() > 0 && pass.length() < 8) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"password_min_8\"}");
    return;
  }
  if (pass.length() > 32) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"password_max_32\"}");
    return;
  }
  strncpy(cfg.ap_ssid, ssid.c_str(), sizeof(cfg.ap_ssid) - 1);
  cfg.ap_ssid[sizeof(cfg.ap_ssid) - 1] = '\0';
  strncpy(cfg.ap_pass, pass.c_str(), sizeof(cfg.ap_pass) - 1);
  cfg.ap_pass[sizeof(cfg.ap_pass) - 1] = '\0';
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleFactoryReset() {
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  LittleFS.remove("/config.txt");
  LittleFS.remove("/ota_state.bin");
  ESP.restart();
}

void handleSystemInfo() {
  String json = "{";
  json += "\"mac\":\"" + jsonEscape(WiFi.macAddress()) + "\",";
  json += "\"apMac\":\"" + jsonEscape(WiFi.softAPmacAddress()) + "\",";
  json += "\"chipId\":\"" + jsonEscape(String(ESP.getChipId(), HEX)) + "\",";
  json += "\"flashSize\":" + String(ESP.getFlashChipSize()) + ",";
  json += "\"mode\":\"" + jsonEscape(String(modeString())) + "\",";
  json += "\"forceMode\":" + String(cfg.force_mode) + ",";
  json += "\"uptime\":" + String(millis());
  json += "}";
  server.send(200, "application/json", json);
}

void refreshSlaveListFromSDK() {
  struct station_info *stat_info = wifi_softap_get_station_info();
  while (stat_info) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             stat_info->bssid[0], stat_info->bssid[1], stat_info->bssid[2],
             stat_info->bssid[3], stat_info->bssid[4], stat_info->bssid[5]);
    bool found = false;
    for (int i = 0; i < MAX_SLAVES; i++) {
      if (strcmp(slaves[i].mac, mac) == 0) {
        slaves[i].ip = stat_info->ip.addr;
        slaves[i].lastSeen = millis();
        found = true;
        break;
      }
    }
    if (!found) {
      for (int i = 0; i < MAX_SLAVES; i++) {
        if (slaves[i].mac[0] == '\0') {
          strncpy(slaves[i].mac, mac, 17);
          slaves[i].mac[17] = '\0';
          slaves[i].ip = stat_info->ip.addr;
          slaves[i].lastSeen = millis();
          break;
        }
      }
    }
    stat_info = STAILQ_NEXT(stat_info, next);
  }
  wifi_softap_free_station_info();
}

void handleSlaves() {
  if (deviceMode != MODE_AP_MASTER) {
    server.send(200, "application/json", "{\"slaves\":[]}");
    return;
  }
  refreshSlaveListFromSDK();
  unsigned long now = millis();
  String json = "{\"slaves\":[";
  bool first = true;
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slaves[i].mac[0] != '\0' && (now - slaves[i].lastSeen) < 30000) {
      if (!first) json += ",";
      json += "{\"mac\":\"" + String(slaves[i].mac) + "\",";
      json += "\"ip\":\"" + IPAddress(slaves[i].ip).toString() + "\",";
      json += "\"ago\":" + String(now - slaves[i].lastSeen);
      json += "}";
      first = false;
    }
  }
  json += "],\"pairing\":" + String(pairingMode ? 1 : 0);
  if (pairingMode) json += ",\"pairingLeft\":" + String((pairingUntil > now) ? (pairingUntil - now) / 1000 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

void handlePairStart() {
  pairingMode = true;
  pairingUntil = millis() + PAIRING_DURATION_MS;
  Serial.println("[PAIR] Pairing mode enabled for 60s");
  server.send(200, "application/json", "{\"ok\":true,\"duration\":60}");
}

void handlePairStop() {
  pairingMode = false;
  Serial.println("[PAIR] Pairing mode stopped");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleForceMode() {
  String mode = server.arg("mode");
  uint8_t newMode = FORCE_MODE_AUTO;
  if (mode == "ap") newMode = FORCE_MODE_AP;
  else if (mode == "slave") newMode = FORCE_MODE_SLAVE;
  else if (mode == "home") newMode = FORCE_MODE_HOME;
  else if (mode == "auto") newMode = FORCE_MODE_AUTO;
  else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_mode\"}");
    return;
  }
  cfg.force_mode = newMode;
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleMasterUdpReceive() {
  int packetSize = udp.parsePacket();
  if (!packetSize) return;
  char buf[256];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';
  String msg = String(buf);
  IPAddress remoteIp = udp.remoteIP();

  if (msg.startsWith("JOIN:")) {
    String mac = msg.substring(5);
    mac.trim();
    Serial.printf("[MASTER] JOIN from %s @ %s\n", mac.c_str(), remoteIp.toString().c_str());

    bool alreadyKnown = false;
    for (int i = 0; i < MAX_SLAVES; i++) {
      if (strcmp(slaves[i].mac, mac.c_str()) == 0) { alreadyKnown = true; break; }
    }

    if (pairingMode || alreadyKnown) {
      udp.beginPacket(remoteIp, UDP_PORT);
      udp.print("ACK:");
      udp.endPacket();
      Serial.printf("[MASTER] ACK sent to %s\n", remoteIp.toString().c_str());
    }
  }
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

// ===== OTA 双分区上传处理 =====
void otaUploadHandler() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (!server.authenticate("admin", "12345678")) {
            otaWriteOk = false;
            return;
        }
        otaUploadSize = 0;
        otaAccLen = 0;
        otaWriteOk = true;
        uint8_t target = rboot_get_target_rom();
        otaTargetAddr = rboot_get_rom_address(target);
        Serial.printf("[OTA] Start ROM %d at 0x%06X\n", target, otaTargetAddr);
    } else if (upload.status == UPLOAD_FILE_WRITE && otaWriteOk) {
        // 防御: 固件大小不能超过 ROM slot (~1016KB = 0xFE000)
        if (otaUploadSize + otaAccLen + upload.currentSize > 0xFE000) {
            Serial.printf("[OTA] FIRMWARE TOO LARGE: %d + %d bytes\n",
                          otaUploadSize + otaAccLen, upload.currentSize);
            otaWriteOk = false;
            return;
        }
        size_t space = sizeof(otaAccBuf) - otaAccLen;
        size_t copy = (upload.currentSize < space) ? upload.currentSize : space;
        memcpy(otaAccBuf + otaAccLen, upload.buf, copy);
        otaAccLen += copy;
        if (otaAccLen >= sizeof(otaAccBuf)) {
            otaWriteOk = rboot_write_flash(otaTargetAddr + otaUploadSize,
                                           otaAccBuf, otaAccLen);
            otaUploadSize += otaAccLen;
            otaAccLen = 0;
        }
        // 处理剩余数据
        if (copy < upload.currentSize && otaWriteOk) {
            size_t remain = upload.currentSize - copy;
            memcpy(otaAccBuf, upload.buf + copy, remain);
            otaAccLen = remain;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        // 刷写最后一块
        if (otaAccLen > 0 && otaWriteOk) {
            otaWriteOk = rboot_write_flash(otaTargetAddr + otaUploadSize,
                                           otaAccBuf, otaAccLen);
            otaUploadSize += otaAccLen;
            otaAccLen = 0;
        }
        Serial.printf("[OTA] Upload done: %d bytes, %s\n",
                      otaUploadSize, otaWriteOk ? "OK" : "FAILED");
    }
}

void otaFinishHandler() {
    if (!server.authenticate("admin", "12345678")) {
        return server.requestAuthentication();
    }
    if (!otaWriteOk || otaUploadSize == 0) {
        server.send(500, "text/plain", "OTA write failed");
        return;
    }

    otaState.magic = OTA_STATE_MAGIC;
    otaState.version = OTA_STATE_VERSION;
    otaState.pending = true;
    otaState.previous_rom = rboot_get_current_rom();
    otaState.timestamp = millis();

    File f = LittleFS.open("/ota_state.bin", "w");
    if (f) { f.write((uint8_t*)&otaState, sizeof(otaState)); f.close(); }

    uint8_t target = rboot_get_target_rom();
    Serial.printf("[OTA] Switching to ROM %d, rebooting...\n", target);
    rboot_set_current_rom(target);
    delay(100);
    ESP.restart();
}

void handleOTAStatus() {
    uint8_t cur = rboot_get_current_rom();
    rboot_config conf = rboot_get_config();
    String json = "{\"ok\":true,\"current_rom\":";
    json += String(cur);
    json += ",\"rom0_addr\":\"0x";
    json += String(conf.roms[0], HEX);
    json += "\",\"rom1_addr\":\"0x";
    json += String(conf.roms[1], HEX);
    json += "\",\"ota_pending\":";
    json += otaState.pending ? "true" : "false";
    json += ",\"sketch_size\":";
    json += String(ESP.getSketchSize());
    json += ",\"free_space\":";
    json += String(ESP.getFreeSketchSpace());
    json += "}";
    server.send(200, "application/json", json);
}

void loadOTAState() {
    File f = LittleFS.open("/ota_state.bin", "r");
    if (f && f.size() == sizeof(otaState)) {
        f.read((uint8_t*)&otaState, sizeof(otaState));
        f.close();
        if (otaState.magic != OTA_STATE_MAGIC || otaState.version != OTA_STATE_VERSION) {
            // Old format or corrupt — reset to defaults.
            otaState = {OTA_STATE_MAGIC, OTA_STATE_VERSION, false, 0, 0};
        }
    } else {
        otaState = {OTA_STATE_MAGIC, OTA_STATE_VERSION, false, 0, 0};
        if (f) f.close();
    }
}

void clearOTAState() {
    otaState = {OTA_STATE_MAGIC, OTA_STATE_VERSION, false, 0, 0};
    File f = LittleFS.open("/ota_state.bin", "w");
    if (f) { f.write((uint8_t*)&otaState, sizeof(otaState)); f.close(); }
}

void checkOTARollback() {
    loadOTAState();
    if (!otaState.pending) return;

    Serial.println("[OTA] Pending update detected, health check...");

    // 检查 1: LittleFS 能挂载（已挂载）
    // 检查 2: 能执行到这里说明代码在运行
    bool healthy = true;

    // STA Home 模式额外检查 WiFi
    if (deviceMode == MODE_STA_HOME) {
        int wifiWait = 0;
        while (WiFi.status() != WL_CONNECTED && wifiWait < 20) {
            delay(500);
            wifiWait++;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[OTA] WiFi failed, rollback!");
            healthy = false;
        }
    }

    if (healthy) {
        Serial.println("[OTA] Health check passed, update confirmed");
        clearOTAState();
    } else {
        uint8_t prev = otaState.previous_rom;
        Serial.printf("[OTA] Rolling back to ROM %d\n", prev);
        clearOTAState();
        rboot_set_current_rom(prev);
        delay(100);
        ESP.restart();
    }
}

void registerApiRoutes() {
  server.on("/update", HTTP_POST, otaFinishHandler, otaUploadHandler);
  server.on("/api/ota/status", HTTP_GET, handleOTAStatus);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/hvac", HTTP_POST, handleHvac);
  server.on("/api/send", HTTP_POST, handleSend);
  server.on("/api/capture", HTTP_GET, handleCapture);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/api/mqtt/config", HTTP_ANY, handleMqttConfig);
  server.on("/api/ap/config", HTTP_ANY, handleApConfig);
  server.on("/api/factory/reset", HTTP_POST, handleFactoryReset);
  server.on("/api/sensor", HTTP_GET, handleSensorStatus);
  server.on("/api/system/info", HTTP_GET, handleSystemInfo);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/pair/start", HTTP_POST, handlePairStart);
  server.on("/api/pair/stop", HTTP_POST, handlePairStop);
  server.on("/api/mode/force", HTTP_POST, handleForceMode);
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

void checkButton() {
  if (millis() - lastBtnCheck < BTN_CHECK_INTERVAL) return;
  lastBtnCheck = millis();

  bool ledWasOn = (digitalRead(LED_YELLOW) == LOW);
  pinMode(LED_YELLOW, INPUT_PULLUP);
  delayMicroseconds(100);
  bool btnState = digitalRead(LED_YELLOW);
  pinMode(LED_YELLOW, OUTPUT);
  if (ledWasOn) LED_ON(LED_YELLOW); else LED_OFF(LED_YELLOW);

  if (btnState == LOW) {
    if (!btnPressed) {
      btnPressed = true;
      btnPressStart = millis();
      Serial.println("[BTN] Pressed");
    } else {
      unsigned long elapsed = millis() - btnPressStart;
      if (elapsed >= BTN_LONG_PRESS_MS) {
        Serial.println("[BTN] Long press → factory reset!");
        for (int i = 0; i < 10; i++) {
          LED_ON(LED_YELLOW); delay(50);
          LED_OFF(LED_YELLOW); delay(50);
        }
        LittleFS.remove("/config.txt");
        LittleFS.remove("/ota_state.bin");
        ESP.restart();
      } else if (elapsed >= 2000) {
        if ((millis() / 200) % 2 == 0) LED_ON(LED_YELLOW);
        else LED_OFF(LED_YELLOW);
      }
    }
  } else {
    btnPressed = false;
  }
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
  } else if (msg.startsWith("ACK:")) {
    String bssid = WiFi.BSSIDstr();
    if (bssid.length() > 0 && strcmp(cfg.paired_master_bssid, bssid.c_str()) != 0) {
      strncpy(cfg.paired_master_bssid, bssid.c_str(), sizeof(cfg.paired_master_bssid) - 1);
      cfg.paired_master_bssid[sizeof(cfg.paired_master_bssid) - 1] = '\0';
      saveConfig();
      Serial.printf("[SLAVE] Paired with master BSSID: %s\n", bssid.c_str());
    }
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

  Serial.printf("[BOOT] ROM %d (rboot dual-partition)\n", rboot_get_current_rom());

  // ===== 加载配置 =====
  bool hasSTA = loadConfig();
  Serial.printf("[BOOT] STA config: %s force_mode=%d\n", hasSTA ? cfg.sta_ssid : "(none)", cfg.force_mode);

  bool skipHome = (cfg.force_mode == FORCE_MODE_AP || cfg.force_mode == FORCE_MODE_SLAVE);
  bool skipScan = (cfg.force_mode == FORCE_MODE_AP || cfg.force_mode == FORCE_MODE_HOME);
  bool allowMasterFallback = (cfg.force_mode == FORCE_MODE_AUTO || cfg.force_mode == FORCE_MODE_AP);

  if (hasSTA && !skipHome) {
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

      // ===== OTA 健康检查 (STA Home 模式，WiFi 已连接) =====
      checkOTARollback();

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

  // ===== OTA 健康检查（无 STA 配置或 STA 连接失败） =====
  checkOTARollback();

  if (!skipScan) {
  Serial.println("[BOOT] Scanning WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  const char* targetSsid = getApSsid();
  const char* targetPass = getApPass();
  bool foundMaster = false;
  bool targetSsidAllocated = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == targetSsid) {
      foundMaster = true;
      Serial.printf("[BOOT] Found AP: %s (RSSI: %d)\n", targetSsid, WiFi.RSSI(i));
      break;
    }
  }
  if (!foundMaster && strlen(cfg.ap_ssid) == 0) {
    int bestRssi = -1000;
    String bestSsid = "";
    for (int i = 0; i < n; i++) {
      String scanSsid = WiFi.SSID(i);
      if (scanSsid.startsWith("IR-AC-") && WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        bestSsid = scanSsid;
      }
    }
    if (bestSsid.length() > 0) {
      foundMaster = true;
      targetSsid = strdup(bestSsid.c_str());
      targetSsidAllocated = true;
      Serial.printf("[BOOT] Found IR-AC-* prefix AP: %s (RSSI: %d)\n", bestSsid.c_str(), bestRssi);
    }
  }
  WiFi.scanDelete();

  if (foundMaster) {
    deviceMode = MODE_STA_SLAVE;
    WiFi.mode(WIFI_STA);
    WiFi.begin(targetSsid, targetPass);
    Serial.printf("[SLAVE] Connecting to %s...\n", targetSsid);

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
      if (allowMasterFallback) {
        Serial.println("\n[SLAVE] Connect failed, switching to Master");
        deviceMode = MODE_AP_MASTER;
      } else {
        Serial.println("\n[SLAVE] Connect failed, force_mode=slave, retrying in 3s");
        delay(3000);
        ESP.restart();
      }
    }
  }
  if (targetSsidAllocated) free((void*)targetSsid);
  }

  if (cfg.force_mode == FORCE_MODE_SLAVE && deviceMode != MODE_STA_SLAVE) {
    Serial.println("[BOOT] force_mode=slave but no master found, retrying in 3s");
    delay(3000);
    ESP.restart();
  }
  if (cfg.force_mode == FORCE_MODE_HOME && hasSTA && deviceMode != MODE_STA_HOME) {
    Serial.println("[BOOT] force_mode=home but STA failed, retrying in 3s");
    delay(3000);
    ESP.restart();
  }

  if (deviceMode == MODE_AP_MASTER) {
    WiFi.mode(WIFI_AP);
    IPAddress local_IP(AP_IP);
    IPAddress gateway(AP_GATEWAY);
    IPAddress subnet(AP_SUBNET);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(getApSsid(), getApPass(), 0);
    Serial.printf("[MASTER] AP: %s  http://%s\n", getApSsid(), WiFi.softAPIP().toString().c_str());

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

  checkButton();

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
    handleMasterUdpReceive();
  }

  if (pairingMode && pairingUntil < millis()) {
    pairingMode = false;
    Serial.println("[PAIR] Pairing window expired");
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
