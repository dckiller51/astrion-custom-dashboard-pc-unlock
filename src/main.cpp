// Astrion PC Unlock
//
// An ESP32-S3 plugged into the PC as a USB keyboard. When the Astrion remote asks (over the
// local network, with a 6-digit code), it shows the Windows sign-in field and types the PIN,
// which is kept on the ESP only: it never travels over the network and is in no log.
//
//   GET  /         status page
//   GET  /info     {"app":"astrion-unlock","version","name","pin","usb"}            no code
//   POST /check    the code is right (nothing typed)                              X-Astrion-Code
//   POST /wake     wakes the PC from sleep through USB, when it allows it         X-Astrion-Code
//   POST /unlock   wakes it if asleep, shows the sign-in field, types the PIN     X-Astrion-Code
//   GET/POST /setup  settings: only in setup mode (first start), or for 3 minutes after a
//                  press on the button
//
// Button: a press opens the settings for 3 minutes; held 10 s, everything is erased (setup mode).
// LED: purple = settings open, blue = joining the Wi-Fi, green flash = PIN typed, red = refused.

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "USB.h"
#include "USBHIDKeyboard.h"

extern "C" bool tud_mounted(void);
extern "C" bool tud_suspended(void);
extern "C" bool tud_remote_wakeup(void);

#if ARDUINO_USB_MODE != 0
#error "Build with -DARDUINO_USB_MODE=0: the native USB port must be the keyboard (TinyUSB), see platformio.ini"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif
#ifndef LED_PIN
#define LED_PIN -1
#endif
#ifndef BUTTON_PIN
#define BUTTON_PIN -1
#endif

static const char *CODE_HEADER = "X-Astrion-Code";
static const uint32_t SETUP_WINDOW_MS = 3UL * 60UL * 1000UL;
static const uint32_t RESET_HOLD_MS = 10000;
static const int MAX_FAILURES = 5;
static const uint32_t LOCKOUT_MS = 60000;
static const int MAX_PIN = 32;

USBHIDKeyboard keyboard;
WebServer server(80);
Preferences prefs;

struct Settings {
  String name;
  String ssid;
  String pass;
  String pin;
  String code;
  String layout;  // "qwerty", "azerty", "qwertz", "numpad"
  bool enter;     // Enter after the PIN (a PIN with letters, or a password)
  uint32_t revealMs;  // after the key that shows the sign-in field
} cfg;

bool setupMode = false;        // access point, first start or after a reset
uint32_t setupUntil = 0;       // settings open (station mode) until then
int failures = 0;
uint32_t lockedUntil = 0;
uint32_t ledUntil = 0;

// ---- LED -------------------------------------------------------------------------------

void led(uint8_t r, uint8_t g, uint8_t b) {
#if LED_PIN >= 0
  rgbLedWrite(LED_PIN, r, g, b);
#endif
}

void flash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  led(r, g, b);
  ledUntil = millis() + ms;
}

bool settingsOpen() { return setupMode || (setupUntil && (int32_t)(setupUntil - millis()) > 0); }

void idleLed() {
  if (ledUntil && (int32_t)(ledUntil - millis()) > 0) return;
  ledUntil = 0;
  if (settingsOpen()) {
    led(20, 0, 20);
  } else if (WiFi.status() != WL_CONNECTED) {
    led(0, 0, 20);
  } else {
    led(0, 0, 0);  // dark in normal use: it sits in the PC
  }
}

// ---- settings --------------------------------------------------------------------------

String randomCode() {
  char buf[7];
  snprintf(buf, sizeof(buf), "%06lu", (unsigned long)(esp_random() % 1000000UL));
  return String(buf);
}

void loadSettings() {
  prefs.begin("unlock", true);
  cfg.name = prefs.getString("name", "Gaming PC");
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.pin = prefs.getString("pin", "");
  cfg.code = prefs.getString("code", "");
  cfg.layout = prefs.getString("layout", "azerty");
  cfg.enter = prefs.getBool("enter", false);
  cfg.revealMs = prefs.getUInt("reveal", 1500);
  prefs.end();
  if (cfg.code.length() != 6) cfg.code = randomCode();
}

void saveSettings() {
  prefs.begin("unlock", false);
  prefs.putString("name", cfg.name);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("pin", cfg.pin);
  prefs.putString("code", cfg.code);
  prefs.putString("layout", cfg.layout);
  prefs.putBool("enter", cfg.enter);
  prefs.putUInt("reveal", cfg.revealMs);
  prefs.end();
}

void eraseSettings() {
  prefs.begin("unlock", false);
  prefs.clear();
  prefs.end();
}

// ---- keyboard --------------------------------------------------------------------------

const uint8_t KEY_SHIFT = 0xE1;  // left shift, as a raw HID usage
const uint8_t KEY_CTRL = 0xE0;   // left ctrl
const uint8_t USAGE_ENTER = 0x28;

void tap(uint8_t usage, bool shift) {
  if (shift) keyboard.pressRaw(KEY_SHIFT);
  keyboard.pressRaw(usage);
  delay(40);
  keyboard.releaseAll();
  delay(70);
}

/** One PIN character as the key (HID usage) and Shift the chosen keyboard layout needs. */
bool keyFor(char c, uint8_t &usage, bool &shift) {
  const String &l = cfg.layout;
  shift = false;
  if (c >= '0' && c <= '9') {
    int n = c == '0' ? 9 : c - '1';  // 1..9 then 0
    if (l == "numpad") {
      usage = 0x59 + n;  // keypad 1..9, 0 (Num Lock on)
    } else {
      usage = 0x1E + n;  // top row
      shift = l == "azerty";  // AZERTY: the digits are shifted
    }
    return true;
  }
  char lower = (char)tolower(c);
  if (lower < 'a' || lower > 'z') return false;
  shift = c != lower;
  char key = lower;  // the US key that gives this letter
  if (l == "azerty") {
    if (lower == 'a') key = 'q';
    else if (lower == 'q') key = 'a';
    else if (lower == 'z') key = 'w';
    else if (lower == 'w') key = 'z';
    else if (lower == 'm') { usage = 0x33; return true; }  // where US has ';'
  } else if (l == "qwertz") {
    if (lower == 'y') key = 'z';
    else if (lower == 'z') key = 'y';
  }
  usage = 0x04 + (key - 'a');
  return true;
}

String usbState() {
  if (!tud_mounted()) return "off";
  return tud_suspended() ? "suspended" : "active";
}

/** Asks the PC to wake from sleep, when the USB host allows this keyboard to. */
bool wakeHost() {
  if (!tud_suspended()) return false;
  return tud_remote_wakeup();
}

// ---- HTTP ------------------------------------------------------------------------------

String jsonEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

String htmlEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

void sendJson(int status, const String &body) { server.send(status, "application/json", body); }

void sendError(int status, const String &message) { sendJson(status, "{\"error\":\"" + jsonEscape(message) + "\"}"); }

/** The code in the request is right; answers the error itself when not. */
bool authorized() {
  if ((int32_t)(lockedUntil - millis()) > 0) {
    sendError(429, "too many wrong codes: try again in a minute");
    return false;
  }
  String given = server.header(CODE_HEADER);
  // Same time whatever the code: compare every character.
  bool ok = given.length() == cfg.code.length() && cfg.code.length() == 6;
  uint8_t diff = 0;
  for (size_t i = 0; i < 6 && i < given.length(); i++) diff |= given[i] ^ cfg.code[i];
  ok = ok && diff == 0;
  if (ok) {
    failures = 0;
    return true;
  }
  if (++failures >= MAX_FAILURES) {
    failures = 0;
    lockedUntil = millis() + LOCKOUT_MS;
  }
  flash(40, 0, 0, 800);
  sendError(401, "wrong code");
  return false;
}

void handleInfo() {
  sendJson(200, String("{\"app\":\"astrion-unlock\",\"version\":\"") + FIRMWARE_VERSION + "\",\"name\":\"" + jsonEscape(cfg.name) +
                    "\",\"pin\":" + (cfg.pin.length() ? "true" : "false") + ",\"usb\":\"" + usbState() + "\"}");
}

void handleCheck() {
  if (!authorized()) return;
  sendJson(200, String("{\"ok\":true,\"name\":\"") + jsonEscape(cfg.name) + "\",\"pin\":" + (cfg.pin.length() ? "true" : "false") +
                    ",\"usb\":\"" + usbState() + "\"}");
}

void handleWake() {
  if (!authorized()) return;
  String usb = usbState();
  if (usb == "off") return sendError(409, "the PC isn't on (its USB port is off)");
  bool asked = wakeHost();
  sendJson(200, String("{\"ok\":true,\"usb\":\"") + usb + "\",\"woken\":" + (asked ? "true" : "false") + "}");
}

void handleUnlock() {
  if (!authorized()) return;
  if (cfg.pin.length() == 0) return sendError(409, "no PIN saved on the ESP: press its button, then open its page");
  if (usbState() == "off") return sendError(409, "the PC isn't on (its USB port is off)");
  if (tud_suspended()) {
    // Asleep: wake it first, then give Windows the time to show its lock screen.
    if (!wakeHost()) return sendError(409, "the PC is asleep and doesn't let this keyboard wake it");
    uint32_t until = millis() + 8000;
    while (tud_suspended() && (int32_t)(until - millis()) > 0) delay(50);
    if (tud_suspended()) return sendError(409, "the PC didn't wake");
    delay(3000);
  }
  // Ctrl shows the sign-in field without typing anything, then the PIN.
  tap(KEY_CTRL, false);
  delay(cfg.revealMs);
  for (size_t i = 0; i < cfg.pin.length(); i++) {
    uint8_t usage;
    bool shift;
    if (keyFor(cfg.pin[i], usage, shift)) tap(usage, shift);
  }
  if (cfg.enter) tap(USAGE_ENTER, false);
  flash(0, 40, 0, 1500);
  sendJson(200, "{\"ok\":true}");
}

static const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Astrion PC Unlock</title><style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px}main{max-width:460px;margin:auto}
h1{font-size:20px}label{display:block;margin:14px 0 4px;color:#aaa;font-size:14px}
input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #444;background:#1c1c1c;color:#eee;font-size:16px}
.row{display:flex;gap:8px;align-items:center;margin-top:14px}.row input{width:auto}
button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;background:#00b8d4;color:#000;font-size:16px;font-weight:600}
.hint{color:#999;font-size:13px}.code{font-size:28px;letter-spacing:6px;font-weight:700;color:#00e5ff}.box{background:#1c1c1c;border-radius:10px;padding:14px;margin-top:14px}
</style></head><body><main><h1>Astrion PC Unlock</h1>)";

void sendPage(const String &body) { server.send(200, "text/html", String(FPSTR(PAGE_HEAD)) + body + "</main></body></html>"); }

void handleRoot() {
  String b = "<div class='box'><b>" + htmlEscape(cfg.name) + "</b><br><span class='hint'>Version " FIRMWARE_VERSION "</span>";
  b += "<p>Wi-Fi: " + (WiFi.status() == WL_CONNECTED ? htmlEscape(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)" : String("not connected"));
  b += "<br>PC (USB): " + usbState() + "<br>PIN: " + String(cfg.pin.length() ? "saved" : "<b>not set</b>") + "</p></div>";
  if (settingsOpen()) {
    b += "<p><a style='color:#00e5ff' href='/setup'>Settings</a></p>";
  } else {
    b += "<p class='hint'>To change the settings, press the button on the ESP, then reload this page (open for 3 minutes).</p>";
  }
  sendPage(b);
}

String option(const char *value, const char *label) {
  return String("<option value='") + value + "'" + (cfg.layout == value ? " selected" : "") + ">" + label + "</option>";
}

void handleSetupForm() {
  if (!settingsOpen()) {
    server.send(403, "text/html", String(FPSTR(PAGE_HEAD)) + "<p>Settings closed: press the button on the ESP, then reload this page.</p></main></body></html>");
    return;
  }
  String b = "<form method='post' action='/setup'>";
  b += "<label>Name</label><input name='name' maxlength='32' value='" + htmlEscape(cfg.name) + "'>";
  b += "<label>Wi-Fi network</label><input name='ssid' maxlength='32' value='" + htmlEscape(cfg.ssid) + "'>";
  b += "<label>Wi-Fi password</label><input name='pass' type='password' maxlength='64' placeholder='" +
       String(cfg.pass.length() ? "unchanged" : "") + "'>";
  b += "<label>Windows PIN</label><input name='pin' type='password' maxlength='32' autocomplete='off' placeholder='" +
       String(cfg.pin.length() ? "unchanged" : "the PIN you type to sign in") + "'>";
  b += "<p class='hint'>Kept on this ESP only. Digits and letters (A–Z).</p>";
  b += "<label>PC keyboard layout</label><select name='layout'>" + option("azerty", "AZERTY (French, Belgian)") +
       option("qwerty", "QWERTY (US, UK, Italian, Spanish…)") + option("qwertz", "QWERTZ (German, Swiss)") +
       option("numpad", "Numeric keypad (digits only, Num Lock on)") + "</select>";
  b += "<div class='row'><input type='checkbox' name='enter' value='1'" + String(cfg.enter ? " checked" : "") +
       "><span>Press Enter after the PIN (a PIN with letters)</span></div>";
  b += "<label>Wait before typing (ms)</label><input name='reveal' type='number' min='300' max='10000' value='" + String(cfg.revealMs) + "'>";
  b += "<p class='hint'>Time for Windows to show the PIN field after the first key.</p>";
  b += "<label>Code for the remote</label><input name='code' inputmode='numeric' pattern='[0-9]{6}' maxlength='6' value='" + cfg.code + "'>";
  b += "<p class='hint'>Type it in Astrion (web page › your PC › 🔓). 6 digits.</p>";
  b += "<button type='submit'>Save</button></form>";
  sendPage(b);
}

void handleSetupSave() {
  if (!settingsOpen()) return sendError(403, "settings closed: press the button on the ESP");
  String name = server.arg("name");
  name.trim();
  if (name.length()) cfg.name = name.substring(0, 32);
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length()) cfg.ssid = ssid.substring(0, 32);
  if (server.arg("pass").length()) cfg.pass = server.arg("pass").substring(0, 64);
  String pin = server.arg("pin");
  if (pin.length()) {
    for (size_t i = 0; i < pin.length(); i++) {
      if (!isalnum((unsigned char)pin[i])) return sendPage("<p>The PIN may hold digits and letters only.</p><p><a style='color:#00e5ff' href='/setup'>Back</a></p>");
    }
    cfg.pin = pin.substring(0, MAX_PIN);
  }
  String layout = server.arg("layout");
  if (layout == "azerty" || layout == "qwerty" || layout == "qwertz" || layout == "numpad") cfg.layout = layout;
  cfg.enter = server.arg("enter") == "1";
  long reveal = server.arg("reveal").toInt();
  if (reveal >= 300 && reveal <= 10000) cfg.revealMs = (uint32_t)reveal;
  String code = server.arg("code");
  code.trim();
  bool codeOk = code.length() == 6;
  for (size_t i = 0; codeOk && i < 6; i++) codeOk = isdigit((unsigned char)code[i]);
  if (codeOk) cfg.code = code;
  saveSettings();
  String b = "<div class='box'>Saved. The code for the remote:<div class='code'>" + cfg.code + "</div></div>";
  if (cfg.ssid.length() == 0) {
    b += "<p>No Wi-Fi network yet: the ESP stays in setup mode.</p>";
    sendPage(b);
    return;
  }
  b += "<p>The ESP restarts and joins <b>" + htmlEscape(cfg.ssid) + "</b>. Its address is in your router's list of devices "
       "(name <b>astrion-unlock</b>).</p>";
  sendPage(b);
  delay(1500);
  ESP.restart();
}

// ---- Wi-Fi -----------------------------------------------------------------------------

void startSetupMode() {
  setupMode = true;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Astrion-Unlock-%02X%02X", mac[4], mac[5]);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid);
}

void startStation() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("astrion-unlock");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
}

// ---- button ----------------------------------------------------------------------------

void pollButton() {
#if BUTTON_PIN >= 0
  static uint32_t pressedAt = 0;
  static bool resetShown = false;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && !pressedAt) {
    pressedAt = millis();
    resetShown = false;
  } else if (down && !resetShown && millis() - pressedAt > RESET_HOLD_MS) {
    resetShown = true;
    led(40, 0, 0);
    eraseSettings();
    delay(1000);
    ESP.restart();
  } else if (!down && pressedAt) {
    if (millis() - pressedAt > 50) setupUntil = millis() + SETUP_WINDOW_MS;  // a press: settings open
    pressedAt = 0;
  }
#endif
}

// ---- main ------------------------------------------------------------------------------

void setup() {
#if BUTTON_PIN >= 0
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif
  led(0, 0, 20);
  USB.productName("Astrion PC Unlock");
  USB.manufacturerName("Astrion Custom Dashboard");
  keyboard.begin();
  USB.begin();

  loadSettings();
  if (cfg.ssid.length() == 0) {
    saveSettings();  // keeps the code drawn on the first start
    startSetupMode();
  } else {
    startStation();
  }

  const char *headers[] = {CODE_HEADER};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/check", HTTP_POST, handleCheck);
  server.on("/wake", HTTP_POST, handleWake);
  server.on("/unlock", HTTP_POST, handleUnlock);
  server.on("/setup", HTTP_GET, handleSetupForm);
  server.on("/setup", HTTP_POST, handleSetupSave);
  server.onNotFound([]() { sendError(404, "not found"); });
  server.begin();
}

/** Never joined the Wi-Fi 90 s after starting (a wrong password?): the setup network comes back too. */
void fallBackToSetup() {
  static bool everConnected = false;
  if (WiFi.status() == WL_CONNECTED) everConnected = true;
  if (setupMode || everConnected || millis() < 90000) return;
  setupMode = true;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Astrion-Unlock-%02X%02X", mac[4], mac[5]);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid);
}

void loop() {
  server.handleClient();
  fallBackToSetup();
  pollButton();
  idleLed();
  delay(2);
}
