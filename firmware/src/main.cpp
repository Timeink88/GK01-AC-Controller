#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
extern "C" {
#include <user_interface.h>
}

#ifndef IRAC_RBOOT_OTA
#define IRAC_RBOOT_OTA 1
#endif

#if IRAC_RBOOT_OTA
#include "rboot.h"
#else
typedef struct {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  mode;
    uint8_t  current_rom;
    uint8_t  gpio_rom;
    uint8_t  count;
    uint8_t  unused[2];
    uint32_t roms[2];
} rboot_config;

static rboot_config rboot_get_config() {
    rboot_config conf = {0xE1, 0x01, 0, 0, 0, 1, {0, 0}, {0x000000, 0x000000}};
    return conf;
}
static uint8_t rboot_get_current_rom() { return 0; }
static bool rboot_set_current_rom(uint8_t) { return false; }
static uint8_t rboot_get_target_rom() { return 0; }
static uint32_t rboot_get_rom_address(uint8_t) { return 0; }
static bool rboot_write_flash(uint32_t, const uint8_t*, size_t) { return false; }
#endif

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
#define SENSOR_TEMP 2   // DS18B20 (GPIO2/D4) — 1-Wire 数据；不要作为状态 LED 使用
#define SENSOR_PIR  16  // AM312 PIR  (GPIO16/D0) — 数字输入
#define LED_BLUE 4    // 蓝色 LED — 系统状态（GPIO4/D2，低电平点亮）
#define LED_RED 12    // 红色 LED — IR 活动（低电平点亮）
#define LED_YELLOW 13 // 黄色 LED — 状态指示（低电平点亮）

// LED 低电平有效：LOW=亮, HIGH=灭
#define LED_ON(pin)  digitalWrite(pin, LOW)
#define LED_OFF(pin) digitalWrite(pin, HIGH)

void setStatusLeds(bool blue, bool red, bool yellow) {
  if (blue) LED_ON(LED_BLUE); else LED_OFF(LED_BLUE);
  if (red) LED_ON(LED_RED); else LED_OFF(LED_RED);
  if (yellow) LED_ON(LED_YELLOW); else LED_OFF(LED_YELLOW);
}

void blinkStatusLed(uint8_t pin, uint8_t count, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < count; i++) {
    LED_ON(pin);
    delay(onMs);
    LED_OFF(pin);
    delay(offMs);
    yield();
  }
}

void blinkLedRestore(uint8_t pin, uint8_t count, uint16_t onMs, uint16_t offMs) {
  bool wasOn = (digitalRead(pin) == LOW);
  blinkStatusLed(pin, count, onMs, offMs);
  if (wasOn) LED_ON(pin);
  else LED_OFF(pin);
}

void bootLedSelfTest() {
  setStatusLeds(true, true, true);
  delay(300);
  setStatusLeds(false, false, false);
  delay(120);
}

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
  char last_vendor[16] = "GREE";
  char last_mode[8] = "Cool";
  uint8_t last_temp = 26;
  char last_fan[8] = "Auto";
  bool last_power = false;
  char device_name[33] = "";
  char device_icon[12] = "ac";
  char device_floor[33] = "";
} cfg;

#define MAX_SLAVES 4
struct SlaveInfo {
  char mac[18] = "";
  uint32_t ip = 0;
  unsigned long lastSeen = 0;
  char name[33] = "";
  char icon[12] = "ac";
  char floor[33] = "";
};
SlaveInfo slaves[MAX_SLAVES];

bool pairingMode = false;
unsigned long pairingUntil = 0;
#define PAIRING_DURATION_MS 60000
#define SLAVES_FILE "/slaves.txt"

String slaveIdFromMac(const char* mac) {
  String id = "";
  for (int i = 12; i < 17; i += 3) {
    if (mac[i]) id += (char)toupper(mac[i]);
    if (mac[i+1]) id += (char)toupper(mac[i+1]);
  }
  return id;
}

String mySlaveId() {
  return slaveIdFromMac(WiFi.macAddress().c_str());
}

int findSlaveSlotByMac(const char* mac) {
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slaves[i].mac[0] != '\0' && strcmp(slaves[i].mac, mac) == 0) return i;
  }
  return -1;
}

int findFreeSlaveSlot() {
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slaves[i].mac[0] == '\0') return i;
  }
  return -1;
}

const char* getApSsid() {
  if (strlen(cfg.ap_ssid)) return cfg.ap_ssid;
  return AP_SSID;
}
const char* getApPass() { return strlen(cfg.ap_pass) ? cfg.ap_pass : AP_PASS; }

void copyToBuffer(char* dst, size_t dstSize, const String& value) {
  if (!dst || dstSize == 0) return;
  strncpy(dst, value.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

String sanitizeConfigValue(String value, size_t maxLen) {
  String out;
  out.reserve(min(value.length(), maxLen));
  for (unsigned int i = 0; i < value.length() && out.length() < maxLen; i++) {
    char c = value.charAt(i);
    if (c == '\r' || c == '\n') c = ' ';
    if ((uint8_t)c < 0x20) continue;
    out += c;
  }
  return out;
}

String sanitizeMetadata(String value, size_t maxLen) {
  value.trim();
  String out;
  out.reserve(min(value.length(), maxLen));
  for (unsigned int i = 0; i < value.length() && out.length() < maxLen; i++) {
    char c = value.charAt(i);
    if (c == '|' || c == ':' || c == '\r' || c == '\n' ||
        c == '<' || c == '>' || c == '&' || c == '"' ||
        c == '\'' || c == '`') {
      c = ' ';
    }
    if ((uint8_t)c < 0x20) continue;
    out += c;
  }
  out.trim();
  return out;
}

String sanitizeIconKey(String value) {
  value.trim();
  String out;
  out.reserve(11);
  for (unsigned int i = 0; i < value.length() && out.length() < 11; i++) {
    char c = value.charAt(i);
    if (isalnum((unsigned char)c) || c == '_' || c == '-') out += c;
  }
  if (out.length() == 0) out = "ac";
  return out;
}

bool isValidMacString(const String& mac) {
  if (mac.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
      if (mac.charAt(i) != ':') return false;
    } else if (!isxdigit((unsigned char)mac.charAt(i))) {
      return false;
    }
  }
  return true;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseMacBytes(const char* mac, uint8_t out[6]) {
  if (!mac || strlen(mac) != 17) return false;
  for (int i = 0; i < 6; i++) {
    int pos = i * 3;
    int hi = hexNibble(mac[pos]);
    int lo = hexNibble(mac[pos + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i < 5 && mac[pos + 2] != ':') return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool replaceLittleFSFile(const char* target, const char* tmp, const char* bak, const char* tag) {
  LittleFS.remove(bak);
  bool hadOld = LittleFS.exists(target);
  if (hadOld && !LittleFS.rename(target, bak)) {
    Serial.printf("[%s] backup rename failed\n", tag);
    LittleFS.remove(tmp);
    return false;
  }

  if (!LittleFS.rename(tmp, target)) {
    Serial.printf("[%s] commit rename failed\n", tag);
    if (hadOld) LittleFS.rename(bak, target);
    return false;
  }

  if (hadOld) LittleFS.remove(bak);
  return true;
}

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
unsigned long lastHelloSent = 0;
#define HELLO_INTERVAL_MS 30000
bool masterApStarted = false;

// ===== GPIO13 按键（与黄色 LED 复用）=====
unsigned long btnPressStart = 0;
bool btnPressed = false;
bool btnLongHandled = false;
unsigned long lastBtnCheck = 0;
#define BTN_CHECK_INTERVAL 50   // 按键轮询间隔 (ms)
#define BTN_LONG_PRESS_MS 5000  // 长按恢复出厂阈值 (ms)
#define BTN_SHORT_PRESS_MIN_MS 80
#define BTN_SHORT_PRESS_MAX_MS 1200
#define OFFICE_PRESET_VENDOR "GREE"
#define OFFICE_PRESET_MODE   "Cool"
#define OFFICE_PRESET_TEMP   26
#define OFFICE_PRESET_FAN    "Auto"
#define OFFICE_PRESET_SWING  "Off"
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
bool sensorsInitialized = false;
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
static bool otaHeaderChecked = false;
static char otaRejectReason[96] = "";
static uint8_t otaAccBuf[4096];
static size_t otaAccLen = 0;
static unsigned long otaConfirmAt = 0;  // loop 中确认 OTA 的时间点（0=不确认）
#define OTA_CONFIRM_DELAY_MS 25000      // setup 基本检查通过后再稳定运行 25 秒确认
#define OTA_MAX_CRASH_BOOTS 3           // OTA pending 期间允许的最大启动次数，超过则回滚
#define OTA_SLOT_MAX_SIZE 0xFE000       // 每个 rboot ROM slot 约 1016KB
#define OTA_APP_FIRST_SECTION 0x40202010UL
#define OTA_FLASH_MODE_DOUT 0x03

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
  if (!f) {
    f = LittleFS.open("/config.bak", "r");
    if (f) Serial.println("[CFG] config.txt missing, loading backup");
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
    if (key == "ap_ssid") copyToBuffer(cfg.ap_ssid, sizeof(cfg.ap_ssid), sanitizeConfigValue(val, sizeof(cfg.ap_ssid) - 1));
    else if (key == "ap_pass") copyToBuffer(cfg.ap_pass, sizeof(cfg.ap_pass), sanitizeConfigValue(val, sizeof(cfg.ap_pass) - 1));
    else if (key == "ssid") copyToBuffer(cfg.sta_ssid, sizeof(cfg.sta_ssid), sanitizeConfigValue(val, sizeof(cfg.sta_ssid) - 1));
    else if (key == "pass") copyToBuffer(cfg.sta_pass, sizeof(cfg.sta_pass), sanitizeConfigValue(val, sizeof(cfg.sta_pass) - 1));
    else if (key == "mqtt_host") copyToBuffer(cfg.mqtt_host, sizeof(cfg.mqtt_host), sanitizeConfigValue(val, sizeof(cfg.mqtt_host) - 1));
    else if (key == "mqtt_port") cfg.mqtt_port = val.toInt();
    else if (key == "mqtt_user") copyToBuffer(cfg.mqtt_user, sizeof(cfg.mqtt_user), sanitizeConfigValue(val, sizeof(cfg.mqtt_user) - 1));
    else if (key == "mqtt_pass") copyToBuffer(cfg.mqtt_pass, sizeof(cfg.mqtt_pass), sanitizeConfigValue(val, sizeof(cfg.mqtt_pass) - 1));
    else if (key == "mqtt_topic") copyToBuffer(cfg.mqtt_topic, sizeof(cfg.mqtt_topic), sanitizeConfigValue(val, sizeof(cfg.mqtt_topic) - 1));
    else if (key == "force_mode") cfg.force_mode = (uint8_t)val.toInt();
    else if (key == "paired_master_bssid") copyToBuffer(cfg.paired_master_bssid, sizeof(cfg.paired_master_bssid), sanitizeConfigValue(val, sizeof(cfg.paired_master_bssid) - 1));
    else if (key == "last_vendor") copyToBuffer(cfg.last_vendor, sizeof(cfg.last_vendor), sanitizeConfigValue(val, sizeof(cfg.last_vendor) - 1));
    else if (key == "last_mode") copyToBuffer(cfg.last_mode, sizeof(cfg.last_mode), sanitizeConfigValue(val, sizeof(cfg.last_mode) - 1));
    else if (key == "last_temp") cfg.last_temp = (uint8_t)val.toInt();
    else if (key == "last_fan") copyToBuffer(cfg.last_fan, sizeof(cfg.last_fan), sanitizeConfigValue(val, sizeof(cfg.last_fan) - 1));
    else if (key == "last_power") cfg.last_power = (val.toInt() != 0);
    else if (key == "device_name") copyToBuffer(cfg.device_name, sizeof(cfg.device_name), sanitizeMetadata(val, sizeof(cfg.device_name) - 1));
    else if (key == "device_icon") copyToBuffer(cfg.device_icon, sizeof(cfg.device_icon), sanitizeIconKey(val));
    else if (key == "device_floor") copyToBuffer(cfg.device_floor, sizeof(cfg.device_floor), sanitizeMetadata(val, sizeof(cfg.device_floor) - 1));
  }
  f.close();
  if (cfg.force_mode > FORCE_MODE_HOME) cfg.force_mode = FORCE_MODE_AUTO;
  if (cfg.mqtt_port == 0) cfg.mqtt_port = 1883;
  cfg.last_temp = constrain(cfg.last_temp, 16, 30);
  Serial.printf("[CFG] ap=%s sta=%s mqtt=%s:%d topic=%s\n",
    getApSsid(), cfg.sta_ssid, cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_topic);
  return strlen(cfg.sta_ssid) > 0;
}

void saveConfig() {
  File f = LittleFS.open("/config.tmp", "w");
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
  f.printf("last_vendor=%s\n", cfg.last_vendor);
  f.printf("last_mode=%s\n", cfg.last_mode);
  f.printf("last_temp=%d\n", cfg.last_temp);
  f.printf("last_fan=%s\n", cfg.last_fan);
  f.printf("last_power=%d\n", cfg.last_power ? 1 : 0);
  f.printf("device_name=%s\n", cfg.device_name);
  f.printf("device_icon=%s\n", cfg.device_icon);
  f.printf("device_floor=%s\n", cfg.device_floor);
  f.close();
  if (!replaceLittleFSFile("/config.txt", "/config.tmp", "/config.bak", "CFG")) return;
  Serial.println("[CFG] saved");
}

void enterRecoveryAp(const char* reason) {
  Serial.printf("[RECOVERY] %s; opening AP mode for configuration\n", reason);
  blinkStatusLed(LED_RED, 3, 80, 80);
  deviceMode = MODE_AP_MASTER;

  if (cfg.force_mode != FORCE_MODE_AUTO) {
    cfg.force_mode = FORCE_MODE_AUTO;
    saveConfig();
  }
}

bool startMasterAp() {
  masterApStarted = false;
  WiFi.mode(WIFI_AP);
  IPAddress local_IP(AP_IP);
  IPAddress gateway(AP_GATEWAY);
  IPAddress subnet(AP_SUBNET);
  if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
    Serial.println("[MASTER] softAPConfig failed");
  }

  bool ok = WiFi.softAP(getApSsid(), getApPass(), 1);
  if (!ok && strlen(cfg.ap_pass) > 0) {
    Serial.println("[MASTER] AP start failed, clearing custom AP password");
    cfg.ap_pass[0] = '\0';
    saveConfig();
    ok = WiFi.softAP(getApSsid(), getApPass(), 1);
  }

  if (!ok && strlen(cfg.ap_ssid) > 0) {
    Serial.println("[MASTER] AP start failed, clearing custom AP SSID");
    cfg.ap_ssid[0] = '\0';
    saveConfig();
    ok = WiFi.softAP(getApSsid(), getApPass(), 1);
  }

  if (!ok) {
    Serial.println("[MASTER] AP start failed");
    blinkStatusLed(LED_RED, 8, 80, 80);
    return false;
  }

  Serial.printf("[MASTER] AP: %s  http://%s\n",
                getApSsid(), WiFi.softAPIP().toString().c_str());
  masterApStarted = true;
  return true;
}

void clearSlaveSlot(int slot) {
  if (slot < 0 || slot >= MAX_SLAVES) return;
  slaves[slot] = SlaveInfo();
}

void loadPairedSlaves() {
  for (int i = 0; i < MAX_SLAVES; i++) clearSlaveSlot(i);

  File f = LittleFS.open(SLAVES_FILE, "r");
  if (!f) return;

  int slot = 0;
  while (f.available() && slot < MAX_SLAVES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);
    if (p1 != 17 || p2 < 0 || p3 < 0) continue;

    String mac = line.substring(0, p1);
    String name = line.substring(p1 + 1, p2);
    String icon = line.substring(p2 + 1, p3);
    String floor = line.substring(p3 + 1);
    if (!isValidMacString(mac)) continue;
    name = sanitizeMetadata(name, sizeof(slaves[slot].name) - 1);
    icon = sanitizeIconKey(icon);
    floor = sanitizeMetadata(floor, sizeof(slaves[slot].floor) - 1);

    copyToBuffer(slaves[slot].mac, sizeof(slaves[slot].mac), mac);
    copyToBuffer(slaves[slot].name, sizeof(slaves[slot].name), name);
    copyToBuffer(slaves[slot].icon, sizeof(slaves[slot].icon), icon);
    copyToBuffer(slaves[slot].floor, sizeof(slaves[slot].floor), floor);
    slaves[slot].ip = 0;
    slaves[slot].lastSeen = 0;
    slot++;
  }
  f.close();
  Serial.printf("[SLAVE] Loaded %d paired slave(s)\n", slot);
}

void savePairedSlaves() {
  File f = LittleFS.open("/slaves.tmp", "w");
  if (!f) { Serial.println("[SLAVE] paired slave save failed"); return; }

  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slaves[i].mac[0] == '\0') continue;
    String name = sanitizeMetadata(String(slaves[i].name), sizeof(slaves[i].name) - 1);
    String icon = sanitizeIconKey(String(slaves[i].icon));
    String floor = sanitizeMetadata(String(slaves[i].floor), sizeof(slaves[i].floor) - 1);
    f.printf("%s|%s|%s|%s\n", slaves[i].mac, name.c_str(), icon.c_str(), floor.c_str());
  }
  f.close();

  if (!replaceLittleFSFile(SLAVES_FILE, "/slaves.tmp", "/slaves.bak", "SLAVE")) return;
  Serial.println("[SLAVE] paired slaves saved");
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

void handleCaptive() {
  String uri = server.uri();
  if (uri.startsWith("/api/") || uri == "/update") {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
    return;
  }
  if (uri == "/wpad.dat") { server.send(404, "text/plain", ""); return; }
  String host = server.hostHeader();
  if (host.indexOf(AP_IP_STR) >= 0) { handleRoot(); return; }
  const char* target = (uri == "/connecttest.txt")
    ? "http://logout.net"
    : "http://" AP_IP_STR "/";
  server.sendHeader("Location", target, true);
  server.send(302, "text/plain", "");
  server.client().stop();
}

// ===== 空调控制（核心）=====
bool sendHvacCommand(String vendor, bool power, String mode, int temp, String fan, String swing) {
  int minTemp = (vendor == "WAHIN") ? 17 : 16;
  temp = constrain(temp, minTemp, 30);

  if (vendor == "GREE") {
    sendGreeYBOFB(power, mode, temp, fan);
    currentVendor = vendor;
    currentPower = power;
    currentMode = mode;
    currentTemp = temp;
    currentFan = fan;
    return true;
  }
  if (vendor == "WAHIN") {
    sendWahin(power, mode, temp, fan, swing);
    currentVendor = vendor;
    currentPower = power;
    currentMode = mode;
    currentTemp = temp;
    currentFan = fan;
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
  if (ok) {
    currentVendor = vendor;
    currentPower = power;
    currentMode = mode;
    currentTemp = temp;
    currentFan = fan;
  }
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
  temp = constrain(temp, (vendor == "WAHIN") ? 17 : 16, 30);

  bool power = server.arg("power") == "On";
  String mode = server.arg("mode");
  String fan = server.arg("fan");
  String swing = server.arg("swing");

  bool ok = sendHvacCommand(vendor, power, mode, temp, fan, swing);

  if (ok) {
    copyToBuffer(cfg.last_vendor, sizeof(cfg.last_vendor), vendor);
    copyToBuffer(cfg.last_mode, sizeof(cfg.last_mode), mode);
    cfg.last_temp = (uint8_t)temp;
    copyToBuffer(cfg.last_fan, sizeof(cfg.last_fan), fan);
    cfg.last_power = power;
    saveConfig();
  }

  if (ok && deviceMode == MODE_AP_MASTER) {
    String target = server.arg("target");
    if (target.length() == 0) target = "ALL";
    char msg[280];
    snprintf(msg, sizeof(msg), "HVAC:%s:%s,%s,%s,%d,%s,%s",
      target.c_str(), vendor.c_str(), power ? "1" : "0", mode.c_str(), temp,
      fan.c_str(), swing.c_str());
    broadcastUdp(msg);
  }

  if (ok && mqttEnabled && mqtt.connected()) {
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

  static uint16_t buf[512];  // 1KB — 改 static 避免栈爆（handleSend 不重入）
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

  String raw = capturedRaw;
  String proto = capturedProto;
  int bits = capturedBits;
  hasNewCapture = false;

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
  String ssid = sanitizeConfigValue(server.arg("ssid"), sizeof(cfg.sta_ssid) - 1);
  String pass = sanitizeConfigValue(server.arg("pass"), sizeof(cfg.sta_pass) - 1);
  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty_ssid\"}");
    return;
  }
  copyToBuffer(cfg.sta_ssid, sizeof(cfg.sta_ssid), ssid);
  copyToBuffer(cfg.sta_pass, sizeof(cfg.sta_pass), pass);
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
    json += "\"ssid\":\"" + jsonEscape(String(getApSsid())) + "\",";
    json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
    json += "\"rssi\":0,";
  } else {
    json += "\"ssid\":\"" + jsonEscape(String(getApSsid())) + "\",";
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
  String ssid = sanitizeConfigValue(server.arg("ssid"), sizeof(cfg.ap_ssid) - 1);
  String pass = sanitizeConfigValue(server.arg("pass"), sizeof(cfg.ap_pass) - 1);
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
  copyToBuffer(cfg.ap_ssid, sizeof(cfg.ap_ssid), ssid);
  copyToBuffer(cfg.ap_pass, sizeof(cfg.ap_pass), pass);
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleFactoryReset() {
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  LittleFS.remove("/config.txt");
  LittleFS.remove("/config.bak");
  LittleFS.remove("/ota_state.bin");
  LittleFS.remove(SLAVES_FILE);
  LittleFS.remove("/slaves.bak");
  ESP.restart();
}

void handleHvacState() {
  String json = "{";
  json += "\"vendor\":\"" + jsonEscape(String(cfg.last_vendor)) + "\",";
  json += "\"mode\":\"" + jsonEscape(String(cfg.last_mode)) + "\",";
  json += "\"temp\":" + String(cfg.last_temp) + ",";
  json += "\"fan\":\"" + jsonEscape(String(cfg.last_fan)) + "\",";
  json += "\"power\":" + String(cfg.last_power ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSystemInfo() {
  String json = "{";
  json += "\"mac\":\"" + jsonEscape(WiFi.macAddress()) + "\",";
  json += "\"apMac\":\"" + jsonEscape(WiFi.softAPmacAddress()) + "\",";
  json += "\"chipId\":\"" + jsonEscape(String(ESP.getChipId(), HEX)) + "\",";
  json += "\"flashSize\":" + String(ESP.getFlashChipSize()) + ",";
  json += "\"mode\":\"" + jsonEscape(String(modeString())) + "\",";
  json += "\"forceMode\":" + String(cfg.force_mode) + ",";
  json += "\"uptime\":" + String(millis()) + ",";
  json += "\"deviceName\":\"" + jsonEscape(String(cfg.device_name)) + "\",";
  json += "\"deviceIcon\":\"" + jsonEscape(String(cfg.device_icon)) + "\"";
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
    int slot = findSlaveSlotByMac(mac);
    if (slot >= 0) {
      slaves[slot].ip = stat_info->ip.addr;
      slaves[slot].lastSeen = millis();
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
    if (slaves[i].mac[0] != '\0' && slaves[i].lastSeen != 0 &&
        (now - slaves[i].lastSeen) < 30000) {
      if (!first) json += ",";
      json += "{\"mac\":\"" + String(slaves[i].mac) + "\",";
      json += "\"id\":\"" + slaveIdFromMac(slaves[i].mac) + "\",";
      json += "\"ip\":\"" + IPAddress(slaves[i].ip).toString() + "\",";
      json += "\"name\":\"" + jsonEscape(String(slaves[i].name)) + "\",";
      json += "\"icon\":\"" + jsonEscape(String(slaves[i].icon)) + "\",";
      json += "\"floor\":\"" + jsonEscape(String(slaves[i].floor)) + "\",";
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

void handleSlaveConfig() {
  String targetId = server.arg("id");
  String cmd = server.arg("cmd");
  String val = server.arg("val");
  if (targetId.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_id\"}");
    return;
  }
  IPAddress targetIp(0,0,0,0);
  int slot = -1;
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slaveIdFromMac(slaves[i].mac) == targetId) {
      targetIp = slaves[i].ip;
      slot = i;
      break;
    }
  }
  if (targetIp == IPAddress(0,0,0,0)) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"slave_not_found\"}");
    return;
  }
  bool valueCmd = (cmd == "name" || cmd == "icon" || cmd == "floor" ||
                   cmd == "ap_ssid" || cmd == "ap_pass");
  bool actionCmd = (cmd == "reboot" || cmd == "disconnect");
  if (!valueCmd && !actionCmd) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_cmd\"}");
    return;
  }
  String safeVal = val;
  if (cmd == "name") safeVal = sanitizeMetadata(val, sizeof(slaves[0].name) - 1);
  else if (cmd == "icon") safeVal = sanitizeIconKey(val);
  else if (cmd == "floor") safeVal = sanitizeMetadata(val, sizeof(slaves[0].floor) - 1);
  else if (cmd == "ap_ssid") safeVal = sanitizeConfigValue(val, sizeof(cfg.ap_ssid) - 1);
  else if (cmd == "ap_pass") safeVal = sanitizeConfigValue(val, sizeof(cfg.ap_pass) - 1);

  String msg = "CONFIG:" + targetId + ":" + cmd;
  if (valueCmd) msg += "=" + safeVal;
  udp.beginPacket(targetIp, UDP_PORT);
  udp.print(msg);
  udp.endPacket();

  if (slot >= 0) {
    bool shouldSave = false;
    if (cmd == "name") {
      copyToBuffer(slaves[slot].name, sizeof(slaves[slot].name), safeVal);
      shouldSave = true;
    } else if (cmd == "icon") {
      copyToBuffer(slaves[slot].icon, sizeof(slaves[slot].icon), safeVal);
      shouldSave = true;
    } else if (cmd == "floor") {
      copyToBuffer(slaves[slot].floor, sizeof(slaves[slot].floor), safeVal);
      shouldSave = true;
    } else if (cmd == "disconnect") {
      clearSlaveSlot(slot);
      shouldSave = true;
    }
    if (shouldSave) savePairedSlaves();
  }

  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"sent\"}");
}

void handleDeviceConfig() {
  if (server.method() == HTTP_GET) {
    String json = "{";
    json += "\"name\":\"" + jsonEscape(String(cfg.device_name)) + "\",";
    json += "\"icon\":\"" + jsonEscape(String(cfg.device_icon)) + "\",";
    json += "\"floor\":\"" + jsonEscape(String(cfg.device_floor)) + "\"";
    json += "}";
    server.send(200, "application/json", json);
    return;
  }
  String name = server.arg("name");
  String icon = server.arg("icon");
  String floor = server.arg("floor");
  if (server.hasArg("name")) copyToBuffer(cfg.device_name, sizeof(cfg.device_name), sanitizeMetadata(name, sizeof(cfg.device_name) - 1));
  if (server.hasArg("icon")) copyToBuffer(cfg.device_icon, sizeof(cfg.device_icon), sanitizeIconKey(icon));
  if (server.hasArg("floor")) copyToBuffer(cfg.device_floor, sizeof(cfg.device_floor), sanitizeMetadata(floor, sizeof(cfg.device_floor) - 1));
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}");
  delay(500);
  ESP.restart();
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
  if (newMode == FORCE_MODE_HOME && strlen(cfg.sta_ssid) == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_sta_config\"}");
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

  if (msg.startsWith("HELLO:")) {
    // 格式: HELLO:<mac>:<name>:<icon>:<floor>
    // MAC 是固定 17 字节 ("AA:BB:CC:DD:EE:FF")，内部含 5 个冒号，
    // 不能用 indexOf(':') 定位字段分隔符——会命中 MAC 内部的冒号。
    const int MAC_LEN = 17;
    const int HDR_LEN = 6;  // "HELLO:"
    String mac = "";
    String name = "";
    String icon = "ac";
    String floor = "";
    if (msg.length() >= HDR_LEN + MAC_LEN) {
      mac = msg.substring(HDR_LEN, HDR_LEN + MAC_LEN);
      mac.trim();
      // MAC 后的分隔符位置
      int fieldStart = HDR_LEN + MAC_LEN;
      if (fieldStart < (int)msg.length() && msg.charAt(fieldStart) == ':') fieldStart++;
      // 剩余 "name:icon:floor" 按 ':' 切分（允许 name 为空，如 "::ac:floor"）
      String rest = msg.substring(fieldStart);
      int c1 = rest.indexOf(':');
      if (c1 >= 0) {
        name = rest.substring(0, c1);
        int c2 = rest.indexOf(':', c1 + 1);
        if (c2 >= 0) {
          icon = rest.substring(c1 + 1, c2);
          floor = rest.substring(c2 + 1);
        } else {
          icon = rest.substring(c1 + 1);
        }
      } else {
        name = rest;
      }
      name = sanitizeMetadata(name, sizeof(slaves[0].name) - 1);
      icon = sanitizeIconKey(icon);
      floor = sanitizeMetadata(floor, sizeof(slaves[0].floor) - 1);
    }
    if (!isValidMacString(mac)) {
      Serial.printf("[MASTER] Ignoring malformed HELLO: %s\n", msg.substring(0, 80).c_str());
      return;
    }

    int slot = findSlaveSlotByMac(mac.c_str());
    bool alreadyKnown = (slot >= 0);
    if (!alreadyKnown && pairingMode) {
      slot = findFreeSlaveSlot();
    }
    bool accepted = (slot >= 0);
    if (accepted) {
      bool changed = !alreadyKnown;
      copyToBuffer(slaves[slot].mac, sizeof(slaves[slot].mac), mac);
      slaves[slot].ip = remoteIp;
      slaves[slot].lastSeen = millis();
      if (name.length() > 0 && strcmp(slaves[slot].name, name.c_str()) != 0) {
        copyToBuffer(slaves[slot].name, sizeof(slaves[slot].name), name);
        changed = true;
      }
      if (icon.length() > 0 && strcmp(slaves[slot].icon, icon.c_str()) != 0) {
        copyToBuffer(slaves[slot].icon, sizeof(slaves[slot].icon), icon);
        changed = true;
      }
      if (floor.length() > 0 && strcmp(slaves[slot].floor, floor.c_str()) != 0) {
        copyToBuffer(slaves[slot].floor, sizeof(slaves[slot].floor), floor);
        changed = true;
      }
      if (changed) savePairedSlaves();
    }

    if (accepted) {
      udp.beginPacket(remoteIp, UDP_PORT);
      udp.print("ACK:");
      udp.endPacket();
    } else {
      Serial.printf("[MASTER] Ignoring unpaired slave %s\n", mac.c_str());
    }
  } else if (msg.startsWith("CONFIGACK:")) {
    Serial.printf("[MASTER] CONFIGACK: %s\n", msg.substring(10).c_str());
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
  String host = sanitizeConfigValue(server.arg("host"), sizeof(cfg.mqtt_host) - 1);
  copyToBuffer(cfg.mqtt_host, sizeof(cfg.mqtt_host), host);
  String port = sanitizeConfigValue(server.arg("port"), 5);
  if (port.length() > 0) {
    long newPort = port.toInt();
    if (newPort < 1 || newPort > 65535) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_port\"}");
      return;
    }
    cfg.mqtt_port = (uint16_t)newPort;
  }
  String user = sanitizeConfigValue(server.arg("user"), sizeof(cfg.mqtt_user) - 1);
  copyToBuffer(cfg.mqtt_user, sizeof(cfg.mqtt_user), user);
  if (server.hasArg("pass")) {
    String pass = sanitizeConfigValue(server.arg("pass"), sizeof(cfg.mqtt_pass) - 1);
    copyToBuffer(cfg.mqtt_pass, sizeof(cfg.mqtt_pass), pass);
  }
  String topic = sanitizeConfigValue(server.arg("topic"), sizeof(cfg.mqtt_topic) - 1);
  if (topic.length() > 0) copyToBuffer(cfg.mqtt_topic, sizeof(cfg.mqtt_topic), topic);
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

void initSensors() {
  if (sensorsInitialized) return;
  sensorsInitialized = true;

  pinMode(SENSOR_PIR, INPUT);
  pirDetected = (digitalRead(SENSOR_PIR) == HIGH);

  dallas.begin();
  sensorPresent = (dallas.getDeviceCount() > 0);
  if (sensorPresent) {
    dallas.setResolution(12);
    dallas.requestTemperatures();
    Serial.printf("[SENSOR] DS18B20 found, count=%d\n", dallas.getDeviceCount());
  } else {
    Serial.println("[SENSOR] No DS18B20 detected");
  }
}

void updateSensors() {
  initSensors();
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
void otaReject(const char* reason) {
    otaWriteOk = false;
    strncpy(otaRejectReason, reason, sizeof(otaRejectReason) - 1);
    otaRejectReason[sizeof(otaRejectReason) - 1] = '\0';
    Serial.printf("[OTA] Reject: %s\n", otaRejectReason);
}

uint32_t readLe32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool otaImageHeaderValid(const uint8_t* data, size_t len) {
    if (!data || len < 16) {
        otaReject("image header too short");
        return false;
    }

    if (data[0] != 0xE9) {
        otaReject("invalid ESP8266 image magic");
        return false;
    }

    uint8_t segmentCount = data[1];
    uint8_t flashMode = data[2];
    uint32_t firstSection = readLe32(data + 8);
    if (segmentCount < 3 || segmentCount > 16 || firstSection != OTA_APP_FIRST_SECTION) {
        otaReject("upload stripped OTA app image: firmware-rom0/rom1-*.bin or flash_images/rom0.bin/rom1.bin");
        return false;
    }
    if (flashMode != OTA_FLASH_MODE_DOUT) {
        otaReject("invalid flash mode; this board requires DOUT images");
        return false;
    }

    return true;
}

void otaUploadHandler() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (!server.authenticate("admin", getApPass())) {
            otaWriteOk = false;
            return;
        }
        otaUploadSize = 0;
        otaAccLen = 0;
        otaWriteOk = true;
        otaHeaderChecked = false;
        otaRejectReason[0] = '\0';
        uint8_t target = rboot_get_target_rom();
        otaTargetAddr = rboot_get_rom_address(target);
        if (otaTargetAddr == 0) {
            otaReject("invalid OTA target partition");
            return;
        }
        Serial.printf("[OTA] Start ROM %d at 0x%06X\n", target, otaTargetAddr);
    } else if (upload.status == UPLOAD_FILE_WRITE && otaWriteOk) {
        // 防御: 固件大小不能超过 ROM slot (~1016KB = 0xFE000)
        if (otaUploadSize + otaAccLen + upload.currentSize > OTA_SLOT_MAX_SIZE) {
            Serial.printf("[OTA] FIRMWARE TOO LARGE: %d + %d bytes\n",
                          otaUploadSize + otaAccLen, upload.currentSize);
            otaReject("firmware image is too large for OTA slot");
            return;
        }

        size_t inPos = 0;
        while (inPos < upload.currentSize && otaWriteOk) {
            size_t space = sizeof(otaAccBuf) - otaAccLen;
            size_t remaining = upload.currentSize - inPos;
            size_t copy = (remaining < space) ? remaining : space;
            memcpy(otaAccBuf + otaAccLen, upload.buf + inPos, copy);
            otaAccLen += copy;
            inPos += copy;

            if (!otaHeaderChecked && otaAccLen >= 16) {
                otaHeaderChecked = true;
                if (!otaImageHeaderValid(otaAccBuf, otaAccLen)) return;
            }

            if (otaAccLen >= sizeof(otaAccBuf)) {
                otaWriteOk = rboot_write_flash(otaTargetAddr + otaUploadSize,
                                               otaAccBuf, otaAccLen);
                otaUploadSize += otaAccLen;
                otaAccLen = 0;
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!otaHeaderChecked && otaWriteOk) {
            otaHeaderChecked = true;
            otaImageHeaderValid(otaAccBuf, otaAccLen);
        }
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
    if (!server.authenticate("admin", getApPass())) {
        return server.requestAuthentication();
    }
    if (!otaWriteOk || otaUploadSize == 0) {
        String msg = "OTA write failed";
        if (strlen(otaRejectReason) > 0) {
            msg += ": ";
            msg += otaRejectReason;
        }
        server.send(500, "text/plain", msg);
        return;
    }

    otaState.magic = OTA_STATE_MAGIC;
    otaState.version = OTA_STATE_VERSION;
    otaState.pending = true;
    otaState.previous_rom = rboot_get_current_rom();
    otaState.timestamp = millis();

    File f = LittleFS.open("/ota_state.bin", "w");
    if (!f) {
        server.send(500, "text/plain", "OTA state write failed");
        return;
    }
    size_t written = f.write((uint8_t*)&otaState, sizeof(otaState));
    f.close();
    if (written != sizeof(otaState)) {
        server.send(500, "text/plain", "OTA state write incomplete");
        return;
    }

    uint8_t target = rboot_get_target_rom();
    Serial.printf("[OTA] Switching to ROM %d, rebooting...\n", target);
    rboot_set_current_rom(target);
    delay(100);
    ESP.restart();
}

void handleOTAStatus() {
    uint8_t cur = rboot_get_current_rom();
    uint8_t target = rboot_get_target_rom();
    rboot_config conf = rboot_get_config();
    String json = "{\"ok\":true,\"current_rom\":";
    json += String(cur);
    json += ",\"target_rom\":";
    json += String(target);
    json += ",\"rom0_addr\":\"0x";
    json += String(conf.roms[0], HEX);
    json += "\",\"rom1_addr\":\"0x";
    json += String(conf.roms[1], HEX);
    json += "\",\"target_addr\":\"0x";
    json += String(rboot_get_rom_address(target), HEX);
    json += "\",\"ota_pending\":";
    json += otaState.pending ? "true" : "false";
    json += ",\"slot_max\":";
    json += String(OTA_SLOT_MAX_SIZE);
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

bool rollbackPendingOTA(const char* reason) {
#if !IRAC_RBOOT_OTA
    (void)reason;
    return false;
#endif
    loadOTAState();
    if (!otaState.pending) return false;
    Serial.printf("[OTA] Rollback: %s\n", reason);
    uint8_t prev = otaState.previous_rom;
    clearOTAState();
    LittleFS.remove("/ota_boots.txt");
    rboot_set_current_rom(prev);
    delay(100);
    ESP.restart();
    return true;
}

void checkOTARollback() {
#if !IRAC_RBOOT_OTA
    otaState = {OTA_STATE_MAGIC, OTA_STATE_VERSION, false, 0, 0};
    otaConfirmAt = 0;
    return;
#endif
    loadOTAState();
    if (!otaState.pending) return;

    Serial.println("[OTA] Pending update detected, health check...");

    struct rst_info *rst = ESP.getResetInfoPtr();
    bool isCrash = (rst && (rst->reason == REASON_WDT_RST ||
                            rst->reason == REASON_EXCEPTION_RST ||
                            rst->reason == REASON_SOFT_WDT_RST));
    uint8_t otaBoots = 0;
    if (isCrash) {
      File bf = LittleFS.open("/ota_boots.txt", "r");
      if (bf) { otaBoots = (uint8_t)bf.parseInt(); bf.close(); }
      otaBoots++;
      bf = LittleFS.open("/ota_boots.txt", "w");
      if (bf) { bf.println(otaBoots); bf.close(); }
      Serial.printf("[OTA] Crash boot #%d since OTA (max %d)\n", otaBoots, OTA_MAX_CRASH_BOOTS);
    } else {
      Serial.printf("[OTA] Clean reset, crash counter unchanged\n");
    }

    if (otaBoots > OTA_MAX_CRASH_BOOTS) {
        uint8_t prev = otaState.previous_rom;
        Serial.printf("[OTA] Too many crash boots, rolling back to ROM %d\n", prev);
        clearOTAState();
        LittleFS.remove("/ota_boots.txt");
        rboot_set_current_rom(prev);
        delay(100);
        ESP.restart();
    }

    if (ESP.getFreeHeap() < 5000) {
        Serial.printf("[OTA] Critical low heap: %u, rollback!\n", ESP.getFreeHeap());
        uint8_t prev = otaState.previous_rom;
        clearOTAState();
        LittleFS.remove("/ota_boots.txt");
        rboot_set_current_rom(prev);
        delay(100);
        ESP.restart();
    }

    bool healthy = true;
    if (deviceMode == MODE_STA_HOME || deviceMode == MODE_STA_SLAVE) {
        int wifiWait = 0;
        while (WiFi.status() != WL_CONNECTED && wifiWait < 20) {
            delay(500);
            wifiWait++;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[OTA] WiFi failed, rollback!");
            healthy = false;
        }
    } else if (deviceMode == MODE_AP_MASTER) {
        if (!masterApStarted || WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
            Serial.println("[OTA] AP failed to start, rollback!");
            healthy = false;
        }
    }

    if (healthy) {
        Serial.printf("[OTA] Basic check passed, will confirm after %ds stable run\n", OTA_CONFIRM_DELAY_MS / 1000);
        otaConfirmAt = millis() + OTA_CONFIRM_DELAY_MS;
    } else {
        uint8_t prev = otaState.previous_rom;
        Serial.printf("[OTA] Rolling back to ROM %d\n", prev);
        clearOTAState();
        LittleFS.remove("/ota_boots.txt");
        rboot_set_current_rom(prev);
        delay(100);
        ESP.restart();
    }
}

void confirmOTAIfReady() {
    if (otaConfirmAt == 0 || !otaState.pending) return;
    if (millis() < otaConfirmAt) return;

    Serial.println("[OTA] Stable run reached, update confirmed");
    clearOTAState();
    LittleFS.remove("/ota_boots.txt");
    otaConfirmAt = 0;
}

void registerApiRoutes() {
#if IRAC_RBOOT_OTA
  server.on("/update", HTTP_POST, otaFinishHandler, otaUploadHandler);
  server.on("/api/ota/status", HTTP_GET, handleOTAStatus);
#else
  server.on("/update", HTTP_POST, []() {
    server.send(503, "text/plain", "rboot OTA is disabled in factory firmware");
  }, []() {
    HTTPUpload& upload = server.upload();
    (void)upload;
  });
  server.on("/api/ota/status", HTTP_GET, []() {
    server.send(200, "application/json",
                "{\"ok\":true,\"ota_supported\":false,\"mode\":\"factory\"}");
  });
#endif
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
  server.on("/api/hvac/state", HTTP_GET, handleHvacState);
  server.on("/api/system/info", HTTP_GET, handleSystemInfo);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/pair/start", HTTP_POST, handlePairStart);
  server.on("/api/pair/stop", HTTP_POST, handlePairStop);
  server.on("/api/mode/force", HTTP_POST, handleForceMode);
  server.on("/api/slave/config", HTTP_POST, handleSlaveConfig);
  server.on("/api/device/config", HTTP_ANY, handleDeviceConfig);
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

  String action = "off";
  if (currentPower) {
    if (currentMode == "Cool") action = "cooling";
    else if (currentMode == "Heat") action = "heating";
    else if (currentMode == "Dry") action = "drying";
    else if (currentMode == "Fan") action = "fan";
    else action = "idle";
  }
  mqtt.publish((base + "/action").c_str(), action.c_str(), true);
}

void mqttPublishDiscovery() {
  String base = mqttTopicBase();
  String chipId = String(ESP.getChipId(), HEX);
  String devName = strlen(cfg.device_name) > 0 ? jsonEscape(String(cfg.device_name)) : String("IR AC");
  String disc = String()
    + "{"
    + "\"name\":\"" + devName + "\","
    + "\"unique_id\":\"ir-ac-" + chipId + "\","
    + "\"icon\":\"mdi:air-conditioner\","
    + "\"availability_topic\":\"" + base + "/availability\","
    + "\"payload_available\":\"online\","
    + "\"payload_not_available\":\"offline\","
    + "\"mode_command_topic\":\"" + base + "/mode/set\","
    + "\"mode_state_topic\":\"" + base + "/mode_state\","
    + "\"action_topic\":\"" + base + "/action\","
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
        + "\"identifiers\":[\"ir-ac-" + chipId + "\"],"
        + "\"name\":\"" + devName + "\","
        + "\"manufacturer\":\"DIY\","
        + "\"model\":\"IR Mini V105\","
        + "\"sw_version\":\"2.0\""
    + "}"
    + "}";
  mqtt.publish(("homeassistant/climate/ir-ac-" + chipId + "/config").c_str(),
                 disc.c_str(), true);

  String motionName = strlen(cfg.device_name) > 0 ? jsonEscape(String(cfg.device_name)) + " Motion" : String("IR AC Motion");
  String motionDisc = String()
    + "{"
    + "\"name\":\"" + motionName + "\","
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
  char pbuf[256];
  unsigned int copyLen = (length < sizeof(pbuf) - 1) ? length : sizeof(pbuf) - 1;
  memcpy(pbuf, payload, copyLen);
  pbuf[copyLen] = '\0';
  String t = String(topic);
  String p = String(pbuf);
  Serial.printf("[MQTT] %s -> %s\n", t.c_str(), p.c_str());

  String base = mqttTopicBase();
  String vendor = currentVendor.length() > 0 ? currentVendor
                : (strlen(cfg.last_vendor) > 0 ? String(cfg.last_vendor) : String("GREE"));

  if (t == base + "/mode/set") {
    bool ok = false;
    if (p == "off") {
      ok = sendHvacCommand(vendor, false, currentMode, currentTemp, currentFan, "Off");
    } else if (p == "cool") {
      ok = sendHvacCommand(vendor, true, "Cool", currentTemp, currentFan, "Off");
    } else if (p == "heat") {
      ok = sendHvacCommand(vendor, true, "Heat", currentTemp, currentFan, "Off");
    } else if (p == "fan_only") {
      ok = sendHvacCommand(vendor, true, "Fan", currentTemp, currentFan, "Off");
    } else if (p == "dry") {
      ok = sendHvacCommand(vendor, true, "Dry", currentTemp, currentFan, "Off");
    } else if (p == "auto") {
      ok = sendHvacCommand(vendor, true, "Auto", currentTemp, currentFan, "Off");
    }
    if (ok) mqttPublishState();
  } else if (t == base + "/temperature/set") {
    int newTemp = constrain(p.toInt(), 16, 30);
    if (sendHvacCommand(vendor, currentPower, currentMode, newTemp, currentFan, "Off")) {
      mqttPublishState();
    }
  } else if (t == base + "/fan/set") {
    String newFan = currentFan;
    if (p.length() > 0) {
      newFan = p;
      newFan[0] = toupper(newFan[0]);
    }
    if (sendHvacCommand(vendor, currentPower, currentMode, currentTemp, newFan, "Off")) {
      mqttPublishState();
    }
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
  static uint16_t buf[512];  // 1KB — 改 static 避免栈爆（slaveExecRaw 不重入）
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
  // 格式: vendor,power,mode,temp,fan,swing  (power 为 "0"/"1")
  int c1 = data.indexOf(',');
  if (c1 < 0) return;
  String vendor = data.substring(0, c1);

  int c2 = data.indexOf(',', c1 + 1);
  bool power = (c2 > c1) ? data.substring(c1 + 1, c2) == "1" : false;
  if (c2 < 0) { sendHvacCommand(vendor, power, "Cool", 26, "Auto", "Off"); return; }

  int c3 = data.indexOf(',', c2 + 1);
  String mode = (c3 >= 0) ? data.substring(c2 + 1, c3) : data.substring(c2 + 1);
  if (mode.length() == 0) mode = "Cool";
  if (c3 < 0) { sendHvacCommand(vendor, power, mode, 26, "Auto", "Off"); return; }

  int c4 = data.indexOf(',', c3 + 1);
  String tempStr = (c4 >= 0) ? data.substring(c3 + 1, c4) : data.substring(c3 + 1);
  int temp = tempStr.toInt();
  if (temp == 0) temp = 26;
  if (c4 < 0) { sendHvacCommand(vendor, power, mode, temp, "Auto", "Off"); return; }

  int c5 = data.indexOf(',', c4 + 1);
  String fan = (c5 >= 0) ? data.substring(c4 + 1, c5) : data.substring(c4 + 1);
  if (fan.length() == 0) fan = "Auto";
  String swing = (c5 >= 0) ? data.substring(c5 + 1) : "Off";

  sendHvacCommand(vendor, power, mode, temp, fan, swing);
  Serial.printf("[SLAVE] %s %s %dC\n", vendor.c_str(), power ? "ON" : "OFF", temp);
}

bool applyOfficePresetFromButton() {
  bool power = !currentPower;
  String vendor = OFFICE_PRESET_VENDOR;
  String mode = OFFICE_PRESET_MODE;
  String fan = OFFICE_PRESET_FAN;
  String swing = OFFICE_PRESET_SWING;
  int temp = OFFICE_PRESET_TEMP;

  Serial.printf("[BTN] Office preset: %s %s %dC fan=%s\n",
                vendor.c_str(), power ? "ON" : "OFF", temp, fan.c_str());
  bool ok = sendHvacCommand(vendor, power, mode, temp, fan, swing);
  if (!ok) {
    Serial.println("[BTN] Office preset failed");
    blinkLedRestore(LED_RED, 3, 80, 80);
    return false;
  }

  copyToBuffer(cfg.last_vendor, sizeof(cfg.last_vendor), vendor);
  copyToBuffer(cfg.last_mode, sizeof(cfg.last_mode), mode);
  cfg.last_temp = (uint8_t)temp;
  copyToBuffer(cfg.last_fan, sizeof(cfg.last_fan), fan);
  cfg.last_power = power;
  saveConfig();

  if (deviceMode == MODE_AP_MASTER) {
    char msg[128];
    snprintf(msg, sizeof(msg), "HVAC:ALL:%s,%s,%s,%d,%s,%s",
             vendor.c_str(), power ? "1" : "0", mode.c_str(), temp,
             fan.c_str(), swing.c_str());
    broadcastUdp(msg);
  }

  if (mqttEnabled && mqtt.connected()) {
    mqttPublishState();
  }

  if (power) blinkLedRestore(LED_BLUE, 2, 80, 80);
  else blinkLedRestore(LED_YELLOW, 2, 80, 80);
  return true;
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
      btnLongHandled = false;
      btnPressStart = millis();
      Serial.println("[BTN] Pressed");
    } else {
      unsigned long elapsed = millis() - btnPressStart;
      if (elapsed >= BTN_LONG_PRESS_MS && !btnLongHandled) {
        btnLongHandled = true;
        Serial.println("[BTN] Long press → factory reset!");
        for (int i = 0; i < 10; i++) {
          LED_ON(LED_YELLOW); delay(50);
          LED_OFF(LED_YELLOW); delay(50);
        }
        LittleFS.remove("/config.txt");
        LittleFS.remove("/config.bak");
        LittleFS.remove("/ota_state.bin");
        LittleFS.remove(SLAVES_FILE);
        LittleFS.remove("/slaves.bak");
        ESP.restart();
      } else if (elapsed >= 2000) {
        if ((millis() / 200) % 2 == 0) LED_ON(LED_YELLOW);
        else LED_OFF(LED_YELLOW);
      }
    }
  } else {
    if (btnPressed) {
      unsigned long elapsed = millis() - btnPressStart;
      if (!btnLongHandled && elapsed >= BTN_SHORT_PRESS_MIN_MS &&
          elapsed <= BTN_SHORT_PRESS_MAX_MS) {
        applyOfficePresetFromButton();
      } else if (!btnLongHandled && elapsed > BTN_SHORT_PRESS_MAX_MS) {
        Serial.println("[BTN] Release ignored (hold too long for shortcut)");
      }
    }
    btnPressed = false;
    btnLongHandled = false;
  }
}

void slaveLoop() {
  if (millis() - lastHelloSent > HELLO_INTERVAL_MS) {
    lastHelloSent = millis();
    String hello = "HELLO:" + WiFi.macAddress() + ":" + String(cfg.device_name) + ":" + String(cfg.device_icon) + ":" + String(cfg.device_floor);
    udp.beginPacket(AP_IP_STR, UDP_PORT);
    udp.print(hello);
    udp.endPacket();
  }

  int packetSize = udp.parsePacket();
  if (!packetSize) goto slaveCheckWifi;

  {
  static char buf[1024];  // 改 static 避免栈爆（slaveLoop 不重入）
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) goto slaveCheckWifi;
  buf[len] = '\0';

  String msg = String(buf);
  Serial.printf("[SLAVE] recv: %s\n", msg.substring(0, 80).c_str());

  if (msg.startsWith("RAW:")) {
    int firstColon = msg.indexOf(':', 4);
    if (firstColon > 0) {
      slaveExecRaw(msg.substring(firstColon + 1));
    }
  } else if (msg.startsWith("HVAC:")) {
    String payload = msg.substring(5);
    int colon = payload.indexOf(':');
    if (colon > 0) {
      String target = payload.substring(0, colon);
      String hvacData = payload.substring(colon + 1);
      if (target == "ALL" || ("," + target + ",").indexOf("," + mySlaveId() + ",") >= 0) {
        slaveExecHvac(hvacData);
      }
    } else {
      slaveExecHvac(payload);
    }
  } else if (msg.startsWith("CONFIG:")) {
    String payload = msg.substring(7);
    int colon = payload.indexOf(':');
    if (colon > 0) {
      String target = payload.substring(0, colon);
      String cmd = payload.substring(colon + 1);
      if (target == mySlaveId()) {
        IPAddress masterIp = udp.remoteIP();
        String ack = "CONFIGACK:" + mySlaveId() + ":";
        bool doReboot = false;
        bool doDisconnect = false;
        int eq = cmd.indexOf('=');
        if (eq > 0) {
          String key = cmd.substring(0, eq);
          String val = cmd.substring(eq + 1);
          bool keyOk = true;
          if (key == "name") copyToBuffer(cfg.device_name, sizeof(cfg.device_name), sanitizeMetadata(val, sizeof(cfg.device_name) - 1));
          else if (key == "icon") copyToBuffer(cfg.device_icon, sizeof(cfg.device_icon), sanitizeIconKey(val));
          else if (key == "floor") copyToBuffer(cfg.device_floor, sizeof(cfg.device_floor), sanitizeMetadata(val, sizeof(cfg.device_floor) - 1));
          else if (key == "ap_ssid") copyToBuffer(cfg.ap_ssid, sizeof(cfg.ap_ssid), sanitizeConfigValue(val, sizeof(cfg.ap_ssid) - 1));
          else if (key == "ap_pass") copyToBuffer(cfg.ap_pass, sizeof(cfg.ap_pass), sanitizeConfigValue(val, sizeof(cfg.ap_pass) - 1));
          else { ack += "unknown_key"; keyOk = false; }
          if (keyOk) { saveConfig(); ack += "ok"; }
        } else if (cmd == "reboot") {
          ack += "ok";
          doReboot = true;
        } else if (cmd == "disconnect") {
          ack += "ok";
          doDisconnect = true;
        } else {
          ack += "unknown_cmd";
        }
        udp.beginPacket(masterIp, UDP_PORT);
        udp.print(ack);
        udp.endPacket();
        Serial.printf("[SLAVE] CONFIG %s → %s\n", cmd.c_str(), ack.substring(ack.lastIndexOf(':') + 1).c_str());
        if (doDisconnect) {
          delay(100);
          cfg.force_mode = FORCE_MODE_AP;
          cfg.paired_master_bssid[0] = '\0';
          saveConfig();
          WiFi.disconnect();
          delay(500);
          ESP.restart();
        }
        if (doReboot) {
          delay(100);
          ESP.restart();
        }
      }
    }
  } else if (msg.startsWith("ACK:")) {
    String bssid = WiFi.BSSIDstr();
    if (bssid.length() > 0 && strcmp(cfg.paired_master_bssid, bssid.c_str()) != 0) {
      copyToBuffer(cfg.paired_master_bssid, sizeof(cfg.paired_master_bssid), bssid);
      saveConfig();
      Serial.printf("[SLAVE] Paired with master BSSID: %s\n", bssid.c_str());
    }
  }
  }

slaveCheckWifi:
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
  bootLedSelfTest();
  LED_ON(LED_BLUE);  // 应用已进入 setup，后续阶段会继续更新状态灯

  wifi_set_sleep_type(NONE_SLEEP_T);
  irSend.begin();

  // ===== 初始化文件系统 =====
  blinkStatusLed(LED_BLUE, 1, 80, 80);
  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS mount failed, formatting...");
    blinkStatusLed(LED_RED, 2, 120, 120);
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("[FS] LittleFS still failed after format!");
      blinkStatusLed(LED_RED, 8, 80, 80);
    }
  }

#if IRAC_RBOOT_OTA
  Serial.printf("[BOOT] ROM %d (rboot dual-partition)\n", rboot_get_current_rom());
#else
  Serial.println("[BOOT] factory firmware (direct 0x0, rboot OTA disabled)");
#endif

  // ===== 加载配置 =====
  blinkStatusLed(LED_YELLOW, 1, 80, 80);
  bool hasStoredConfig = LittleFS.exists("/config.txt") || LittleFS.exists("/config.bak");
  bool hasSTA = loadConfig();
  loadPairedSlaves();
  mqttEnabled = strlen(cfg.mqtt_host) > 0;
  currentVendor = strlen(cfg.last_vendor) > 0 ? String(cfg.last_vendor) : String("GREE");
  currentPower = cfg.last_power;
  currentMode = strlen(cfg.last_mode) > 0 ? String(cfg.last_mode) : String("Cool");
  currentTemp = constrain((int)cfg.last_temp, 16, 30);
  currentFan = strlen(cfg.last_fan) > 0 ? String(cfg.last_fan) : String("Auto");
  if (cfg.force_mode == FORCE_MODE_HOME && !hasSTA) {
    Serial.println("[BOOT] force_mode=home without STA config, reverting to auto");
    cfg.force_mode = FORCE_MODE_AUTO;
    saveConfig();
  }
  Serial.printf("[BOOT] STA config: %s force_mode=%d config=%s\n",
                hasSTA ? cfg.sta_ssid : "(none)", cfg.force_mode,
                hasStoredConfig ? "stored" : "fresh");

  // AUTO→优先家庭 WiFi，再扫描主机/开 AP；HOME/AP/SLAVE 为强制模式
  bool skipHome = (cfg.force_mode == FORCE_MODE_AP || cfg.force_mode == FORCE_MODE_SLAVE);
  bool skipScan = (!hasStoredConfig || cfg.force_mode == FORCE_MODE_AP ||
                   cfg.force_mode == FORCE_MODE_HOME);
  bool allowMasterFallback = (cfg.force_mode == FORCE_MODE_AUTO || cfg.force_mode == FORCE_MODE_AP);

  if (hasSTA && !skipHome) {
    Serial.printf("[STA] Connecting to %s...\n", cfg.sta_ssid);
    setStatusLeds(true, false, false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.sta_ssid, cfg.sta_pass);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      if (attempts % 2 == 0) LED_ON(LED_BLUE); else LED_OFF(LED_BLUE);
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

      setStatusLeds(true, false, true);
      Serial.println("=== STA Home Ready ===");
      return;
    }
    Serial.println("\n[STA] Failed, falling back to AP mode");
    blinkStatusLed(LED_RED, 1, 120, 80);
  }

  if (!skipScan) {
  Serial.println("[BOOT] Scanning WiFi...");
  setStatusLeds(false, false, true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  const char* targetPass = getApPass();
  String targetSsid = "";
  uint8_t targetBssid[6] = {0};
  uint8_t pairedBssid[6] = {0};
  int32_t targetChannel = 0;
  int bestRssi = -1000;
  bool foundMaster = false;
  bool foundPaired = false;
  bool targetBssidSet = false;
  bool hasCustomApSsid = strlen(cfg.ap_ssid) > 0;
  bool hasPairedBssid = parseMacBytes(cfg.paired_master_bssid, pairedBssid);

  for (int i = 0; i < n; i++) {
    String scanSsid = WiFi.SSID(i);
    bool ssidMatches = hasCustomApSsid ? (scanSsid == cfg.ap_ssid)
                                       : (scanSsid == getApSsid());
    if (!ssidMatches) continue;

    uint8_t* scanBssid = WiFi.BSSID(i);
    bool bssidMatches = hasPairedBssid && scanBssid &&
                        memcmp(scanBssid, pairedBssid, sizeof(pairedBssid)) == 0;
    if (bssidMatches || (!foundPaired && WiFi.RSSI(i) > bestRssi)) {
      foundMaster = true;
      foundPaired = bssidMatches;
      bestRssi = WiFi.RSSI(i);
      targetSsid = scanSsid;
      targetChannel = WiFi.channel(i);
      if (scanBssid) {
        memcpy(targetBssid, scanBssid, sizeof(targetBssid));
        targetBssidSet = true;
      } else {
        targetBssidSet = false;
      }
      Serial.printf("[BOOT] Candidate AP: %s RSSI=%d%s\n",
                    targetSsid.c_str(), bestRssi,
                    bssidMatches ? " paired" : "");
    }
  }
  WiFi.scanDelete();

  if (foundMaster) {
    deviceMode = MODE_STA_SLAVE;
    setStatusLeds(true, false, true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(targetSsid.c_str(), targetPass, targetChannel,
               targetBssidSet ? targetBssid : nullptr);
    Serial.printf("[SLAVE] Connecting to %s%s...\n",
                  targetSsid.c_str(), foundPaired ? " (paired BSSID)" : "");

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      if (attempts % 2 == 0) {
        LED_ON(LED_BLUE);
        LED_OFF(LED_YELLOW);
      } else {
        LED_OFF(LED_BLUE);
        LED_ON(LED_YELLOW);
      }
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[SLAVE] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      udp.begin(UDP_PORT);
      irRecv.enableIRIn();

      delay(200);
      String hello = "HELLO:" + WiFi.macAddress() + ":" + String(cfg.device_name) + ":" + String(cfg.device_icon) + ":" + String(cfg.device_floor);
      udp.beginPacket(AP_IP_STR, UDP_PORT);
      udp.print(hello);
      udp.endPacket();
      lastHelloSent = millis();

      setStatusLeds(true, false, true);
      Serial.println("=== Slave Ready ===");

      // Slave health depends on the STA connection being up.
      checkOTARollback();
    } else {
      if (allowMasterFallback) {
        Serial.println("\n[SLAVE] Connect failed, switching to Master");
        blinkStatusLed(LED_RED, 1, 120, 80);
        deviceMode = MODE_AP_MASTER;
      } else {
        if (rollbackPendingOTA("force_mode=slave, connect failed")) return;
        enterRecoveryAp("force_mode=slave connect failed");
      }
    }
  }
  }

  if (cfg.force_mode == FORCE_MODE_SLAVE && deviceMode != MODE_STA_SLAVE) {
    if (rollbackPendingOTA("force_mode=slave, no master found")) return;
    enterRecoveryAp("force_mode=slave no master found");
  }
  if (cfg.force_mode == FORCE_MODE_HOME && hasSTA && deviceMode != MODE_STA_HOME) {
    if (rollbackPendingOTA("force_mode=home, STA failed")) return;
    enterRecoveryAp("force_mode=home STA failed");
  }

  if (deviceMode == MODE_AP_MASTER) {
    setStatusLeds(false, false, true);
    bool apStarted = startMasterAp();

    if (apStarted) dnsServer.start(53, "*", WiFi.softAPIP());
    irRecv.enableIRIn();

    registerApiRoutes();
    // Captive Portal — 全平台检测路径
    server.on("/generate_204", handleCaptive);             // Android / Chrome
    server.on("/hotspot-detect.html", handleCaptive);      // iOS / macOS
    server.on("/library/test/success.html", handleCaptive); // macOS 旧版
    server.on("/connecttest.txt", handleCaptive);          // Windows 10/11
    server.on("/connecttest.htm", handleCaptive);          // Windows 旧版
    server.on("/redirect", handleCaptive);                 // Windows 弹窗目标
    server.on("/ncsi.txt", handleCaptive);                 // Windows NCSI
    server.on("/fwlink", handleCaptive);                   // Windows legacy
    server.on("/wpad.dat", handleCaptive);                 // Windows WPAD
    server.onNotFound(handleCaptive);
    if (apStarted) server.begin();

    if (apStarted) udp.begin(UDP_PORT);
    delay(100);
    if (apStarted) {
      udp.beginPacket(AP_BROADCAST, UDP_PORT);
      udp.print("BOOT:");
      udp.endPacket();
    }

    for (int i = 0; i < 3; i++) {
      LED_ON(LED_BLUE); delay(100);
      LED_OFF(LED_BLUE);  delay(100);
    }
    setStatusLeds(false, false, true);
    Serial.println(apStarted ? "=== Master Ready ===" : "=== Master AP Failed ===");

    // AP health can only be checked after softAP/server startup.
    checkOTARollback();
  }
}

// 红灯状态机：OTA 快闪 > 配对慢闪 > IR 脉冲
void updateRedLed() {
  static unsigned long blinkAt = 0;
  static bool on = false;
  unsigned long now = millis();
  if (otaState.pending || otaConfirmAt > 0) {
    if (now >= blinkAt) { blinkAt = now + 200; on = !on; digitalWrite(LED_RED, on ? LOW : HIGH); }
    ledBlinking = false;
  } else if (pairingMode) {
    if (now >= blinkAt) { blinkAt = now + 500; on = !on; digitalWrite(LED_RED, on ? LOW : HIGH); }
    ledBlinking = false;
  } else {
    if (ledBlinking && now >= ledOffTime) { LED_OFF(LED_RED); ledBlinking = false; }
    if (on) { LED_OFF(LED_RED); on = false; }
  }
}

// ===== 主循环 =====
void loop() {
  updateRedLed();

  confirmOTAIfReady();
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
    dnsServer.processNextRequest();
    server.handleClient();
    updateSensors();
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

      capturedRaw = "";
      capturedRaw.reserve(results.rawlen * 6);
      for (uint16_t i = 1; i < results.rawlen; i++) {
        if (i > 1) capturedRaw += ",";
        capturedRaw += String(results.rawbuf[i] * kRawTick);
      }
      capturedProto = String(typeToString(results.decode_type));
      capturedBits = results.bits;
      hasNewCapture = true;

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
