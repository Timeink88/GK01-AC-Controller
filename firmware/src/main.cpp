#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRac.h>
#include <IRutils.h>
#include "webui.h"

#define IR_TX 14
#define IR_RX 5
#define LED_BLUE 2    // 蓝色 LED — 系统状态（低电平点亮）
#define LED_RED 12    // 红色 LED — IR 活动（低电平点亮）
#define LED_YELLOW 13 // 黄色 LED — 状态指示（低电平点亮）

// LED 低电平有效：LOW=亮, HIGH=灭
#define LED_ON(pin)  digitalWrite(pin, LOW)
#define LED_OFF(pin) digitalWrite(pin, HIGH)

#define AP_SSID "IR-AC"
#define AP_PASS "12345678"
#define AP_IP          10, 1, 1, 1
#define AP_GATEWAY     10, 1, 1, 1
#define AP_SUBNET      255, 255, 255, 0
#define AP_IP_STR      "10.1.1.1"
#define AP_BROADCAST   "10.1.1.255"
#define UDP_PORT       8888

ESP8266WebServer server(80);
IRsend irSend(IR_TX);
IRrecv irRecv(IR_RX, 1024, 50, false);  // timeout 50ms for AC repeat frames
IRac ac(IR_TX);
WiFiUDP udp;

bool isMaster = true;

// ===== 捕获状态 =====
String capturedRaw;
String capturedProto;
int capturedBits = 0;
bool hasNewCapture = false;

// ===== 非阻塞 LED =====
unsigned long ledOffTime = 0;
bool ledBlinking = false;

// ===== 从机重连 =====
unsigned long lastReconnectAttempt = 0;

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
}

void handleHvac() {
  String vendor = server.arg("vendor");
  if (vendor.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_vendor\"}");
    return;
  }

  decode_type_t proto = strToDecodeType(vendor.c_str());
  if (proto == decode_type_t::UNKNOWN) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_vendor\"}");
    return;
  }

  int temp = server.arg("temp").toInt();
  if (temp < 16 || temp > 30) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"temp_out_of_range\"}");
    return;
  }

  stdAc::state_t st = {};
  st.protocol = proto;
  st.model = 1;
  st.power = server.arg("power") == "On";
  st.mode = strToMode(server.arg("mode"));
  st.degrees = temp;
  st.celsius = true;
  st.fanspeed = strToFan(server.arg("fan"));
  st.swingv = strToSwing(server.arg("swing"));
  st.light = true;

  Serial.printf("[HVAC] %s %s %s %dC %s\n",
    vendor.c_str(), server.arg("power").c_str(),
    server.arg("mode").c_str(), temp, server.arg("fan").c_str());

  irRecv.disableIRIn();
  bool ok = ac.sendAc(st);
  irRecv.enableIRIn();

  char msg[256];
  snprintf(msg, sizeof(msg), "HVAC:%s,%s,%s,%d,%s,%s",
    vendor.c_str(),
    st.power ? "1" : "0",
    server.arg("mode").c_str(),
    temp,
    server.arg("fan").c_str(),
    server.arg("swing").c_str());
  broadcastUdp(msg);

  Serial.printf("[HVAC] result: %s\n", ok ? "OK" : "FAIL");
  server.send(200, "application/json",
    ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send_failed\"}");
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

  char prefix[16];
  snprintf(prefix, sizeof(prefix), "RAW:%d:", len);
  String msg = String(prefix) + raw;
  if (msg.length() < 1024) broadcastUdp(msg.c_str());

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
  // data = "VENDOR,POWER,MODE,TEMP,FAN,SWING"
  int p[6], pi = 0;
  for (int i = 0; i < data.length() && pi < 6; ) {
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

  decode_type_t proto = strToDecodeType(vendor.c_str());
  if (proto == decode_type_t::UNKNOWN) return;

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

  Serial.printf("[SLAVE] HVAC %s %s %dC → %s\n", vendor.c_str(), power ? "ON" : "OFF", temp, ok ? "OK" : "FAIL");
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

  if (ledBlinking && millis() >= ledOffTime) {
    LED_OFF(LED_RED);
    ledBlinking = false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    LED_OFF(LED_BLUE);  // 蓝色灭：WiFi 断开
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      Serial.println("[SLAVE] WiFi lost, reconnecting...");
      WiFi.reconnect();
    }
  } else {
    LED_ON(LED_BLUE); // 蓝色亮：WiFi 已连接
  }
}

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

  // 角色检测：扫描 WiFi 判断主从
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
    // 从机模式
    isMaster = false;
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

      // 发送 JOIN 消息激活主机的 UDP 栈
      delay(200);
      udp.beginPacket(AP_IP_STR, UDP_PORT);
      udp.print("JOIN:");
      udp.print(WiFi.macAddress());
      udp.endPacket();

      LED_ON(LED_YELLOW);  // 黄色常亮：电源指示
      LED_ON(LED_BLUE);    // 蓝色常亮：从机已连接主机
      Serial.println("=== Slave Ready ===");
    } else {
      Serial.println("\n[SLAVE] Connect failed, switching to Master");
      isMaster = true;
    }
  }

  if (isMaster) {
    // 主机模式
    WiFi.mode(WIFI_AP);
    IPAddress local_IP(AP_IP);
    IPAddress gateway(AP_GATEWAY);
    IPAddress subnet(AP_SUBNET);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(AP_SSID, AP_PASS, 0);  // 0=自动选最优信道
    Serial.printf("[MASTER] AP: %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    irRecv.enableIRIn();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/hvac", HTTP_POST, handleHvac);
    server.on("/api/send", HTTP_POST, handleSend);
    server.on("/api/capture", HTTP_GET, handleCapture);
    server.begin();

    udp.begin(UDP_PORT);

    // 自触发：发一个空包激活 UDP 广播栈
    delay(100);
    udp.beginPacket(AP_BROADCAST, UDP_PORT);
    udp.print("BOOT:");
    udp.endPacket();

    // 蓝色闪 3 下表示主机启动完成，然后常灭
    for (int i = 0; i < 3; i++) {
      LED_ON(LED_BLUE); delay(100);
      LED_OFF(LED_BLUE);  delay(100);
    }
    LED_ON(LED_YELLOW);  // 黄色常亮：电源指示
    Serial.println("=== Master Ready ===");
  }
}

void loop() {
  if (!isMaster) {
    slaveLoop();
    yield();
    return;
  }

  server.handleClient();

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

  if (ledBlinking && millis() >= ledOffTime) {
    LED_OFF(LED_RED);
    ledBlinking = false;
  }

  yield();
}
