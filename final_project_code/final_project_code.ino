#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ── WiFi & Telegram ──
const char *ssid = "Sinthi";
const char *password = "1234sinthi1";
const char *botToken = "";
const char *chatID = "";

// ── Real pins ──
#define SOLAR_BATT_PIN 34
#define PANEL_PIN 32
#define MAIN_BATT_PIN 35
#define RELAY_FAN 26
#define RELAY_LED 27
#define RELAY_SOLAR 14 // CH3
#define RELAY_BATTERY 13 // CH4

// ── Calibration ──
#define SOLAR_BATT_CAL (3.432 / 660.0)
#define PANEL_CAL (3.3 / 4095.0 * 2.0)
#define MAIN_BATT_CAL (3.3 / 4095.0 * 2.0)

// ── Thresholds ──
#define PANEL_THRESHOLD_V 1.5

#define BATT_LOW_ON_V 2.85
#define BATT_LOW_OFF_V 2.70
#define BATT_CRITICAL_V 2.5

#define BATT_CRITICAL_ON_V 2.6
#define BATT_CRITICAL_OFF_V 2.4

// ── Persistent state ──
bool fanOn = true;
bool useSolarBatt = true;
String lastMode = "";

float solarBattSmooth = 0;
float mainBattSmooth = 0;
float panelSmooth = 0;
bool firstRun = true;

// ── Manual control state (Fan and LED controlled independently) ──
bool fanManual = false;
bool ledManual = false;
bool manualFanOn = false;
bool manualLedOn = false;
long lastUpdateId = 0;
unsigned long lastTelegramCheck = 0;

int readStableADC(int pin) {
  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += analogRead(pin);
    delay(3);
  }
  return sum / 50;
}

String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else if (c == '\n') {
      encoded += "%0A";
    } else if (c == ':') {
      encoded += "%3A";
    } else if (c == '.') {
      encoded += "%2E";
    } else {
      encoded += "%" + String((int)c, HEX);
    }
  }
  return encoded;
}

void sendTelegramMessage(String message) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(botToken) +
                 "/sendMessage?chat_id=" + String(chatID) +
                 "&text=" + urlEncode(message);
    http.begin(url);
    http.GET();
    http.end();
  }
}

void checkTelegramCommands() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(botToken) +
               "/getUpdates?offset=" + String(lastUpdateId + 1);
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    // DEBUG: print raw Telegram response
    Serial.println("---- Telegram payload ----");
    Serial.println(payload);
    Serial.println("---------------------------");

    if (payload.indexOf("/fanon") > -1) {
      fanManual = true;
      manualFanOn = true;
      sendTelegramMessage("Fan turned ON manually.");
    }
    if (payload.indexOf("/fanoff") > -1) {
      fanManual = true;
      manualFanOn = false;
      sendTelegramMessage("Fan turned OFF manually.");
    }
    if (payload.indexOf("/ledon") > -1) {
      ledManual = true;
      manualLedOn = true;
      sendTelegramMessage("LED turned ON manually.");
    }
    if (payload.indexOf("/ledoff") > -1) {
      ledManual = true;
      manualLedOn = false;
      sendTelegramMessage("LED turned OFF manually.");
    }
    if (payload.indexOf("/auto") > -1) {
      fanManual = false;
      ledManual = false;
      sendTelegramMessage("Switched back to AUTO mode for both Fan and LED.");
    }

    int idx = payload.lastIndexOf("\"update_id\":");
    if (idx > -1) {
      int start = idx + 12;
      int end = payload.indexOf(',', start);
      String idStr = payload.substring(start, end);
      if (idStr.length() > 0) {
        lastUpdateId = idStr.toInt();
      }
    }

    // DEBUG: confirm lastUpdateId
    Serial.print("lastUpdateId is now: ");
    Serial.println(lastUpdateId);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_FAN, OUTPUT);
  pinMode(RELAY_LED, OUTPUT);
  pinMode(RELAY_SOLAR, OUTPUT);
  pinMode(RELAY_BATTERY, OUTPUT);

  digitalWrite(RELAY_FAN, LOW);
  digitalWrite(RELAY_LED, LOW);
  digitalWrite(RELAY_SOLAR, LOW);
  digitalWrite(RELAY_BATTERY, LOW);

  if (!display.begin(0x3C, true)) {
    Serial.println("OLED init failed - check wiring");
    while (1)
      ;
  }
  display.clearDisplay();
  display.display();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.println(WiFi.localIP());
    sendTelegramMessage("Solar IOT System started successfully. Send /fanon "
                        "/fanoff /ledon /ledoff /auto to control.");
  } else {
    Serial.println(
        "\nWiFi connection failed - continuing without notifications");
  }
}

void loop() {
  int panelRaw = readStableADC(PANEL_PIN);
  int solarBattRaw = readStableADC(SOLAR_BATT_PIN);
  int mainBattRaw = readStableADC(MAIN_BATT_PIN);

  float panelV_raw = panelRaw * PANEL_CAL;
  float solarBattV_raw = solarBattRaw * SOLAR_BATT_CAL;
  float mainBattV_raw = mainBattRaw * MAIN_BATT_CAL;

  if (firstRun) {
    panelSmooth = panelV_raw;
    solarBattSmooth = solarBattV_raw;
    mainBattSmooth = mainBattV_raw;
    firstRun = false;
  } else {
    panelSmooth = 0.85 * panelSmooth + 0.15 * panelV_raw;
    solarBattSmooth = 0.85 * solarBattSmooth + 0.15 * solarBattV_raw;
    mainBattSmooth = 0.85 * mainBattSmooth + 0.15 * mainBattV_raw;
  }

  float panelV = panelSmooth;
  float solarBattV = solarBattSmooth;
  float mainBattV = mainBattSmooth;

  bool sunAvailable = panelV > PANEL_THRESHOLD_V;

  String mode;

  if (sunAvailable) {
    digitalWrite(RELAY_BATTERY, LOW);
    digitalWrite(RELAY_SOLAR, HIGH);
    mode = "SOLAR";
    useSolarBatt = true;
  } else {
    if (solarBattV > BATT_CRITICAL_ON_V) {
      useSolarBatt = true;
    } else if (solarBattV < BATT_CRITICAL_OFF_V) {
      useSolarBatt = false;
    }

    if (useSolarBatt) {
      digitalWrite(RELAY_BATTERY, LOW);
      digitalWrite(RELAY_SOLAR, HIGH);
      mode = "SOLAR BATT";
    } else {
      digitalWrite(RELAY_SOLAR, LOW);
      digitalWrite(RELAY_BATTERY, HIGH);
      mode = "MAIN BATT";
    }
  }

  float activeBattV = useSolarBatt ? solarBattV : mainBattV;
  bool ledOn;

  if (sunAvailable) {
    fanOn = true;
    ledOn = true;
  } else if (activeBattV < BATT_CRITICAL_V) {
    fanOn = false;
    ledOn = false;
  } else {
    ledOn = true;
    if (activeBattV < BATT_LOW_OFF_V) {
      fanOn = false;
    } else if (activeBattV > BATT_LOW_ON_V) {
      fanOn = true;
    }
  }

  // Manual override: Fan and LED controlled independently
  if (fanManual) {
    fanOn = manualFanOn;
  }
  if (ledManual) {
    ledOn = manualLedOn;
  }

  digitalWrite(RELAY_FAN, fanOn ? HIGH : LOW);
  digitalWrite(RELAY_LED, ledOn ? HIGH : LOW);

  if (mode != lastMode) {
    String msg = "Mode changed to: " + mode + "\nPanel: " + String(panelV, 2) +
                 "V" + "\nSolar Batt: " + String(solarBattV, 2) + "V" +
                 "\nMain Batt: " + String(mainBattV, 2) + "V";
    sendTelegramMessage(msg);
    lastMode = mode;
  }

  // Check for Telegram commands every 3 seconds
  if (millis() - lastTelegramCheck > 3000) {
    checkTelegramCommands();
    lastTelegramCheck = millis();
  }

  Serial.print("Panel V: ");
  Serial.print(panelV, 2);
  Serial.print("  SolarBatt V: ");
  Serial.print(solarBattV, 2);
  Serial.print("  MainBatt V: ");
  Serial.print(mainBattV, 2);
  Serial.print("  Mode: ");
  Serial.print(mode);
  Serial.print("  Fan: ");
  Serial.print(fanOn ? "ON" : "OFF");
  Serial.print(fanManual ? "(M)" : "(A)");
  Serial.print("  LED: ");
  Serial.print(ledOn ? "ON" : "OFF");
  Serial.println(ledManual ? "(M)" : "(A)");

  // OLED display
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("SOLAR IOT SYSTEM");

  display.setCursor(0, 12);
  display.print("Panel: ");
  display.print(panelV, 2);
  display.println("V");

  display.setCursor(0, 22);
  display.print("S.Batt: ");
  display.print(solarBattV, 2);
  display.println("V");

  display.setCursor(0, 32);
  display.print("M.Batt: ");
  display.print(mainBattV, 2);
  display.println("V");

  display.setCursor(0, 44);
  display.print("Mode: ");
  display.print(mode);

  display.setCursor(0, 56);
  display.print("Fan:");
  display.print(fanOn ? "ON " : "OFF");
  display.print(fanManual ? "[M]" : "");
  display.print(" LED:");
  display.print(ledOn ? "ON" : "OFF");
  display.print(ledManual ? "[M]" : "");

  display.display();

  delay(1000);
}