// Host-side unit tests for M5NSCareLinkParse.h (no device needed).
// Build and run from the repository root:
//   g++ -std=c++17 -I. -I<path to ArduinoJson>/src test/carelink_parse_test.cpp -o /tmp/cltest && /tmp/cltest
// The sample replies below are shaped after the CareLink fields documented by
// carelink-python-client (doc/carelink-data.ods) and read by xDrip's CareLink follower.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include "M5NSCareLinkParse.h"

using namespace carelink;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static time_t utc(int y, int mo, int d, int h, int mi, int s = 0) {
  return (time_t)(daysFromCivil(y, mo, d) * 86400LL + h * 3600 + mi * 60 + s);
}

static void testIso() {
  bool z;
  CHECK(parseIso("2026-10-11T14:10:00Z", &z) == utc(2026, 10, 11, 14, 10) && z);
  CHECK(parseIso("2026-10-11T14:10:00.000Z", &z) == utc(2026, 10, 11, 14, 10) && z);
  CHECK(parseIso("2026-10-11T15:10:00+01:00", &z) == utc(2026, 10, 11, 14, 10) && z);
  CHECK(parseIso("2026-10-11T15:10:00.123+0100", &z) == utc(2026, 10, 11, 14, 10) && z);
  CHECK(parseIso("2026-10-11T14:10:00.000-00:00", &z) == utc(2026, 10, 11, 14, 10) && z);
  CHECK(parseIso("2026-10-11T09:10:00-05:00", &z) == utc(2026, 10, 11, 14, 10) && z);
  CHECK(parseIso("2026-10-11T14:10:00", &z) == utc(2026, 10, 11, 14, 10) && !z);
  CHECK(parseIso("2026-10-11 14:10", &z) == utc(2026, 10, 11, 14, 10) && !z);
  CHECK(parseIso("2024-02-29T23:59:59Z", &z) == utc(2024, 2, 29, 23, 59, 59));
  CHECK(parseIso("garbage", &z) == 0);
  CHECK(parseIso("", &z) == 0);
  CHECK(parseIso(nullptr, &z) == 0);
  CHECK(parseIso("2026-13-01T00:00:00Z", &z) == 0);
}

static void testTrend() {
  CHECK(std::string(trendToDirection("UP", true)) == "FortyFiveUp");
  CHECK(std::string(trendToDirection("UP_DOUBLE", true)) == "SingleUp");
  CHECK(std::string(trendToDirection("UP_TRIPLE", true)) == "DoubleUp");
  CHECK(std::string(trendToDirection("DOWN", true)) == "FortyFiveDown");
  CHECK(std::string(trendToDirection("DOWN_DOUBLE", true)) == "SingleDown");
  CHECK(std::string(trendToDirection("DOWN_TRIPLE", true)) == "DoubleDown");
  CHECK(std::string(trendToDirection("NONE", true)) == "Flat");
  CHECK(std::string(trendToDirection("NONE", false)) == "NONE");
  CHECK(std::string(trendToDirection("SOMETHING_NEW", true)) == "NONE");
}

static void testCode() {
  char c[128];
  CHECK(extractCode("com.medtronic.carepartner:/sso?code=AbC123-xyz&state=s1", c, sizeof c) && std::string(c) == "AbC123-xyz");
  CHECK(extractCode("https://example.com/cb?state=1&code=QQ%2BZ", c, sizeof c) && std::string(c) == "QQ+Z");
  CHECK(extractCode("  code=plain\n", c, sizeof c) && std::string(c) == "plain");
  CHECK(extractCode("JustTheCode42", c, sizeof c) && std::string(c) == "JustTheCode42");
  CHECK(!extractCode("https://example.com/cb?state=1", c, sizeof c));
  CHECK(!extractCode("https://x/cb?code_challenge=abc", c, sizeof c));
  CHECK(!extractCode("", c, sizeof c));
}

static int parse(const char *json, Reading *r) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) { printf("bad test json: %s\n", e.c_str()); failures++; return -1; }
  return parsePatientData(doc["patientData"].as<JsonObjectConst>(), r);
}

static void testNormal() {
  // Times carry a zone; server time agrees, so no correction.
  const char *j = R"({"metadata":{},"patientData":{
    "lastConduitDateTime":"2026-10-11T14:11:00.000Z",
    "lastConduitUpdateServerDateTime":1791727860000,
    "sensorState":"NO_ERROR_MESSAGE","lastSGTrend":"DOWN",
    "lastSG":{"sg":112,"timestamp":"2026-10-11T14:10:00.000Z","kind":"SG"},
    "activeInsulin":{"amount":1.35,"kind":"Insulin"},
    "sgs":[
      {"sg":130,"timestamp":"2026-10-11T13:45:00.000Z"},
      {"sg":0,"timestamp":"2026-10-11T13:50:00.000Z","sensorState":"NO_DATA_FROM_PUMP"},
      {"sg":124,"timestamp":"2026-10-11T13:55:00.000Z"},
      {"sg":120,"timestamp":"2026-10-11T14:00:00.000Z"},
      {"sg":116,"timestamp":"2026-10-11T14:05:00.000Z"},
      {"sg":112,"timestamp":"2026-10-11T14:10:00.000Z"}]}})";
  Reading r;
  CHECK(parse(j, &r) == 0);
  CHECK(r.sgMgDl == 112);
  CHECK(r.time == utc(2026, 10, 11, 14, 10));
  CHECK(std::string(r.direction) == "FortyFiveDown");
  CHECK(r.prevMgDl == 116 && r.prevTime == utc(2026, 10, 11, 14, 5));
  CHECK(r.history[0] == 112 && r.history[1] == 116 && r.history[2] == 120 && r.history[3] == 124);
  CHECK(r.history[4] == 0); // 13:50 had no value: stays a gap
  CHECK(r.history[5] == 130);
  CHECK(r.iobValid && std::fabs(r.iob - 1.35f) < 0.001f);
  CHECK(r.timeShiftHours == 0);
}

static void testZoneCorrection() {
  // Pump-local Irish summer time (UTC+1) labelled as UTC: server clock says 13:11 UTC
  // while the conduit time claims 14:11, so everything moves back one hour.
  const char *j = R"({"patientData":{
    "lastConduitDateTime":"2026-10-11T14:11:00.000Z",
    "lastConduitUpdateServerDateTime":1791724260000,
    "lastSGTrend":"NONE",
    "lastSG":{"sg":99,"datetime":"2026-10-11T14:10:00"},
    "sgs":[{"sg":101,"datetime":"2026-10-11T14:05:00"},{"sg":99,"datetime":"2026-10-11T14:10:00"}]}})";
  Reading r;
  CHECK(parse(j, &r) == 0);
  CHECK(r.timeShiftHours == -1);
  CHECK(r.time == utc(2026, 10, 11, 13, 10));
  CHECK(r.prevMgDl == 101);
  CHECK(std::string(r.direction) == "Flat");
  CHECK(!r.iobValid);
}

static void testNoReading() {
  const char *j = R"({"patientData":{"sensorState":"WARM_UP","lastSG":{"sg":0,"timestamp":"2026-10-11T14:10:00Z"},"sgs":[]}})";
  Reading r;
  CHECK(parse(j, &r) == 1004);
  CHECK(std::string(r.sensorState) == "WARM_UP");
  CHECK(parse(R"({"patientData":null})", &r) == 1004);
  CHECK(parse(R"({})", &r) == 1004);
}

static void testFallbackToSgs() {
  // lastSG missing: newest valid sgs entry is used.
  const char *j = R"({"patientData":{"lastSGTrend":"UP_DOUBLE","sgs":[
      {"sg":140,"timestamp":"2026-10-11T14:00:00Z"},
      {"sg":150,"timestamp":"2026-10-11T14:05:00Z"},
      {"sg":0,"timestamp":"2026-10-11T14:10:00Z"}]}})";
  Reading r;
  CHECK(parse(j, &r) == 0);
  CHECK(r.sgMgDl == 150 && r.time == utc(2026, 10, 11, 14, 5));
  CHECK(r.prevMgDl == 140);
  CHECK(std::string(r.direction) == "SingleUp");
}

static void testNegativeIob() {
  const char *j = R"({"patientData":{"lastSG":{"sg":100,"timestamp":"2026-10-11T14:10:00Z"},"activeInsulin":{"amount":-1}}})";
  Reading r;
  CHECK(parse(j, &r) == 0);
  CHECK(!r.iobValid);
  CHECK(r.prevMgDl == 0);
}

int main() {
  testIso();
  testTrend();
  testCode();
  testNormal();
  testZoneCorrection();
  testNoReading();
  testFallbackToSgs();
  testNegativeIob();
  if (failures) { printf("%d check(s) failed\n", failures); return 1; }
  printf("All CareLink parsing tests passed\n");
  return 0;
}
