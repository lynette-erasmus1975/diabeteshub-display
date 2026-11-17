/*
    Wee Glo (DiabetesHub Ireland): Improv Wi-Fi over USB serial.
    See WeeGloImprov.h. This file is part of the DiabetesHub display, GNU GPL v3.
*/
#include <Arduino.h>
#include "M5NSDevice.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "soc/rtc_wdt.h"
#include "M5NSconfig.h"
#include "externs.h"
#include "WeeGloImprov.h"

extern void saveConfigToFlash(tConfig *cfg);

// Improv serial constants (version 1)
static const uint8_t IMP_VERSION = 1;
enum : uint8_t { T_STATE = 0x01, T_ERROR = 0x02, T_RPC = 0x03, T_RPC_RESULT = 0x04 };
enum : uint8_t { ST_READY = 0x02, ST_PROVISIONING = 0x03, ST_PROVISIONED = 0x04 };
enum : uint8_t { E_NONE = 0x00, E_INVALID = 0x01, E_UNKNOWN_RPC = 0x02, E_CONNECT = 0x03, E_UNKNOWN = 0xFF };
enum : uint8_t { C_WIFI = 0x01, C_STATE = 0x02, C_INFO = 0x03, C_SCAN = 0x04 };

static uint8_t buf[256];
static size_t pos = 0;
static bool heardHost = false;          // the install page has spoken to us
static unsigned long lastBeacon = 0;

static void sendPacket(uint8_t type, const uint8_t *data, uint8_t len) {
  uint8_t hdr[9] = {'I','M','P','R','O','V', IMP_VERSION, type, len};
  uint8_t sum = 0;
  for (int i = 0; i < 9; i++) sum += hdr[i];
  for (int i = 0; i < len; i++) sum += data[i];
  // Start on a fresh line: the install page skips any line that doesn't begin with IMPROV,
  // and Wee Glo's own log messages don't always end with a newline.
  Serial.write('\n');
  Serial.write(hdr, 9);
  if (len) Serial.write(data, len);
  Serial.write(sum);
  Serial.write('\n');
  Serial.flush();
}

static void sendState(uint8_t st) { sendPacket(T_STATE, &st, 1); }
static void sendError(uint8_t e) { sendPacket(T_ERROR, &e, 1); }

// RPC result: [command, length of the rest, (len, string)...]
static void sendResult(uint8_t cmd, const char *const *strs, int n) {
  uint8_t out[250];
  int p = 2;
  for (int i = 0; i < n; i++) {
    size_t l = strlen(strs[i]);
    if (l > 120) l = 120;
    if (p + 1 + (int)l > (int)sizeof(out)) break;
    out[p++] = (uint8_t)l;
    memcpy(out + p, strs[i], l);
    p += l;
  }
  out[0] = cmd;
  out[1] = (uint8_t)(p - 2);
  sendPacket(T_RPC_RESULT, out, (uint8_t)p);
}

static bool onHomeWiFi() {
  return !cfg.is_task_bootstrapping && WiFi.status() == WL_CONNECTED;
}

static void sendProvisionedUrl(uint8_t cmd) {
  char url[48];
  snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());
  const char *s[1] = {url};
  sendResult(cmd, s, 1);
}

// Keep the networks Wee Glo already knows; update the password if this one is known,
// otherwise use the first free slot (slot 0 if all ten are taken).
static void rememberNetwork(const char *ssid, const char *pass) {
  int slot = -1;
  for (int i = 0; i < 10; i++) if (strcmp(cfg.wlanssid[i], ssid) == 0) { slot = i; break; }
  if (slot < 0) for (int i = 0; i < 10; i++) if (cfg.wlanssid[i][0] == 0) { slot = i; break; }
  if (slot < 0) slot = 0;
  strlcpy(cfg.wlanssid[slot], ssid, sizeof(cfg.wlanssid[slot]));
  strlcpy(cfg.wlanpass[slot], pass, sizeof(cfg.wlanpass[slot]));
}

static void showOnScreen(const char *line1, const char *line2) {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 90);
  M5.Lcd.println(line1);
  M5.Lcd.setCursor(10, 120);
  M5.Lcd.println(line2);
}

static void handleWiFi(const uint8_t *d, uint8_t len) {
  // d = [ssid_len, ssid..., pass_len, pass...]
  if (len < 2) { sendError(E_INVALID); return; }
  uint8_t sl = d[0];
  if (1 + sl + 1 > len) { sendError(E_INVALID); return; }
  uint8_t pl = d[1 + sl];
  if (1 + sl + 1 + pl > len || sl == 0 || sl > 63 || pl > 63) { sendError(E_INVALID); return; }
  char ssid[64], pass[64];
  memcpy(ssid, d + 1, sl); ssid[sl] = 0;
  memcpy(pass, d + 2 + sl, pl); pass[pl] = 0;

  sendState(ST_PROVISIONING);
  showOnScreen("Joining Wi-Fi...", ssid);

  // Try the network before keeping it, so a mistyped password is caught on the computer.
  if (cfg.is_task_bootstrapping) WiFi.mode(WIFI_AP_STA); else WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, pl ? pass : NULL);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    showOnScreen("Could not join", "Check the password");
    sendError(E_CONNECT);
    sendState(ST_READY);
    if (cfg.is_task_bootstrapping) {
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);   // stay in setup mode, so the family can simply try again
    } else {
      delay(1500);
      ESP.restart();        // back on the Wi-Fi Wee Glo already knows, nothing changed
    }
    return;
  }

  rememberNetwork(ssid, pass);
  cfg.is_task_bootstrapping = 0;
  saveConfigToFlash(&cfg);

  sendState(ST_PROVISIONED);
  sendProvisionedUrl(C_WIFI);
  showOnScreen("Connected to", ssid);
  delay(3000);       // let the install page show "Connected" before the restart
  // A full restart, like unplugging Wee Glo and plugging it back in, so the chime sounds exactly as it does
  // on every other start. A normal software restart (ESP.restart) left part of the chip as it was, and the
  // chime then came out far too loud and distorted. The chip's own timer watchdog resets everything.
  rtc_wdt_protect_off();
  rtc_wdt_disable();
  rtc_wdt_set_stage(RTC_WDT_STAGE0, RTC_WDT_STAGE_ACTION_RESET_RTC);
  rtc_wdt_set_time(RTC_WDT_STAGE0, 200);
  rtc_wdt_enable();
  rtc_wdt_protect_on();
  unsigned long w0 = millis();
  while (millis() - w0 < 3000) delay(10);
  ESP.restart();   // only if the full restart didn't happen
}

static void handleScan() {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    char rssi[8];
    snprintf(rssi, sizeof(rssi), "%d", WiFi.RSSI(i));
    const char *strs[3] = {s.c_str(), rssi, WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"};
    sendResult(C_SCAN, strs, 3);
  }
  WiFi.scanDelete();
  sendResult(C_SCAN, NULL, 0);   // empty result = end of list
}

static void handleRpc(const uint8_t *d, uint8_t len) {
  heardHost = true;
  if (len < 2 || d[1] + 2 > len) { sendError(E_INVALID); return; }
  uint8_t cmd = d[0];
  const uint8_t *payload = d + 2;
  uint8_t plen = d[1];
  switch (cmd) {
    case C_WIFI: handleWiFi(payload, plen); break;
    case C_STATE:
      if (onHomeWiFi()) { sendState(ST_PROVISIONED); sendProvisionedUrl(C_STATE); }
      else sendState(ST_READY);
      break;
    case C_INFO: {
      const char *s[4] = {"Wee Glo", M5NSversion.c_str(), "ESP32", cfg.deviceName};
      sendResult(C_INFO, s, 4);
      break;
    }
    case C_SCAN: handleScan(); break;
    default: sendError(E_UNKNOWN_RPC);
  }
}

/*
 * The install page (ESP Web Tools 10.1) asks "are you there?" only once, about a second after
 * the install, while Wee Glo is still starting up, so the question is lost. It does accept a
 * state message it didn't ask for, then goes on to ask for the Wi-Fi as normal. So for the
 * first 30 seconds after starting, until the page speaks to us, Wee Glo says so every second:
 * "ready" in setup mode, or "on Wi-Fi" with its address once it has joined the home Wi-Fi.
 */
void improvBeacon() {
  if (heardHost || millis() > 30000 || millis() - lastBeacon < 1000) return;
  if (cfg.is_task_bootstrapping) {
    lastBeacon = millis();
    sendState(ST_READY);
  } else if (onHomeWiFi()) {
    lastBeacon = millis();
    sendState(ST_PROVISIONED);
    sendProvisionedUrl(C_STATE);
  }
}

void improvLoop() {
  improvBeacon();
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (pos < 6) {                       // match the "IMPROV" header byte by byte
      if (b == (uint8_t)"IMPROV"[pos]) buf[pos++] = b;
      else pos = (b == 'I') ? (buf[0] = b, 1) : 0;
      continue;
    }
    buf[pos++] = b;
    if (pos >= 9) {
      uint8_t len = buf[8];
      if (pos == (size_t)(9 + len + 1)) {    // header, version, type, length, data, checksum
        uint8_t sum = 0;
        for (size_t i = 0; i < pos - 1; i++) sum += buf[i];
        uint8_t type = buf[7];
        if (buf[6] == IMP_VERSION && sum == buf[pos - 1] && type == T_RPC) handleRpc(buf + 9, len);
        else if (sum != buf[pos - 1]) sendError(E_INVALID);
        pos = 0;
      }
    }
    if (pos >= sizeof(buf)) pos = 0;
  }
}
