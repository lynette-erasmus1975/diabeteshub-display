/*  M5Stack Nightscout monitor - Medtronic CareLink follower, pure parsing helpers

    Kept free of Arduino/ESP32 dependencies so the parsing can be unit-tested on a PC
    (see the CareLink section of the README). Used only by M5NSCareLink.cpp.

    Field names and the time-zone correction follow the open-source CareLink clients in
    NightscoutFoundation/xDrip (cgm/carelinkfollow, GPL v3) and
    ondrej1024/carelink-python-client (MIT).
*/

#ifndef _M5NSCARELINKPARSE_H
#define _M5NSCARELINKPARSE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef ARDUINOJSON_USE_LONG_LONG
#define ARDUINOJSON_USE_LONG_LONG 1
#endif
#include <ArduinoJson.h>

namespace carelink {

// Days since 1970-01-01 for a UTC civil date (Howard Hinnant's days_from_civil).
inline int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// Parses ISO-8601 style CareLink timestamps into UTC epoch seconds:
//   2026-10-11T14:10:00, 2026-10-11T14:10:00.000, ...Z, ...+01:00, ...+0100, ...-00:00
// A missing zone is treated as UTC (as xDrip does) and reported via *hadZone so the
// caller can apply the server/pump time-zone correction. Returns 0 if unparseable.
inline time_t parseIso(const char *s, bool *hadZone) {
  if (hadZone) *hadZone = false;
  if (s == nullptr) return 0;
  int Y, M, D, h, m, sec = 0;
  int n = 0;
  if (sscanf(s, "%4d-%2d-%2d%*1[T ]%2d:%2d%n", &Y, &M, &D, &h, &m, &n) != 5)
    return 0;
  const char *p = s + n;
  if (*p == ':') {
    p++;
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return 0;
    sec = (p[0] - '0') * 10 + (p[1] - '0');
    p += 2;
  }
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') p++; // fractional seconds ignored
  }
  if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60)
    return 0;
  long offset = 0;
  if (*p == 'Z' || *p == 'z') {
    if (hadZone) *hadZone = true;
  } else if (*p == '+' || *p == '-') {
    int sign = (*p == '-') ? -1 : 1;
    p++;
    int oh = 0, om = 0;
    if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
      oh = (p[0] - '0') * 10 + (p[1] - '0');
      p += 2;
      if (*p == ':') p++;
      if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')
        om = (p[0] - '0') * 10 + (p[1] - '0');
      offset = sign * (oh * 3600L + om * 60L);
      if (hadZone) *hadZone = true;
    }
  }
  int64_t days = daysFromCivil(Y, M, D);
  return (time_t)(days * 86400LL + h * 3600 + m * 60 + sec - offset);
}

// Medtronic trend arrows (lastSGTrend) mapped to Nightscout direction names, which the
// display already knows how to draw. One Medtronic arrow means 1-2 mg/dL/min, two
// arrows 2-3, three arrows over 3. Medtronic shows no arrow when glucose is steady, so
// NONE becomes Flat when there is a current reading.
inline const char *trendToDirection(const char *t, bool haveReading) {
  if (t == nullptr) t = "";
  if (strcmp(t, "UP") == 0) return "FortyFiveUp";
  if (strcmp(t, "UP_DOUBLE") == 0 || strcmp(t, "DOUBLE_UP") == 0) return "SingleUp";
  if (strcmp(t, "UP_TRIPLE") == 0 || strcmp(t, "TRIPLE_UP") == 0) return "DoubleUp";
  if (strcmp(t, "DOWN") == 0) return "FortyFiveDown";
  if (strcmp(t, "DOWN_DOUBLE") == 0 || strcmp(t, "DOUBLE_DOWN") == 0) return "SingleDown";
  if (strcmp(t, "DOWN_TRIPLE") == 0 || strcmp(t, "TRIPLE_DOWN") == 0) return "DoubleDown";
  if (strcmp(t, "NONE") == 0 || t[0] == 0) return haveReading ? "Flat" : "NONE";
  return "NONE";
}

// Pulls the OAuth authorization code out of whatever the user pasted: the full address
// the browser ended on (…?code=XYZ&state=…), just "code=XYZ", or the bare code itself.
// Returns false if nothing usable was found.
inline bool extractCode(const char *in, char *out, size_t outLen) {
  if (in == nullptr || outLen < 2) return false;
  while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
  const char *p = strstr(in, "code=");
  // "code=" must start the string or follow ? / & / #, so "xcode=" or "code_challenge=" don't match
  while (p != nullptr && p != in && p[-1] != '?' && p[-1] != '&' && p[-1] != '#')
    p = strstr(p + 1, "code=");
  const char *start = p ? p + 5 : in;
  if (!p && (strchr(in, '?') || strchr(in, '=') || strstr(in, "://")))
    return false; // looks like a URL but has no code parameter
  size_t len = 0;
  while (start[len] && start[len] != '&' && start[len] != '#' && start[len] != ' ' &&
         start[len] != '\r' && start[len] != '\n' && start[len] != '\t')
    len++;
  if (len == 0 || len >= outLen) return false;
  // undo the only percent-escapes that appear in practice in Auth0 codes
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < outLen; i++) {
    if (start[i] == '%' && i + 2 < len) {
      char hex[3] = {start[i + 1], start[i + 2], 0};
      char *e;
      long v = strtol(hex, &e, 16);
      if (e == hex + 2) {
        out[o++] = (char)v;
        i += 2;
        continue;
      }
    }
    out[o++] = start[i];
  }
  out[o] = 0;
  return o > 0;
}

// One reading as the display needs it. All glucose values in mg/dL.
struct Reading {
  int sgMgDl = 0;          // current sensor glucose, 0 = none
  time_t time = 0;         // UTC epoch seconds of the current reading
  int prevMgDl = 0;        // an earlier reading for the delta, 0 = none
  time_t prevTime = 0;
  const char *direction = "NONE";
  float iob = 0;
  bool iobValid = false;
  int history[10] = {0};   // [0] = current, [k] = reading about k*5 minutes earlier, 0 = gap
  char sensorState[32] = "";
  int timeShiftHours = 0;  // time-zone correction applied (for the log)
};

// Time of one sensor glucose entry: "timestamp" (v13 endpoint) or "datetime" (older).
inline time_t sgTime(JsonObjectConst sg, bool *hadZone) {
  const char *ts = sg["timestamp"] | (const char *)nullptr;
  if (ts == nullptr || ts[0] == 0) ts = sg["datetime"] | (const char *)nullptr;
  return parseIso(ts, hadZone);
}

// Converts CareLink "patientData" (from POST …/display/message) into a Reading.
// Returns 0 on success, 1004 if there is no current sensor glucose value.
inline int parsePatientData(JsonObjectConst pd, Reading *r) {
  *r = Reading();
  if (pd.isNull()) return 1004;

  strncpy(r->sensorState, pd["sensorState"] | "", sizeof(r->sensorState) - 1);

  // Time-zone correction (same rule as xDrip): pump-local times can arrive labelled as
  // UTC. lastConduitUpdateServerDateTime is the server's own epoch-ms clock, so the
  // whole-hour gap between it and lastConduitDateTime is the zone error to remove.
  int shiftH = 0;
  bool conduitZone = false;
  time_t conduit = parseIso(pd["lastConduitDateTime"] | (const char *)nullptr, &conduitZone);
  long long serverMs = pd["lastConduitUpdateServerDateTime"] | 0LL;
  if (conduit > 0 && serverMs > 1) {
    double diff = ((double)(serverMs / 1000) - (double)conduit) / 3600.0;
    int h = (int)(diff < 0 ? diff - 0.5 : diff + 0.5);
    if (h != 0 && h > -26 && h < 26) shiftH = h;
  }
  r->timeShiftHours = shiftH;
  const long shift = shiftH * 3600L;

  // Current value: lastSG, else the newest valid entry in sgs.
  JsonObjectConst last = pd["lastSG"];
  int cur = last["sg"] | 0;
  time_t curT = 0;
  if (cur > 0) {
    curT = sgTime(last, nullptr);
    if (curT > 0) curT += shift;
  }
  JsonArrayConst sgs = pd["sgs"];
  if (cur <= 0 || curT == 0) {
    cur = 0;
    for (JsonObjectConst s : sgs) {
      int v = s["sg"] | 0;
      time_t t = sgTime(s, nullptr);
      if (v > 0 && t > 0 && t + shift >= curT) {
        cur = v;
        curT = t + shift;
      }
    }
  }
  if (cur <= 0 || curT == 0) return 1004;

  r->sgMgDl = cur;
  r->time = curT;
  r->history[0] = cur;
  r->direction = trendToDirection(pd["lastSGTrend"] | "", true);

  // History slots by time, so gaps stay gaps instead of squeezing the graph.
  time_t slotTime[10] = {0};
  for (JsonObjectConst s : sgs) {
    int v = s["sg"] | 0;
    time_t t = sgTime(s, nullptr);
    if (v <= 0 || t <= 0) continue;
    t += shift;
    if (t >= curT) continue;
    long k = (long)((curT - t + 150) / 300);
    if (k < 1 || k > 9) continue;
    if (r->history[k] == 0 || t > slotTime[k]) {
      r->history[k] = v;
      slotTime[k] = t;
    }
  }
  for (int k = 1; k <= 3; k++) {
    if (r->history[k] > 0) {
      r->prevMgDl = r->history[k];
      r->prevTime = slotTime[k];
      break;
    }
  }

  JsonVariantConst ai = pd["activeInsulin"]["amount"];
  if (!ai.isNull()) {
    float a = ai.as<float>();
    if (a >= 0) {
      r->iob = a;
      r->iobValid = true;
    }
  }
  return 0;
}

} // namespace carelink

#endif
