/*  M5Stack Nightscout monitor - Medtronic CareLink follower data source

    Reads sensor glucose straight from Medtronic CareLink with a care partner account,
    so Medtronic users (MiniMed 780G, Simplera) need neither a Nightscout site nor an
    Android phone running xDrip+ as a bridge.

    Protocol follows the CareLink Connect ("care partner") app as implemented by the
    open-source clients NightscoutFoundation/xDrip (cgm/carelinkfollow, GPL v3) and
    ondrej1024/carelink-python-client (MIT):
      1. discovery JSON  -> region endpoints + sign-in (Auth0) configuration
      2. one-time browser sign-in -> authorization code -> access + refresh token
      3. refresh token   -> new access token whenever it expires (refresh token rotates,
                            so every new one is saved to flash straight away)
      4. users/me, links/patients -> role and followed patient
      5. POST display/message     -> current glucose, trend, last 24 h, active insulin

    CareLink access is unofficial. Medtronic can change it at any time, so every failure
    is reported on screen and in the error log rather than retried silently forever.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <strings.h>
#include <mbedtls/base64.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include "M5NSCareLink.h"
#include "M5NSCareLinkParse.h"
#include "M5NSDexcom.h" // reuses directionToArrowAngle()
#include "externs.h"

#define CL_DISCOVERY_URL "https://clcloud.minimed.eu/connect/carepartner/v13/discover/android/3.8"
#define CL_APP_VERSION   "3.8.0"
#define CL_USER_AGENT    "Dalvik/2.1.0 (Linux; U; Android 10; Nexus 5X Build/QQ3A.200805.001)"
#define CL_PREFS         "carelink"
#define CL_EARLY_REFRESH_S 300 // refresh the access token this long before it expires

// Looked-up CareLink configuration for the configured country.
static bool clcLoaded = false;
static String clcCountry, clcRegion, clcCumulus, clcCareLinkBase;
static String clcSsoBase, clcAuthPath, clcTokenPath;
static String clcClientId, clcScope, clcRedirect, clcAudience;

// Login, kept in flash (namespace "carelink") so it survives restarts.
static bool clCredsLoaded = false;
static String clRefresh, clClientId, clClientSecret;

// Short-lived access token, memory only.
static String clAccess;
static uint32_t clAccessSetMs = 0;
static uint32_t clAccessLifeMs = 0;

// Who we follow.
static bool clSessionLoaded = false;
static String clUsername, clRole, clPatientId, clPatientName;

static bool clNeedsSignIn = false;
static String clLastError;

// ---------------------------------------------------------------------------------
// helpers

static String urlEncode(const String &s) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[((uint8_t)c) >> 4];
      out += hex[((uint8_t)c) & 15];
    }
  }
  return out;
}

static void setError(int code, const String &text) {
  clLastError = text;
  Serial.printf("[CareLink] %d %s\r\n", code, text.c_str());
  addErrorLog(code);
}

static void loadCreds() {
  if (clCredsLoaded) return;
  Preferences p;
  if (p.begin(CL_PREFS, true)) {
    clRefresh = p.getString("rt", "");
    clClientId = p.getString("cid", "");
    clClientSecret = p.getString("cs", "");
    p.end();
  }
  clCredsLoaded = true;
}

static void saveCreds() {
  Preferences p;
  if (!p.begin(CL_PREFS, false)) {
    Serial.println("[CareLink] cannot open flash storage for the login");
    return;
  }
  p.putString("rt", clRefresh);
  p.putString("cid", clClientId);
  p.putString("cs", clClientSecret);
  p.end();
}

static bool accessValid() {
  return clAccess.length() > 0 && (uint32_t)(millis() - clAccessSetMs) < clAccessLifeMs;
}

static void setAccess(const char *token, long expiresIn) {
  clAccess = token;
  clAccessSetMs = millis();
  long life = expiresIn > 0 ? expiresIn : 3600;
  life -= CL_EARLY_REFRESH_S;
  if (life < 60) life = 60;
  clAccessLifeMs = (uint32_t)life * 1000UL;
}

// GET/POST returning the whole body; for the small JSON replies (config, token, user).
static int httpText(WiFiClientSecure &client, const char *method, const String &url,
                    const String &body, const char *contentType, bool auth, String &resp) {
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -1;
  http.addHeader("User-Agent", CL_USER_AGENT);
  http.addHeader("Accept", "application/json");
  if (contentType) http.addHeader("Content-Type", contentType);
  if (auth) http.addHeader("Authorization", "Bearer " + clAccess);
  int code = (strcmp(method, "POST") == 0) ? http.POST(body) : http.GET();
  resp = (code > 0) ? http.getString() : String();
  http.end();
  return code;
}

// ---------------------------------------------------------------------------------
// 1. configuration (discovery + Auth0 sign-in settings)

static int loadConfig(WiFiClientSecure &client, tConfig *cfg) {
  String country = String(cfg->cl_country);
  country.trim();
  country.toUpperCase();
  if (country.length() != 2) {
    setError(1304, "Set the CareLink country (two letters, e.g. IE) on the setup page");
    return 1304;
  }
  if (clcLoaded && country == clcCountry) return 0;
  clcLoaded = false;

  // Discovery: keep only each country's region and the per-region endpoint objects.
  HTTPClient http;
  http.setTimeout(15000);
  http.useHTTP10(true); // body is parsed straight off the stream, so no chunked framing
  http.begin(client, CL_DISCOVERY_URL);
  http.addHeader("User-Agent", CL_USER_AGENT);
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  Serial.printf("[CareLink] discovery code=%d\r\n", code);
  if (code != HTTP_CODE_OK) {
    http.end();
    setError(code > 0 ? code : 1301, "CareLink configuration not reachable");
    return code > 0 ? code : 1301;
  }
  JsonDocument filter;
  filter["supportedCountries"][0]["*"]["region"] = true;
  filter["CP"][0]["*"] = true;
  JsonDocument disco;
  DeserializationError jerr = deserializeJson(disco, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (jerr) {
    setError(1304, String("CareLink configuration unreadable: ") + jerr.c_str());
    return 1304;
  }

  String region;
  for (JsonObject c : disco["supportedCountries"].as<JsonArray>()) {
    const char *r = c[country]["region"] | (const char *)nullptr;
    if (r) { region = r; break; }
  }
  if (region.length() == 0) {
    setError(1304, "Country " + country + " is not supported by CareLink");
    return 1304;
  }

  JsonObject cp;
  for (JsonObject c : disco["CP"].as<JsonArray>()) {
    if (region.equals(c["region"] | "")) { cp = c; break; }
  }
  if (cp.isNull()) {
    setError(1304, "No CareLink endpoints for region " + region);
    return 1304;
  }
  const char *ssoKey = cp["UseSSOConfiguration"] | "SSOConfiguration";
  String ssoUrl = cp[ssoKey] | "";
  if (ssoUrl.length() == 0) ssoUrl = cp["SSOConfiguration"] | "";
  clcCumulus = cp["baseUrlCumulus"] | "";
  clcCareLinkBase = cp["baseUrlCareLink"] | "";
  disco.clear();
  if (ssoUrl.length() == 0 || clcCumulus.length() == 0 || clcCareLinkBase.length() == 0) {
    setError(1304, "CareLink configuration incomplete");
    return 1304;
  }

  // Sign-in (Auth0) configuration.
  String resp;
  code = httpText(client, "GET", ssoUrl, "", nullptr, false, resp);
  Serial.printf("[CareLink] sign-in config code=%d\r\n", code);
  if (code != HTTP_CODE_OK) {
    setError(code > 0 ? code : 1301, "CareLink sign-in configuration not reachable");
    return code > 0 ? code : 1301;
  }
  JsonDocument sso;
  jerr = deserializeJson(sso, resp);
  if (jerr) {
    setError(1304, String("CareLink sign-in configuration unreadable: ") + jerr.c_str());
    return 1304;
  }
  String host = sso["server"]["hostname"] | "";
  int port = sso["server"]["port"] | 443;
  String prefix = sso["server"]["prefix"] | "";
  while (prefix.startsWith("/")) prefix.remove(0, 1);
  while (prefix.endsWith("/")) prefix.remove(prefix.length() - 1);
  clcSsoBase = "https://" + host + (port != 443 ? ":" + String(port) : String("")) +
               (prefix.length() ? "/" + prefix : String(""));
  clcAuthPath = sso["system_endpoints"]["authorization_endpoint_path"] | "";
  clcTokenPath = sso["system_endpoints"]["token_endpoint_path"] | "";
  clcClientId = sso["client"]["client_id"] | "";
  clcScope = sso["client"]["scope"] | "";
  clcRedirect = sso["client"]["redirect_uri"] | "";
  clcAudience = sso["client"]["audience"] | "";
  if (host.length() == 0 || clcTokenPath.length() == 0 || clcClientId.length() == 0) {
    setError(1304, "CareLink sign-in configuration incomplete");
    return 1304;
  }
  if (!clcAuthPath.startsWith("/")) clcAuthPath = "/" + clcAuthPath;
  if (!clcTokenPath.startsWith("/")) clcTokenPath = "/" + clcTokenPath;

  clcRegion = region;
  clcCountry = country;
  clcLoaded = true;
  Serial.printf("[CareLink] country %s, region %s\r\n", country.c_str(), region.c_str());
  return 0;
}

// The classic CareLink web host, used as a fallback for user/patient lookups.
static String careLinkWebHost() {
  return clcRegion.equalsIgnoreCase("US") ? "https://carelink.minimed.com" : "https://carelink.minimed.eu";
}

// ---------------------------------------------------------------------------------
// 2./3. tokens

// Stores tokens from a token-endpoint reply. Returns false if there is no access token.
static bool takeTokens(const String &resp, const String &clientId) {
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  const char *at = doc["access_token"] | "";
  if (strlen(at) == 0) return false;
  setAccess(at, doc["expires_in"] | 3600L);
  const char *rt = doc["refresh_token"] | "";
  bool changed = false;
  if (strlen(rt) > 0 && !clRefresh.equals(rt)) { clRefresh = rt; changed = true; }
  if (clientId.length() && !clClientId.equals(clientId)) { clClientId = clientId; changed = true; }
  if (changed) saveCreds(); // rotated refresh token: the old one stops working, save now
  clNeedsSignIn = false;
  return true;
}

static int refreshAccess(WiFiClientSecure &client) {
  loadCreds();
  if (clRefresh.length() == 0) {
    clNeedsSignIn = true;
    setError(1302, "CareLink sign-in needed");
    return 1302;
  }
  String clientId = clClientId.length() ? clClientId : clcClientId;
  String body = "client_id=" + urlEncode(clientId) +
                "&refresh_token=" + urlEncode(clRefresh) +
                "&grant_type=refresh_token";
  if (clClientSecret.length()) body += "&client_secret=" + urlEncode(clClientSecret);
  String resp;
  int code = httpText(client, "POST", clcSsoBase + clcTokenPath, body,
                      "application/x-www-form-urlencoded; charset=UTF-8", false, resp);
  Serial.printf("[CareLink] token refresh code=%d\r\n", code);
  if (code == HTTP_CODE_OK && takeTokens(resp, clientId)) return 0;
  if (code == 400 || code == 401 || code == 403) {
    // invalid_grant: the refresh token expired or was revoked (for example because
    // the same care partner account signed in somewhere else).
    clNeedsSignIn = true;
    clAccess = "";
    setError(1302, "CareLink login expired, sign in again on the setup page");
    return 1302;
  }
  setError(code > 0 ? code : 1301, "CareLink token refresh failed");
  return code > 0 ? code : 1301;
}

static int ensureAccess(WiFiClientSecure &client) {
  if (accessValid()) return 0;
  return refreshAccess(client);
}

// ---------------------------------------------------------------------------------
// 4. who are we, who do we follow

// preferred_username from the access token (a JWT), if present.
static String usernameFromToken(const String &jwt) {
  int a = jwt.indexOf('.');
  int b = jwt.indexOf('.', a + 1);
  if (a < 0 || b < 0) return "";
  String p = jwt.substring(a + 1, b);
  p.replace('-', '+');
  p.replace('_', '/');
  while (p.length() % 4) p += '=';
  size_t outLen = 0;
  size_t cap = p.length() * 3 / 4 + 4;
  unsigned char *buf = (unsigned char *)malloc(cap + 1);
  if (!buf) return "";
  if (mbedtls_base64_decode(buf, cap, &outLen, (const unsigned char *)p.c_str(), p.length()) != 0) {
    free(buf);
    return "";
  }
  buf[outLen] = 0;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char *)buf, outLen);
  free(buf);
  if (err) return "";
  const char *u = doc["token_details"]["preferred_username"] | (const char *)nullptr;
  if (!u) u = doc["preferred_username"] | (const char *)nullptr;
  return u ? String(u) : String("");
}

static int loadSession(WiFiClientSecure &client, tConfig *cfg) {
  if (clSessionLoaded) return 0;
  String resp;
  JsonDocument doc;

  clUsername = usernameFromToken(clAccess);
  if (clUsername.length() == 0) {
    int code = httpText(client, "GET", careLinkWebHost() + "/patient/users/me/profile", "", nullptr, true, resp);
    if (code == HTTP_CODE_OK && !deserializeJson(doc, resp))
      clUsername = doc["username"] | "";
    if (code == 401 || code == 403) { clAccess = ""; return -401; }
  }

  int code = httpText(client, "GET", clcCareLinkBase + "/users/me", "", nullptr, true, resp);
  if (code != HTTP_CODE_OK)
    code = httpText(client, "GET", careLinkWebHost() + "/patient/users/me", "", nullptr, true, resp);
  Serial.printf("[CareLink] users/me code=%d\r\n", code);
  if (code == 401 || code == 403) { clAccess = ""; return -401; }
  if (code != HTTP_CODE_OK || deserializeJson(doc, resp)) {
    setError(code > 0 ? code : 1301, "CareLink user lookup failed");
    return code > 0 ? code : 1301;
  }
  String role = doc["role"] | "";
  bool carePartner = role.equals("CARE_PARTNER") || role.equals("CARE_PARTNER_OUS");
  clRole = carePartner ? "carepartner" : "patient";
  clPatientId = "";
  clPatientName = "";

  if (carePartner) {
    code = httpText(client, "GET", clcCareLinkBase + "/links/patients", "", nullptr, true, resp);
    if (code != HTTP_CODE_OK)
      code = httpText(client, "GET", careLinkWebHost() + "/patient/m2m/links/patients", "", nullptr, true, resp);
    Serial.printf("[CareLink] patients code=%d\r\n", code);
    if (code == 401 || code == 403) { clAccess = ""; return -401; }
    if (code != HTTP_CODE_OK || deserializeJson(doc, resp)) {
      setError(code > 0 ? code : 1301, "CareLink patient lookup failed");
      return code > 0 ? code : 1301;
    }
    String wanted = String(cfg->cl_patient);
    wanted.trim();
    for (JsonObject pt : doc.as<JsonArray>()) {
      String u = pt["username"] | "";
      if (u.length() == 0) continue;
      if (wanted.length() == 0 || wanted.equalsIgnoreCase(u)) {
        clPatientId = u;
        clPatientName = pt["firstName"] | "";
        break;
      }
    }
    if (clPatientId.length() == 0) {
      setError(1303, wanted.length() ? "Patient " + wanted + " is not linked to this care partner account"
                                     : String("No patient is linked to this care partner account"));
      return 1303;
    }
  }
  if (clUsername.length() == 0) {
    setError(1301, "CareLink username not found");
    return 1301;
  }
  clSessionLoaded = true;
  return 0;
}

// ---------------------------------------------------------------------------------
// 5. data

int readCareLink(tConfig *cfg, struct NSinfo *ns) {
  if (WiFiMultiple.run() != WL_CONNECTED) {
    ESP.restart();
  }
  loadCreds();
  if (clNeedsSignIn || clRefresh.length() == 0) {
    // Logged once when the login was rejected; don't fill the log on every poll.
    if (!clNeedsSignIn) addErrorLog(1302);
    clNeedsSignIn = true;
    if (clLastError.length() == 0) clLastError = "CareLink sign-in needed";
    Serial.println("[CareLink] Skipping fetch, sign-in needed");
    return 1302;
  }

  WiFiClientSecure client;
  client.setInsecure(); // same as the Dexcom/LibreLinkUp sources; no CA bundle on the device

  int err = loadConfig(client, cfg);
  if (err) return err;

  for (int attempt = 0; attempt < 2; attempt++) {
    err = ensureAccess(client);
    if (err) return err;
    err = loadSession(client, cfg);
    if (err == -401) continue; // access token rejected, refresh and try again
    if (err) return err;

    JsonDocument body;
    body["username"] = clUsername;
    body["role"] = clRole;
    if (clPatientId.length()) body["patientId"] = clPatientId;
    body["appVersion"] = CL_APP_VERSION;
    String bodyStr;
    serializeJson(body, bodyStr);

    HTTPClient http;
    http.setTimeout(20000);
    http.useHTTP10(true); // parsed straight off the stream, so no chunked framing
    http.begin(client, clcCumulus + "/display/message");
    http.addHeader("User-Agent", CL_USER_AGENT);
    http.addHeader("Accept", "application/json");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + clAccess);
    int code = http.POST(bodyStr);
    Serial.printf("[CareLink] display message code=%d\r\n", code);
    if (code == 401 || code == 403) {
      http.end();
      clAccess = "";
      continue;
    }
    if (code != HTTP_CODE_OK) {
      http.end();
      setError(code > 0 ? code : 1301, "CareLink data request failed");
      return code > 0 ? code : 1301;
    }

    // The reply carries 24 h of readings, markers and notifications; keep only what the
    // display uses so it fits comfortably in RAM (the Core2 build runs with PSRAM off).
    JsonDocument filter;
    JsonObject f = filter["patientData"].to<JsonObject>();
    f["lastSG"]["sg"] = true;
    f["lastSG"]["timestamp"] = true;
    f["lastSG"]["datetime"] = true;
    f["lastSGTrend"] = true;
    f["sgs"][0]["sg"] = true;
    f["sgs"][0]["timestamp"] = true;
    f["sgs"][0]["datetime"] = true;
    f["activeInsulin"]["amount"] = true;
    f["lastConduitDateTime"] = true;
    f["lastConduitUpdateServerDateTime"] = true;
    f["sensorState"] = true;
    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (jerr) {
      setError(1001, String("CareLink data unreadable: ") + jerr.c_str());
      return 1001;
    }

    carelink::Reading r;
    err = carelink::parsePatientData(doc["patientData"].as<JsonObjectConst>(), &r);
    if (err) {
      clLastError = String("No current reading") + (strlen(r.sensorState) ? String(" (") + r.sensorState + ")" : String(""));
      addErrorLog(err);
      return err;
    }
    if (r.timeShiftHours)
      Serial.printf("[CareLink] corrected pump time zone by %d h\r\n", r.timeShiftHours);

    strlcpy(ns->sensDev, "CareLink", 64);
    ns->is_xDrip = false;
    ns->sensTime = r.time;
    ns->rawtime = (uint64_t)r.time * 1000ULL;
    localtime_r(&ns->sensTime, &ns->sensTm);
    strlcpy(ns->sensDir, r.direction, 32);
    ns->arrowAngle = directionToArrowAngle(ns->sensDir);
    ns->sensSgvMgDl = r.sgMgDl;
    ns->sensSgv = r.sgMgDl / 18.0;
    for (int i = 0; i < 10; i++)
      ns->last10sgv[i] = r.history[i] / 18.0;

    if (r.prevMgDl > 0) {
      ns->delta_mgdl = r.sgMgDl - r.prevMgDl;
      ns->delta_absolute = ns->delta_mgdl;
      ns->delta_scaled = ns->delta_mgdl / 18.0;
      ns->delta_elapsedMins = (r.time - r.prevTime) / 60.0;
      ns->delta_interpolated = false;
      if (cfg->show_mgdl)
        sprintf(ns->delta_display, "%+d", ns->delta_mgdl);
      else
        sprintf(ns->delta_display, "%+.1f", ns->delta_scaled);
    } else {
      ns->delta_mgdl = 0;
      ns->delta_absolute = 0;
      ns->delta_scaled = 0;
      strcpy(ns->delta_display, "N/A");
    }

    if (r.iobValid) {
      ns->iob = r.iob;
      snprintf(ns->iob_display, 16, "%.2f", r.iob);
      snprintf(ns->iob_displayLine, 16, "IOB: %.2fU", r.iob);
    } else {
      ns->iob = 0;
      strlcpy(ns->iob_display, "N/A", 16);
      strlcpy(ns->iob_displayLine, "IOB: N/A", 16);
    }
    ns->cob = 0;
    strlcpy(ns->cob_display, "N/A", 16);
    strlcpy(ns->cob_displayLine, "COB: N/A", 16);
    ns->loop_display_symbol = '?';
    strlcpy(ns->loop_display_code, "N/A", 16);
    strlcpy(ns->loop_display_label, "N/A", 16);
    strlcpy(ns->basal_display, "N/A", 16);

    clLastError = "";
    return 0;
  }
  setError(1302, "CareLink keeps rejecting the login, sign in again on the setup page");
  clNeedsSignIn = true;
  return 1302;
}

// ---------------------------------------------------------------------------------
// setup page support

void careLinkResetSession() {
  clcLoaded = false;
  clSessionLoaded = false;
  clUsername = "";
  clRole = "";
  clPatientId = "";
  clPatientName = "";
  clLastError = "";
}

bool careLinkHasLogin() {
  loadCreds();
  return clRefresh.length() > 0;
}

bool careLinkNeedsSignIn() {
  return clNeedsSignIn || !careLinkHasLogin();
}

String careLinkLoginUrl(tConfig *cfg, String &errOut) {
  WiFiClientSecure client;
  client.setInsecure();
  if (loadConfig(client, cfg) != 0) {
    errOut = clLastError;
    return "";
  }
  if (clcAuthPath.length() <= 1 || clcRedirect.length() == 0) {
    errOut = "CareLink sign-in address not available";
    return "";
  }
  String url = clcSsoBase + clcAuthPath +
               "?client_id=" + urlEncode(clcClientId) +
               "&response_type=code" +
               "&scope=" + urlEncode(clcScope) +
               "&redirect_uri=" + urlEncode(clcRedirect);
  if (clcAudience.length()) url += "&audience=" + urlEncode(clcAudience);
  return url;
}

bool careLinkSignIn(tConfig *cfg, const char *pasted, String &msgOut) {
  String in = String(pasted ? pasted : "");
  in.trim();
  if (in.length() == 0) {
    msgOut = "Nothing was pasted.";
    return false;
  }
  loadCreds();
  WiFiClientSecure client;
  client.setInsecure();

  if (in.startsWith("{")) {
    // Login file from carelink-python-client (logindata.json).
    JsonDocument doc;
    if (deserializeJson(doc, in)) {
      msgOut = "That looks like a login file but could not be read.";
      return false;
    }
    String rt = doc["refresh_token"] | "";
    String cid = doc["client_id"] | "";
    if (rt.length() == 0 || cid.length() == 0) {
      msgOut = "The login file has no refresh_token or client_id.";
      return false;
    }
    clRefresh = rt;
    clClientId = cid;
    clClientSecret = doc["client_secret"] | "";
    saveCreds();
    clAccess = "";
    careLinkResetSession();
    if (loadConfig(client, cfg) != 0) {
      msgOut = "Login saved, but CareLink could not be reached yet: " + clLastError;
      return true;
    }
    if (refreshAccess(client) != 0) {
      msgOut = "Login saved, but CareLink did not accept it: " + clLastError;
      return false;
    }
    msgOut = "Signed in.";
    return true;
  }

  char code[512];
  if (!carelink::extractCode(in.c_str(), code, sizeof(code))) {
    msgOut = "No sign-in code found in what was pasted.";
    return false;
  }
  if (loadConfig(client, cfg) != 0) {
    msgOut = "CareLink could not be reached: " + clLastError;
    return false;
  }
  String body = "client_id=" + urlEncode(clcClientId) +
                "&code=" + urlEncode(String(code)) +
                "&grant_type=authorization_code" +
                "&redirect_uri=" + urlEncode(clcRedirect);
  String resp;
  int status = httpText(client, "POST", clcSsoBase + clcTokenPath, body,
                        "application/x-www-form-urlencoded; charset=UTF-8", false, resp);
  Serial.printf("[CareLink] sign-in code exchange=%d\r\n", status);
  if (status != HTTP_CODE_OK) {
    msgOut = "CareLink did not accept the code (" + String(status) +
             "). Codes are single-use and expire quickly, so sign in again and paste straight away.";
    return false;
  }
  String oldRefresh = clRefresh;
  clRefresh = "";
  clClientSecret = "";
  if (!takeTokens(resp, clcClientId) || clRefresh.length() == 0) {
    clRefresh = oldRefresh;
    msgOut = "CareLink replied without a usable login.";
    return false;
  }
  saveCreds();
  careLinkResetSession();
  msgOut = "Signed in.";
  return true;
}

void careLinkSignOut() {
  clRefresh = "";
  clClientId = "";
  clClientSecret = "";
  clAccess = "";
  saveCreds();
  careLinkResetSession();
  clNeedsSignIn = true;
}

String careLinkStatus() {
  if (!careLinkHasLogin()) return "Not signed in";
  if (clNeedsSignIn) return "Sign-in needed: " + (clLastError.length() ? clLastError : String("login expired"));
  if (clLastError.length()) return "Signed in, last problem: " + clLastError;
  if (clSessionLoaded) {
    if (clRole.equals("carepartner"))
      return "Signed in, following " + (clPatientName.length() ? clPatientName : clPatientId);
    return "Signed in as the patient";
  }
  return "Signed in";
}
