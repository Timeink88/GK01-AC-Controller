#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

#define LED_GPIO2    2
#define LED_BLUE     4
#define LED_RED      12
#define LED_YELLOW   13

#define LED_ON(pin)  digitalWrite(pin, LOW)
#define LED_OFF(pin) digitalWrite(pin, HIGH)

ESP8266WebServer server(80);

const uint8_t kLedPins[] = {LED_GPIO2, LED_BLUE, LED_RED, LED_YELLOW};
const char* kLedNames[] = {"GPIO2/D4", "GPIO4/D2", "GPIO12/D6", "GPIO13/D7"};

void allLedsOff() {
  for (uint8_t i = 0; i < sizeof(kLedPins); i++) {
    LED_OFF(kLedPins[i]);
  }
}

void handleRoot() {
  String html = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>IR-AC DIAG</title><h1>IR-AC DIAG</h1>";
  html += "<p>Firmware reached setup() and WebServer is running.</p>";
  html += "<p>AP: IR-AC-DIAG / 12345678</p>";
  html += "<p>IP: ";
  html += WiFi.softAPIP().toString();
  html += "</p><p>Uptime: ";
  html += String(millis());
  html += " ms</p>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("[DIAG] boot");

  for (uint8_t i = 0; i < sizeof(kLedPins); i++) {
    pinMode(kLedPins[i], OUTPUT);
    LED_OFF(kLedPins[i]);
  }

  for (uint8_t round = 0; round < 3; round++) {
    for (uint8_t i = 0; i < sizeof(kLedPins); i++) LED_ON(kLedPins[i]);
    delay(250);
    allLedsOff();
    delay(250);
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gateway, subnet);
  bool ok = WiFi.softAP("IR-AC-DIAG", "12345678", 1);
  Serial.printf("[DIAG] AP %s IP=%s\n", ok ? "OK" : "FAIL", WiFi.softAPIP().toString().c_str());

  server.on("/", handleRoot);
  server.onNotFound(handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long nextStep = 0;
  static uint8_t idx = 0;
  static unsigned long nextLog = 0;
  unsigned long now = millis();

  if (now >= nextStep) {
    allLedsOff();
    LED_ON(kLedPins[idx]);
    idx = (idx + 1) % sizeof(kLedPins);
    nextStep = now + 500;
  }

  if (now >= nextLog) {
    uint8_t current = (idx + sizeof(kLedPins) - 1) % sizeof(kLedPins);
    Serial.printf("[DIAG] alive led=%s clients=%d heap=%u\n",
                  kLedNames[current], WiFi.softAPgetStationNum(), ESP.getFreeHeap());
    nextLog = now + 2000;
  }

  yield();
}
