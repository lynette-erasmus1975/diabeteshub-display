#include "M5NSDevice.h"  // SD.h + M5Unified, or a non-M5 board shim (-DDEVICE_JC3248W535 / -DDEVICE_WS_TOUCH_LCD_35)

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include "time.h"
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include "Free_Fonts.h"
#include "IniFile.h"
#include "M5NSconfig.h"
#include "M5NSWebConfig.h"
#include "M5NSDexcom.h"
#include "M5NSLibre.h"
#include "M5NSCareLink.h"
#include "externs.h"

// OTA firmware is served straight from this repo (raw.githubusercontent.com, master
// branch, Binaries/ folder). Variant selection mirrors the three images actually
// produced by Scripts/build.ps1 (see Binaries/firmware.json): Basic_4MB (min_spiffs
// partitions, 4MB flash), ESP32_16MB (default partitions, 16MB flash - covers Basic
// 16MB, Fire and all Core2), and CoreS3 (ESP32-S3). Selection is by chip + flash size,
// not board model, since a "basic-looking" board can have either partition layout.
static const char* updateVariantPath() {
#if defined(DEVICE_JC3248W535)
  // Must come before the generic S3 branch: a JC board pulling the CoreS3 image
  // would flash firmware for the wrong panel/pins.
  return "JC3248W535";
#elif defined(DEVICE_WS_TOUCH_LCD_35)
  // Classic-ESP32 16MB board: must come before the flash-size fallback, which would
  // otherwise route it to the ESP32_16MB (M5 panel/pins) image.
  return "WS_TouchLCD35";
#elif CONFIG_IDF_TARGET_ESP32S3
  return "CoreS3";
#else
  return (ESP.getFlashChipSize() >= 16 * 1024 * 1024) ? "ESP32_16MB" : "Basic_4MB";
#endif
}
// Wee Glo: updates come only from the DiabetesHub display's own published files, never from
// the original project, whose builds have no CareLink support. Only the Core2 (ESP32_16MB)
// build is published; every other variant gets a 404, so it is simply never offered an update.
static String updateURL(const char* file) {
  return String("https://lynette-erasmus1975.github.io/diabeteshub-display/ota/")
         + updateVariantPath() + "/" + file;
}

// ---- Shared page chrome + compact row renderers --------------------------------
// Settings are grouped into collapsible <details> sections (native HTML, no JS
// framework) and rendered via these row helpers instead of one-line-per-setting
// string concatenation, so the generated HTML stays small and reserve() up front
// avoids the repeated String reallocation the old ~230-concatenation build had.

// Set when an edited item needs a device restart to take effect (device name, time zone/DST,
// WiFi networks); read by handleRoot() to show a warning banner and by handleSaveConfig() to
// restart automatically after writing M5NS.INI. Lives only in RAM - a real restart clears it,
// and that's the only way it needs to be cleared.
static bool restartPending = false;
// Where the DiabetesHub Wee Glo guide lives. The test build points at the test site; the release build
// MUST be built with "https://diabeteshub.ie" (the guide is only on the live site from launch day).
#ifndef WG_SITE
#define WG_SITE "https://diabeteshub.ie"
#endif
extern void lcdSetBrightness(uint8_t brightness);
static const char* WG_BACK_EARLY = "<svg viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M15 6l-6 6 6 6\"/></svg>";

// Defined further down (shared by handleSaveConfig() and handleGetEditConfigItem()).
static void persistConfigToDisk();

static void pageHeadOpen(String& m, const char* extraMeta) {
  m += "<!DOCTYPE HTML>\r\n<html lang=\"en-IE\">\r\n<head>\r\n";
  m += "<meta charset=\"utf-8\">\r\n";
  m += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\r\n";
  if (extraMeta && extraMeta[0]) m += extraMeta;
  // Wee Glo look, matching diabeteshub.ie, light and dark
  m += "<script>(function(){var t='light';try{var s=localStorage.getItem('dhub_theme');if(s==='dark'||s==='system')t=s;}catch(e){}document.documentElement.setAttribute('data-theme',t);})();</script>\r\n";
  m += "<style>\r\n";
  m += ":root{--navy:#1E3A8A;--blue:#1D4ED8;--bpp:#DBEAFE;--green:#15803D;--gpp:#F0FDF4;--bg:#F7F8FA;--card:#fff;--soft:#F9FAFB;--text:#111827;--muted:#4B5563;--border:#E5E7EB;--red:#BE123C}\r\n";
  // Light, dark or match my device, the same choice as on diabeteshub.ie. Light is the standard look.
  m += ":root[data-theme=dark]{--blue:#93C5FD;--bpp:#1E3A5F;--gpp:#10291D;--bg:#0B1220;--card:#151E2E;--soft:#1B2535;--text:#E5E7EB;--muted:#A3AEBF;--border:#2A3648;--red:#FDA4AF;color-scheme:dark}@media(prefers-color-scheme:dark){:root[data-theme=system]{--blue:#93C5FD;--bpp:#1E3A5F;--gpp:#10291D;--bg:#0B1220;--card:#151E2E;--soft:#1B2535;--text:#E5E7EB;--muted:#A3AEBF;--border:#2A3648;--red:#FDA4AF;color-scheme:dark}}\r\n";
  m += "*{box-sizing:border-box}body{font-family:Inter,system-ui,-apple-system,\"Segoe UI\",Roboto,Arial,sans-serif;font-size:16px;line-height:1.5;margin:0;background:var(--bg);color:var(--text)}\r\n";
  // the DiabetesHub header, as on diabeteshub.ie: logo, name, and the light or dark button
  m += "header{background:#1E3A8A;color:#fff}.hi{max-width:1240px;margin:0 auto;padding:11px 18px;display:flex;align-items:center;justify-content:space-between;gap:14px}"
       ".logo-w{display:flex;align-items:center;gap:11px;text-decoration:none}.logo-mark{width:46px;height:46px;flex-shrink:0}.logo-txt{display:flex;flex-direction:column;line-height:1.05}"
       ".logo-name{font-size:20px;font-weight:700;color:#fff;letter-spacing:-.4px}.logo-name em{font-style:normal}.logo-name .acc{color:#86EFAC}"
       ".logo-tag{font-size:12px;color:rgba(255,255,255,.45);letter-spacing:.22em;text-transform:uppercase;margin-top:3px}"
       "#themeBtn{background:rgba(255,255,255,.14);border:1px solid rgba(255,255,255,.3);color:#fff;width:40px;height:40px;border-radius:50%;cursor:pointer;flex-shrink:0;display:flex;align-items:center;justify-content:center;padding:0}#themeBtn:hover{background:rgba(255,255,255,.26)}"
       ".dhi{display:inline-flex;align-items:center;justify-content:center;flex:none}.dhi svg{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}\r\n";
  // the light or dark chooser, as on diabeteshub.ie
  m += "#themeBackdrop{position:fixed;inset:0;background:rgba(15,23,42,.55);z-index:600;display:flex;align-items:center;justify-content:center;padding:16px}"
       ".stat-modal{background:var(--card);border-radius:20px;max-width:440px;width:100%;box-shadow:0 20px 60px rgba(29,78,216,.2);position:relative;overflow:hidden;animation:smIn .28s cubic-bezier(.34,1.3,.64,1)}"
       "@keyframes smIn{from{opacity:0;transform:scale(.88) translateY(12px)}to{opacity:1;transform:none}}"
       ".sm-header{background:#1E3A8A;padding:22px 24px 18px;color:#fff;position:relative}.sm-eyebrow{font-size:12px;opacity:.7;text-transform:uppercase;letter-spacing:.12em;margin-bottom:6px}"
       ".sm-title{font-size:22px;font-weight:700;line-height:1.3;padding-right:30px}.sm-close{position:absolute;top:14px;right:14px;background:rgba(255,255,255,.18);border:none;width:30px;height:30px;border-radius:50%;cursor:pointer;color:#fff;display:flex;align-items:center;justify-content:center}"
       ".sm-body{padding:14px 16px 18px}.theme-opt{width:100%;display:flex;align-items:center;gap:13px;text-align:left;background:var(--soft);border:2px solid var(--border);border-radius:12px;padding:13px 14px;margin-bottom:9px;cursor:pointer;font-family:inherit;color:var(--text);min-height:60px}"
       ".theme-opt:hover,.theme-opt.sel{border-color:var(--blue)}.theme-opt.sel{background:var(--bpp)}.theme-opt .dhi svg{width:26px;height:26px}"
       ".to-tx{display:flex;flex-direction:column;gap:2px;font-size:16px;color:var(--muted)}.to-tx strong{color:var(--text)}"
       ".to-tx em{font-style:normal;font-size:12px;font-weight:700;background:var(--gpp);color:var(--green);border-radius:20px;padding:1px 8px;margin-left:4px}\r\n";
  m += "main{max-width:560px;margin:0 auto;padding:14px 16px 32px}\r\n";
  m += "h1{font-size:23px;margin:4px 0 6px}h2{font-size:20px;margin:0 0 8px}.lead{color:var(--muted);margin:0 0 14px}\r\n";
  m += ".list{display:flex;flex-direction:column;gap:10px}\r\n";
  m += ".cl{display:flex;align-items:center;gap:12px;background:var(--card);border:2px solid var(--border);border-radius:14px;padding:12px 14px;color:var(--text);min-height:64px}\r\n";
  m += ".cl.dn{border-color:var(--green);background:var(--gpp)}.ci{width:34px;height:34px;border-radius:50%;background:var(--navy);color:#fff;font-weight:800;display:flex;align-items:center;justify-content:center;flex-shrink:0}.cl.dn .ci{background:var(--green)}\r\n";
  m += ".tx{flex:1;min-width:0}.tx b{display:block}.tx span{font-size:14px;color:var(--muted)}.ar{color:var(--blue)}\r\n";
  m += ".card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:16px}\r\n";
  m += ".fld{display:block;font-weight:700;font-size:15px;margin:12px 0 4px}.inp{width:100%;min-height:46px;border:2px solid var(--border);border-radius:10px;padding:0 12px;font:inherit;background:var(--card);color:var(--text)}\r\n";
  m += ".row2{display:grid;grid-template-columns:1fr 1fr;gap:0 12px;align-items:end}\r\n";
  m += ".nav{display:flex;gap:10px;margin-top:16px}.nav>*{flex:1}.nav>.o{flex:.7}\r\n";
  m += ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:50px;padding:0 16px;border-radius:12px;background:var(--navy);color:#fff !important;font-weight:700;font-size:16px;border:2px solid var(--navy);text-decoration:none;cursor:pointer;font-family:inherit}\r\n";
  m += ".btn.o{background:transparent;color:var(--blue) !important;border-color:var(--blue)}.btn.g{background:var(--green);border-color:var(--green)}\r\n";
  m += ".btn2{display:inline-flex;align-items:center;min-height:44px;padding:0 16px;border-radius:12px;background:transparent;color:var(--blue) !important;border:2px solid var(--blue);font-weight:700;font-size:15px;margin:6px 0 6px 6px;font-family:inherit}\r\n";
  m += ".bk{display:inline-flex;align-items:center;gap:4px;font-weight:700;min-height:44px}.cnt{font-size:14px;font-weight:700;color:var(--muted);margin:0 0 10px}\r\n";
  m += ".ok{background:var(--gpp);border:1px solid var(--green);border-radius:10px;padding:10px 12px}.bad{border:1px solid var(--red);color:var(--red);border-radius:10px;padding:10px 12px}\r\n";
  m += ".small{font-size:14px;color:var(--muted)}a{color:var(--blue);text-decoration:none}\r\n";
  // the full settings list (/all) keeps its own compact rows
  m += "details{border:1px solid var(--border);border-radius:10px;margin-bottom:8px;background:var(--card)}summary{font-weight:700;padding:12px;cursor:pointer}\r\n";
  m += ".row{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:6px;padding:8px 12px;border-top:1px solid var(--border)}.row>span:first-child{color:var(--muted);margin-right:8px}\r\n";
  m += "select,input[type=text],input[type=password],textarea{font-size:16px;padding:6px;background:var(--card);color:var(--text);border:1px solid var(--border);border-radius:8px}\r\n";
  m += ".y{color:#B45309}.r{color:var(--red)}.t{color:var(--green)}.warn{color:var(--red)}.note{color:var(--muted);font-size:13px;margin-top:2px;flex-basis:100%}\r\n";
  m += ".sw{display:inline-block;min-width:44px;text-align:center;padding:6px 10px;border-radius:12px;background:#94A3B8;color:#fff !important;font-size:13px;font-weight:700}.sw.on{background:var(--green)}\r\n";
  m += ".seg{display:inline-flex;border-radius:8px;overflow:hidden;border:1px solid var(--border)}.seg a{padding:6px 10px;background:var(--soft);color:var(--text) !important;font-size:13px}.seg a.act{background:var(--green);color:#fff !important;font-weight:700}\r\n";
  // Wee Glo: a "please wait" message for steps that take a few seconds (finding Wi-Fi networks,
  // saving, signing in, checking for updates), so nobody thinks the page is stuck.
  m += "#wgw{position:fixed;inset:0;background:rgba(11,18,32,.72);display:none;align-items:center;justify-content:center;z-index:9;padding:24px}"
       "#wgw div{background:var(--card);color:var(--text);border-radius:16px;padding:22px 20px;max-width:340px;text-align:center;font-weight:700}"
       "#wgw i{display:block;width:36px;height:36px;margin:0 auto 12px;border:4px solid var(--border);border-top-color:var(--blue);border-radius:50%;animation:wgs 1s linear infinite}"
       "@keyframes wgs{to{transform:rotate(360deg)}}\r\n";
  m += "</style>\r\n";
  m += "<script>function wgWait(t){var w=document.getElementById('wgw');if(!w){w=document.createElement('div');w.id='wgw';w.setAttribute('role','status');w.innerHTML='<div><i></i><span></span></div>';document.body.appendChild(w);}w.querySelector('span').textContent=t;w.style.display='flex';}"
       "document.addEventListener('click',function(e){var a=e.target.closest&&e.target.closest('a[data-wait]');if(a)wgWait(a.getAttribute('data-wait'));});"
       "document.addEventListener('submit',function(e){var f=e.target;if(f.getAttribute&&f.getAttribute('data-wait'))wgWait(f.getAttribute('data-wait'));});"
       "window.addEventListener('pageshow',function(){var w=document.getElementById('wgw');if(w)w.style.display='none';});</script>\r\n";
  m += "<title>Wee Glo settings</title>\r\n";
  m += "</head>\r\n<body>\r\n<header><div class=\"hi\"><a class=\"logo-w\" href=\"/\">"
       "<svg class=\"logo-mark\" viewBox=\"0 0 100 100\" fill=\"none\" aria-label=\"DiabetesHub Ireland logo\"><circle cx=\"50\" cy=\"50\" r=\"47\" fill=\"rgba(255,255,255,0.08)\" stroke=\"rgba(255,255,255,0.25)\" stroke-width=\"1.2\"/><path d=\"M50 10C50 10 44 16.3 44 19.52C44 22.6 47 24 50 24C53 24 56 22.6 56 19.52C56 16.3 50 10 50 10Z\" fill=\"#86EFAC\"/><path d=\"M50 84C20 64 14 46 22 36C30 27 43 30 50 40C57 30 70 27 78 36C86 46 80 64 50 84Z\" fill=\"rgba(255,255,255,0.14)\" stroke=\"#fff\" stroke-width=\"2.4\" stroke-linejoin=\"round\"/><path d=\"M24 56L38 56L43 46L50 68L56 50L60 56L76 56\" stroke=\"#86EFAC\" stroke-width=\"3\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>"
       "<div class=\"logo-txt\"><div class=\"logo-name\"><em>Diabetes</em><span class=\"acc\">Hub</span></div><div class=\"logo-tag\">Ireland</div></div></a>"
       "<button type=\"button\" id=\"themeBtn\" aria-haspopup=\"dialog\" aria-label=\"Choose light or dark appearance\" title=\"Appearance\"><span class=\"dhi\"></span></button></div></header>\r\n";
  // the light or dark button and chooser, the same as on diabeteshub.ie (the choice is kept in this browser)
  m += "<script>(function(){var SUN=\"<svg viewBox=\'0 0 24 24\'><circle cx=\'12\' cy=\'12\' r=\'4\'/><path d=\'M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4\'/></svg>\",MOON=\"<svg viewBox=\'0 0 24 24\'><path d=\'M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z\'/></svg>\",PH=\"<svg viewBox=\'0 0 24 24\'><rect x=\'7\' y=\'2\' width=\'10\' height=\'20\' rx=\'2\'/><path d=\'M11 18h2\'/></svg>\",X=\"<svg viewBox=\'0 0 24 24\'><path d=\'M6 6l12 12M18 6L6 18\'/></svg>\";"
       "function cur(){return document.documentElement.getAttribute('data-theme')||'light';}"
       "function dark(){var t=cur();return t==='dark'||(t==='system'&&window.matchMedia&&matchMedia('(prefers-color-scheme: dark)').matches);}"
       "var b=document.getElementById('themeBtn');function sync(){b.firstChild.innerHTML=dark()?MOON:SUN;}sync();"
       "if(window.matchMedia)try{matchMedia('(prefers-color-scheme: dark)').addEventListener('change',sync);}catch(e){}"
       "function close(){var d=document.getElementById('themeBackdrop');if(d)d.remove();}"
       "function set(t){document.documentElement.setAttribute('data-theme',t);try{localStorage.setItem('dhub_theme',t);}catch(e){}sync();close();}"
       "b.onclick=function(){close();var c=cur(),o=[['light',SUN,'Light <em>Standard</em>','Bright and clean. This is the standard look.'],['dark',MOON,'Dark','Easier on the eyes at night or in low light.'],['system',PH,'Match my device','Follow my phone or computer setting automatically.']];"
       "var d=document.createElement('div');d.id='themeBackdrop';d.setAttribute('role','dialog');d.setAttribute('aria-modal','true');d.setAttribute('aria-label','Choose appearance');"
       "d.innerHTML='<div class=\"stat-modal\"><div class=\"sm-header\"><div class=\"sm-eyebrow\">Appearance</div><div class=\"sm-title\">How would you like Wee Glo settings to look?</div><button type=\"button\" class=\"sm-close\" aria-label=\"Close\"><span class=\"dhi\">'+X+'</span></button></div><div class=\"sm-body\">'"
       "+o.map(function(x){return '<button type=\"button\" class=\"theme-opt'+(x[0]===c?' sel':'')+'\" data-t=\"'+x[0]+'\"><span class=\"dhi\">'+x[1]+'</span><span class=\"to-tx\"><strong>'+x[2]+'</strong><span>'+x[3]+'</span></span></button>';}).join('')+'</div></div>';"
       "d.addEventListener('click',function(e){var t=e.target.closest('[data-t]');if(t)return set(t.getAttribute('data-t'));if(e.target===d||e.target.closest('.sm-close'))close();});"
       "document.body.appendChild(d);var s=d.querySelector('.sel');if(s)s.focus();};"
       "document.addEventListener('keydown',function(e){if(e.key==='Escape')close();});})();</script>\r\n<main>\r\n";
}

static void detailsOpen(String& m, const char* id, const char* label, const String& sec) {
  m += "<details"; if (sec.equals(id)) m += " open"; m += "><summary>"; m += label; m += "</summary>\r\n";
}

static void rowText(String& m, const char* label, const String& value) {
  m += "<div class=\"row\"><span>"; m += label; m += "</span><span><b>"; m += value; m += "</b></span></div>\r\n";
}

static void rowEdit(String& m, const char* label, const String& value, const char* editParam, const char* sec, const char* cls = NULL) {
  m += "<div class=\"row\"><span>"; m += label; m += "</span><span>";
  m += "<b"; if (cls) { m += " class=\""; m += cls; m += "\""; } m += ">";
  m += value; m += "</b> <a href=\"edititem?param="; m += editParam; m += "&s="; m += sec; m += "\">edit</a></span></div>\r\n";
}

static void rowToggle(String& m, const char* label, bool value, const char* param, const char* sec, const char* onLabel = "YES", const char* offLabel = "NO") {
  m += "<div class=\"row\"><span>"; m += label; m += "</span><span><a class=\"sw";
  if (value) m += " on";
  m += "\" href=\"switch?param="; m += param; m += "&s="; m += sec; m += "\">";
  m += (value ? onLabel : offLabel); m += "</a></span></div>\r\n";
}

// Two-value choice rendered as two side-by-side buttons (the active one highlighted) instead
// of a single value+link - both options are visible so there's nothing to "cycle" through.
static void rowSeg(String& m, const char* label, bool isOn, const char* param, const char* sec, const char* onLabel, const char* offLabel) {
  m += "<div class=\"row\"><span>"; m += label; m += "</span><span class=\"seg\">";
  m += "<a"; if (isOn) m += " class=\"act\""; m += " href=\"switch?param="; m += param; m += "&v=1&s="; m += sec; m += "\">"; m += onLabel; m += "</a>";
  m += "<a"; if (!isOn) m += " class=\"act\""; m += " href=\"switch?param="; m += param; m += "&v=0&s="; m += sec; m += "\">"; m += offLabel; m += "</a>";
  m += "</span></div>\r\n";
}

static void rowSelect(String& m, const char* label, const char* param, const char* sec, const char* const opts[], const int vals[], int n, int current) {
  m += "<div class=\"row\"><span>"; m += label; m += "</span><span><select onchange=\"location='switch?param=";
  m += param; m += "&v='+this.value+'&s="; m += sec; m += "'\">\r\n";
  for (int i = 0; i < n; i++) {
    m += "<option value=\""; m += vals[i]; m += "\"";
    if (vals[i] == current) m += " selected";
    m += ">"; m += opts[i]; m += "</option>";
  }
  m += "</select></span></div>\r\n";
}

// Escapes text from outside the device (CareLink replies, names) before it goes into a page.
static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

// Row for the /edititem forms: label + input on one line, optional note text wrapping to
// its own line below (via .note{flex-basis:100%}), optional color class on the label.
static void editRow(String& m, const char* label, const String& inputHtml, const char* note = NULL, const char* cls = NULL) {
  m += "<div class=\"row\"><span"; if (cls) { m += " class=\""; m += cls; m += "\""; } m += ">";
  m += label; m += "</span><span>"; m += inputHtml; m += "</span>";
  if (note) { m += "<span class=\"note\">"; m += note; m += "</span>"; }
  m += "</div>\r\n";
}

void handleRoot() {
  IPAddress ip = WiFi.localIP();
  char tmpStr[64];
  char timeStr[20];
  char dateStr[20];

  Serial.println("Serving root web page");

  // Which section (if any) to render open - the one just touched by a switch/edit, so a
  // change doesn't collapse the page back to Status-only. Defaults to Status.
  String sec = w3srv.hasArg("s") ? w3srv.arg("s") : String("st");

  char NSurl[128];
  if(strncmp(cfg.url, "http", 4))
    strcpy(NSurl,"https://");
  else
    strcpy(NSurl,"");
  strcat(NSurl,cfg.url);
  if ((cfg.token!=NULL) && (strlen(cfg.token)>0)) {
    if(strchr(NSurl,'?'))
      strcat(NSurl,"&token=");
    else
      strcat(NSurl,"?token=");
    strcat(NSurl,cfg.token);
  }

  String sgvUnits = cfg.show_mgdl?"mg/dL":"mmol/L";
  int decpl = cfg.show_mgdl?0:1;
  int mult = cfg.show_mgdl?18:1;

  String message;
  message.reserve(11264);
  pageHeadOpen(message, NULL);
  message += "<a class=\"bk\" href=\"/\">&lsaquo; Wee Glo settings</a><h1>Advanced settings</h1><p class=\"lead\">Every setting, for confident users. Most families only need the main Wee Glo settings page.</p>\r\n";

  message += "<a class=\"btn\" href=\"/savecfg\">Save all settings</a>\r\n";
  if (restartPending) {
    message += "<p class=\"warn\">Changes pending that need a restart - the device will restart automatically when you save.</p>\r\n";
  }

  // ---- Status ----
  detailsOpen(message, "st", "Status", sec);
  if ( WiFi.status() == WL_CONNECTED) {
    rowText(message, "WiFi SSID", WiFi.SSID());
    if(mDNSactive) {
      sprintf(tmpStr, "%s.local", cfg.deviceName);
      rowText(message, "mDNS name", tmpStr);
    }
    sprintf(tmpStr, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    rowText(message, "IP address", tmpStr);
    byte mac[6];
    WiFi.macAddress(mac);
    String macStr = String(mac[5],HEX) + ":" + String(mac[4],HEX) + ":" + String(mac[3],HEX) + ":" + String(mac[2],HEX) + ":" + String(mac[1],HEX) + ":" + String(mac[0],HEX);
    rowText(message, "MAC address", macStr);
  }
  sprintf(tmpStr, "%d%%", getBatteryLevel());
  rowText(message, "Battery status", tmpStr);
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)) {
    switch(cfg.time_format) {
      case 1:
        strftime(timeStr, 19, "%I:%M:%S %p ", &timeinfo);
        break;
      default:
        strftime(timeStr, 19, "%H:%M:%S ", &timeinfo);
    }
    switch(cfg.date_format) {
      case 1:
        strftime(dateStr, 19, "%m/%d/%Y ", &timeinfo);
        break;
      default:
        strftime(dateStr, 19, "%e.%m.%Y  ", &timeinfo);
    }
    strcpy(tmpStr, dateStr);
    strcat(tmpStr, timeStr);
  } else {
    strcpy(tmpStr, "??:??");
  }
  rowText(message, "Current time", tmpStr);
  switch(cfg.time_format) {
    case 1:
      strftime(timeStr, 15, "%I:%M:%S %p ", &ns.sensTm);
      break;
    default:
      strftime(timeStr, 15, "%H:%M:%S ", &ns.sensTm);
  }
  switch(cfg.date_format) {
    case 1:
      strftime(dateStr, 15, "%m/%d/%Y ", &ns.sensTm);
      break;
    default:
      strftime(dateStr, 15, "%e.%m.%Y  ", &ns.sensTm);
  }
  strcpy(tmpStr, dateStr);
  strcat(tmpStr, timeStr);
  rowText(message, "Sensor time", tmpStr);
  if( cfg.show_mgdl ) {
    sprintf(tmpStr, "%3.0f mg/dL", ns.sensSgvMgDl);
  } else {
    sprintf(tmpStr, "%4.1f mmol/L", ns.sensSgv);
  }
  rowText(message, "Last sensor value", tmpStr);
  rowText(message, "Last sensor direction", ns.sensDir);
  message += "</details>\r\n";

  // ---- Data source ----
  // Exactly one source is ever active (see readDataSource() in the .ino) - only the
  // fields relevant to the selected source are shown here, the rest stay hidden.
  detailsOpen(message, "dx", "Data source", sec);
  {
    const char* dsOpts[4] = {"Nightscout", "Dexcom Share", "LibreLinkUp", "Medtronic CareLink"};
    const int dsVals[4] = {0, 1, 2, 3};
    rowSelect(message, "Source", "data_source", "dx", dsOpts, dsVals, 4, cfg.data_source);
  }
  message += "<div class=\"row\"><span class=\"note\">Changing the source takes effect after a restart - the device restarts automatically when you save.</span></div>\r\n";
  if (cfg.data_source == 1) {
    rowEdit(message, "Dexcom account", strlen(cfg.dexcom_user) > 0 ? String(cfg.dexcom_user) : String("(not set)"), "dexcom", "dx");
    {
      const char* srvOpts[3] = {"US", "Outside US", "Japan"};
      const int srvVals[3] = {0, 1, 2};
      rowSelect(message, "Dexcom server", "dexcom_server", "dx", srvOpts, srvVals, 3, cfg.dexcom_server);
    }
  } else if (cfg.data_source == 2) {
    rowEdit(message, "LibreLinkUp account", strlen(cfg.libre_user) > 0 ? String(cfg.libre_user) : String("(not set)"), "libre", "dx");
    {
      int srvVals[13];
      for (int i = 0; i < 13; i++) srvVals[i] = i;
      rowSelect(message, "LibreLinkUp region", "libre_server", "dx", libreRegionNames, srvVals, 13, cfg.libre_server);
    }
    message += "<div class=\"row\"><span class=\"note\">Leave at AUTO: the device logs in through LibreLinkUp's universal entry point and detects your region automatically.</span></div>\r\n";
  } else if (cfg.data_source == 3) {
    rowEdit(message, "CareLink country", strlen(cfg.cl_country) > 0 ? String(cfg.cl_country) : String("(not set)"), "carelink", "dx", strlen(cfg.cl_country) > 0 ? NULL : "warn");
    rowEdit(message, "Patient to follow", strlen(cfg.cl_patient) > 0 ? String(cfg.cl_patient) : String("(first linked patient)"), "carelink", "dx");
    message += "<div class=\"row\"><span>CareLink login</span><span><b";
    if (careLinkNeedsSignIn()) message += " class=\"warn\"";
    message += ">"; message += htmlEscape(careLinkStatus()); message += "</b> <a href=\"carelink\">";
    message += careLinkHasLogin() ? "manage" : "sign in";
    message += "</a></span></div>\r\n";
  } else {
    rowEdit(message, "Nightscout URL", "<a href=\"" + String(NSurl) + "\">" + String(NSurl) + "</a>", "NSurl", "dx");
    rowToggle(message, "Filter only SGV values", cfg.sgv_only, "sgv_only", "dx");
  }
  message += "</details>\r\n";

  // ---- Display ----
  detailsOpen(message, "di", "Display", sec);
  rowEdit(message, "User name", cfg.userName, "userName", "di");
  rowSeg(message, "Display units", cfg.show_mgdl, "show_mgdl", "di", "mg/dL", "mmol/L");
  {
    char pageLabels[8][4];
    const char* pageOpts[8]; int pageVals[8];
    int n = 0;
    for (int p = 0; p <= maxPage && p < 8; p++) {
      snprintf(pageLabels[p], sizeof(pageLabels[p]), "%d", p);
      pageOpts[p] = pageLabels[p];
      pageVals[p] = p;
      n++;
    }
    rowSelect(message, "Default page", "default_page", "di", pageOpts, pageVals, n, cfg.default_page);
  }
  rowSeg(message, "Show time", cfg.show_current_time, "show_current_time", "di", "current time", "last data");
  rowToggle(message, "Show COB+IOB", cfg.show_COB_IOB, "show_COB_IOB", "di");
  rowSeg(message, "Time format", cfg.time_format, "time_format", "di", "AM/PM", "24h");
  rowSeg(message, "Date format", cfg.date_format, "date_format", "di", "MM/DD", "dd.mm.");
  {
    const char* rotOpts[4] = {"buttons down","buttons up","mirror buttons up","mirror buttons down"};
    const int rotVals[4] = {1,3,5,7};
    rowSelect(message, "Display rotation", "display_rotation", "di", rotOpts, rotVals, 4, cfg.display_rotation);
  }
  {
    const char* invOpts[3] = {"do not change","false","true"};
    const int invVals[3] = {-1,0,1};
    rowSelect(message, "Display inversion", "invert_display", "di", invOpts, invVals, 3, cfg.invert_display);
  }
  rowEdit(message, "Brightness steps", String(cfg.brightness1) + ", " + String(cfg.brightness2) + ", " + String(cfg.brightness3), "brightness", "di");
  {
    const char* lineOpts[4] = {"sensor info","button function icons","loop info + basal","openaps info + basal"};
    const int lineVals[4] = {0,1,2,3};
    rowSelect(message, "Status line", "info_line", "di", lineOpts, lineVals, 4, cfg.info_line);
  }
  {
    const char* tempOpts[3] = {"Celsius","Kelvin","Fahrenheit"};
    const int tempVals[3] = {1,2,3};
    rowSelect(message, "Temperature units", "temperature_unit", "di", tempOpts, tempVals, 3, cfg.temperature_unit);
  }
  message += "</details>\r\n";

  // ---- Colors & thresholds ----
  detailsOpen(message, "th", "Colors & thresholds", sec);
  rowEdit(message, "Yellow below", String(cfg.yellow_low*mult, decpl) + " " + sgvUnits, "dispColors", "th", "y");
  rowEdit(message, "Yellow above", String(cfg.yellow_high*mult, decpl) + " " + sgvUnits, "dispColors", "th", "y");
  rowEdit(message, "Red below", String(cfg.red_low*mult, decpl) + " " + sgvUnits, "dispColors", "th", "r");
  rowEdit(message, "Red above", String(cfg.red_high*mult, decpl) + " " + sgvUnits, "dispColors", "th", "r");
  message += "</details>\r\n";

  // ---- Alarms & sounds ----
  detailsOpen(message, "al", "Alarms & sounds", sec);
  rowEdit(message, "Snooze timeout", String(cfg.snooze_timeout) + " min", "alarmTiming", "al");
  rowEdit(message, "Repeat alarm every", String(cfg.alarm_repeat) + " min", "alarmTiming", "al");
  rowEdit(message, "Warning sound below", String(cfg.snd_warning*mult, decpl) + " " + sgvUnits, "sndAlarms", "al", "t");
  rowEdit(message, "Warning sound above", String(cfg.snd_warning_high*mult, decpl) + " " + sgvUnits, "sndAlarms", "al", "t");
  rowEdit(message, "Alarm sound below", String(cfg.snd_alarm*mult, decpl) + " " + sgvUnits, "sndAlarms", "al", "t");
  rowEdit(message, "Alarm sound above", String(cfg.snd_alarm_high*mult, decpl) + " " + sgvUnits, "sndAlarms", "al", "t");
  rowEdit(message, "No-readings warning", String(cfg.snd_no_readings) + " min", "sndAlarms", "al", "t");
  rowToggle(message, "Alarm on LOOP Error", cfg.snd_loop_error, "snd_loop_error", "al");
  rowToggle(message, "Start-up chime", cfg.snd_warning_at_startup, "snd_warning_at_startup", "al");
  rowToggle(message, "Test alarm at startup", cfg.snd_alarm_at_startup, "snd_alarm_at_startup", "al");
  rowEdit(message, "Warning sound volume", String(cfg.warning_volume) + "%", "sndAlarms", "al", "t");
  rowEdit(message, "Alarm sound volume", String(cfg.alarm_volume) + "%", "sndAlarms", "al", "t");
  message += "</details>\r\n";

  // ---- WiFi & network ----
  detailsOpen(message, "wf", "WiFi & network", sec);
  rowEdit(message, "Device name", cfg.deviceName, "deviceName", "wf");
  rowSeg(message, "Internal Web Server", !cfg.disable_web_server, "disable_web_server", "wf", "Enabled", "Disabled");
  int wlans_defined_count = 0;
  for(int i=0; i<10; i++) {
    if(cfg.wlanssid[i][0] != 0) {
      wlans_defined_count++;
      String val = "SSID='" + String(cfg.wlanssid[i]) + "'";
      if(cfg.wlanpass[i][0]!=0) {
        String stars;
        for(int j=0; j<(int)strlen(cfg.wlanpass[i]); j++)
          stars += "*";
        val += ", PASS='" + stars + "'";
      } else {
        val += ", no password (open)";
      }
      char lbl[10]; snprintf(lbl, sizeof(lbl), "wlan%d", i);
      rowEdit(message, lbl, val, "wlans", "wf");
    }
  }
  if (wlans_defined_count < 1) {
    rowEdit(message, "WiFi Configuration", "(none)", "wlans", "wf");
  }
  message += "</details>\r\n";

  // ---- Hardware add-ons ----
  detailsOpen(message, "hw", "Hardware add-ons", sec);
  {
    const char* ledOpts[4] = {"OFF","visualize sound","show warnings and alarms","light always"};
    const int ledVals[4] = {0,1,2,3};
    rowSelect(message, "LED strip mode", "LED_strip_mode", "hw", ledOpts, ledVals, 4, cfg.LED_strip_mode);
  }
  rowEdit(message, "LED strip pin", String(cfg.LED_strip_pin), "led_strip", "hw");
  rowEdit(message, "LED strip count", String(cfg.LED_strip_count), "led_strip", "hw");
  rowEdit(message, "LED strip brightness", String(cfg.LED_strip_brightness) + "%", "led_strip", "hw");
  message += "<div class=\"row\"><span>Test the side lights</span><span><a href=\"/lighttest\">Run the light test</a></span></div>\r\n";
  rowToggle(message, "Vibration motor unit", cfg.vibration_mode, "vibration_mode", "hw");
  rowEdit(message, "Vibration motor pin", String(cfg.vibration_pin), "vibration_motor", "hw");
  rowEdit(message, "Vibration motor strength", String(cfg.vibration_strength), "vibration_motor", "hw");
  rowToggle(message, "Micro Dot pHAT", cfg.micro_dot_pHAT, "micro_dot_pHAT", "hw");
  message += "</details>\r\n";

  // ---- System & firmware ----
  detailsOpen(message, "sy", "System & firmware", sec);
  rowEdit(message, "Time offset", String(cfg.timeZone) + " s", "timeZoneDST", "sy");
  rowEdit(message, "DST offset", String(cfg.dst) + " s", "timeZoneDST", "sy");
  rowEdit(message, "Restart at time", strcmp(cfg.restart_at_time,"NORES")==0?String("do NOT restart"):String(cfg.restart_at_time), "restartAt", "sy");
  rowEdit(message, "Restart after N errors", cfg.restart_at_logged_errors==0?String("do NOT restart"):String(cfg.restart_at_logged_errors), "restartAt", "sy");
  rowToggle(message, "Developer mode", cfg.dev_mode, "dev_mode", "sy", "Enabled", "Disabled");
  rowText(message, "Current firmware version", M5NSversion);
  message += "<div class=\"row\"><span></span><span><a href=\"/fwcheck\">Check for update</a></span></div>\r\n";
  message += "</details>\r\n";

  message += "</body>\r\n</html>\r\n";
  w3srv.send(200, "text/html; charset=utf-8", message);

  if(cfg.LED_strip_mode != 0) {
    pixels.show();
  }
}

// Firmware version/whatsnew check, split out of handleRoot() so the root page never
// blocks on a GitHub fetch (up to 2x5s timeout) - loads instantly, checked on demand.
void handleFwCheck() {
  String webVer;
  String whatsNew;
  bool fetchOk = false;
  int failCode = 0;   // 0 not on Wi-Fi, <0 couldn't connect, 404 nothing published yet, other HTTP error

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClientSecure updateClient;
    updateClient.setInsecure();

    http.begin(updateClient, updateURL("update.inf"));
    http.setTimeout(5000);
    http.setConnectTimeout(5000);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      webVer = http.getString();
      webVer.trim();
      fetchOk = true;
    } else {
      failCode = httpCode;
      Serial.printf("Error getting update.inf: %d\r\n", httpCode);
    }
    http.end();

    if (fetchOk) {
      http.begin(updateClient, updateURL("whatsnew.txt"));
      http.setTimeout(5000);
      http.setConnectTimeout(5000);
      httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        whatsNew = htmlEscape(http.getString());
        whatsNew.replace("\r\n", "<br>");
        whatsNew.replace("\n", "<br>");
      }
      http.end();
    }
  }

  String message;
  message.reserve(2048);
  pageHeadOpen(message, NULL);
  message += "<a class=\"bk\" href=\"/\">"; message += WG_BACK_EARLY; message += " All settings</a>\r\n";
  message += "<div class=\"card\"><h2>Check for updates</h2>\r\n";
  message += "<p>This Wee Glo: version <b>"; message += M5NSversion; message += "</b></p>\r\n";
  bool tryAgain = false;
  if (fetchOk && webVer > M5NSversion) {
    message += "<p class=\"ok\"><b>A new version is ready:</b> "; message += htmlEscape(webVer); message += "</p>\r\n";
    if (whatsNew.length()) { message += "<p class=\"small\"><b>What's new</b><br>"; message += whatsNew; message += "</p>\r\n"; }
    message += "<p class=\"small\">It takes about 2 minutes. Keep Wee Glo plugged in. It restarts by itself, and your settings stay as they are.</p>\r\n";
    message += "<div class=\"nav\"><a class=\"btn o\" href=\"/\">"; message += WG_BACK_EARLY; message += "Not now</a>"
               "<a class=\"btn g\" href=\"/update\" data-wait=\"Updating Wee Glo. This takes about 2 minutes. Keep it plugged in.\">Update Wee Glo</a></div>\r\n";
  } else if (fetchOk) {
    message += "<p class=\"ok\"><b>Wee Glo is up to date.</b> There's nothing to do.</p>\r\n";
  } else if (failCode == 404) {
    message += "<p><b>No updates have been published yet.</b> There's nothing to do. Check again another time.</p>\r\n";
  } else if (failCode == 0) {
    message += "<p class=\"bad\">Wee Glo isn't on your Wi-Fi right now, so it can't check. Set up Home Wi-Fi first.</p>\r\n";
  } else {
    message += "<p class=\"bad\">Wee Glo couldn't reach the update server. This is usually brief. Try again in a few minutes.</p>\r\n";
    tryAgain = true;
  }
  if (!(fetchOk && webVer > M5NSversion)) {
    message += "<div class=\"nav\"><a class=\"btn o\" href=\"/\">"; message += WG_BACK_EARLY; message += "All settings</a>";
    if (tryAgain) message += "<a class=\"btn\" href=\"/fwcheck\" data-wait=\"Checking for updates. This takes a few seconds.\">Try again</a>";
    message += "</div>\r\n";
  }
  message += "</div></main></body>\r\n</html>\r\n";
  w3srv.send(200, "text/html; charset=utf-8", message);
}

// ---- On-device OTA (no browser needed) ------------------------------------------
// Used by the config page (page 4, M5_NightscoutMon.ino): draw_page() checks once on
// page entry and caches the result, since draw_page() also re-runs every 15s from
// loop() and can't afford a network fetch on every redraw. The middle button then
// runs the update using the same WiFiClientSecure + httpUpdate path as handleUpdate()'s
// web /update route below, just without any HTML.
static String otaCachedVer = "";
static bool   otaCacheValid = false;

bool otaCheckLatest() {
  otaCacheValid = false;
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, updateURL("update.inf"));
  http.setTimeout(5000);
  http.setConnectTimeout(5000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    otaCachedVer = http.getString();
    otaCachedVer.trim();
    otaCacheValid = true;
  }
  http.end();
  return otaUpdateAvailable();
}

bool otaUpdateAvailable() {
  return otaCacheValid && (otaCachedVer > M5NSversion);
}

String otaLatestVersion() {
  return otaCachedVer;
}

void otaRunUpdate() {
  if (!otaUpdateAvailable()) return;

  Serial.print("Updating firmware, please wait ... ");
  M5.Display.setBrightness(255);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor(0, 18);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setFreeFont(FMB9);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.println("FIRMWARE UPDATE");
  M5.Lcd.println();
  M5.Lcd.print("Current firmware: "); M5.Lcd.println(M5NSversion);
  M5.Lcd.print("New firmware: "); M5.Lcd.println(otaCachedVer);
  M5.Lcd.println();
  M5.Lcd.println("Updating the firmware... ");
  M5.Lcd.println();

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return ret = httpUpdate.update(client, updateURL("M5_NightscoutMon.ino.bin"));

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      M5.Lcd.setTextColor(RED);
      M5.Lcd.println("UPDATE FAILED");
      delay(3000);
      M5.Lcd.fillScreen(BLACK);
      draw_page();
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP_UPDATE_NO_UPDATES");
      M5.Lcd.setTextColor(YELLOW);
      M5.Lcd.println("NO UPDATES");
      delay(3000);
      M5.Lcd.fillScreen(BLACK);
      draw_page();
      break;

    case HTTP_UPDATE_OK:
      Serial.println("HTTP_UPDATE_OK, restarting ...");
      M5.Lcd.setTextColor(GREEN);
      M5.Lcd.println("UPDATED SUCCESSFULLY");
      M5.Lcd.println("Restarting ...");
      M5.update();
      delay(1000);
      ESP.restart();
      break;
  }
}

void handleUpdate() {
  Serial.print("Updating firmware, please wait ... ");
  M5.Display.setBrightness(255);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor(0, 18);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setFreeFont(FMB9);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.println("FIRMWARE UPDATE");
  M5.Lcd.println();
  Serial.print("Free Heap: "); Serial.println(ESP.getFreeHeap());
  M5.Lcd.print("Free Heap: "); M5.Lcd.println(ESP.getFreeHeap());
  M5.Lcd.println();

  String message;
  message.reserve(1024);
  pageHeadOpen(message, "<meta http-equiv=\"refresh\" content=\"30;url=/\" />\r\n");

  String webVer;
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  if((WiFi.status() == WL_CONNECTED)) {
    http.begin(client, updateURL("update.inf"));
    int httpCode = http.GET();
    if(httpCode > 0) {
      if(httpCode == HTTP_CODE_OK) {
        webVer = http.getString();
      }
    }
  }

  M5.Lcd.print("Current firmware: "); M5.Lcd.println(M5NSversion);
  M5.Lcd.print("Found firmware:"); M5.Lcd.println(webVer);
  
  if(webVer > M5NSversion) {
    message += "<p>Updating firmware to version ";
    message += webVer;
    message += ", please wait ... </p>\r\n";
    message += "<p>Device will restart automatically.</p>\r\n";
    message += "</body>\r\n";
    message += "</html>\r\n";
    w3srv.send(200, "text/html; charset=utf-8", message);

    M5.Lcd.println();
    M5.Lcd.println("Updating the firmware... ");
    M5.Lcd.println();
    httpUpdate.rebootOnUpdate(false);
    t_httpUpdate_return ret = httpUpdate.update(client, updateURL("M5_NightscoutMon.ino.bin"));
    //t_httpUpdate_return ret = httpUpdate.update(client, "server", 80, "file.bin");

    switch (ret) {
      case HTTP_UPDATE_FAILED:
        Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        M5.Lcd.setTextColor(RED);
        M5.Lcd.println("UPDATE FAILED");
        break;

      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("HTTP_UPDATE_NO_UPDATES");
        M5.Lcd.setTextColor(YELLOW);
        M5.Lcd.println("NO UPDATES");
        break;

      case HTTP_UPDATE_OK:
        Serial.println("HTTP_UPDATE_OK, restarting ...");
        M5.Lcd.setTextColor(GREEN);
        M5.Lcd.println("UPDATED SUCCESSFULLY");
        M5.Lcd.println("Restarting ...");

        M5.update();
        delay(1000);
        ESP.restart();
        break;
    }    
  } else {
    message += "<p>Nothing to update. Firmware version ";
    message += webVer;
    message += " is current.</p>\r\n";
    message += "</body>\r\n";
    message += "</html>\r\n";
    w3srv.send(200, "text/html; charset=utf-8", message);

    Serial.println("Nothing to update");
    M5.Lcd.println();
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.println("NOTHING TO UPDATE");
  }
  if (!cfg.is_task_bootstrapping) {
    M5.update();
    delay(2000);
    M5.Lcd.fillScreen(BLACK);
    draw_page();
    if(cfg.LED_strip_mode != 0) { 
      pixels.show();
    }
  }
}

void handleSwitchConfig() {
  // Multi-value settings accept an explicit "v" (from the root page's <select> dropdowns
  // and [Seg] two-way buttons) and set that value directly; without "v" they fall back to
  // the old cycle-to-next behavior, so existing bookmarked/typed switch links keep working.
  // Yes/No settings always just toggle. "s" carries the section id back to handleRoot() so
  // only the section just touched stays expanded.
  bool haveVal = w3srv.hasArg("v");
  long val = haveVal ? String(w3srv.arg("v")).toInt() : 0;
  String sec = w3srv.hasArg("s") ? w3srv.arg("s") : String("st");

  // Turning the internal web server off kills this page mid-session: loop() stops calling
  // handleClient() as soon as the flag flips, so the redirect back to "/" never loads and the
  // Save button becomes unreachable. That means the change can never actually be persisted from
  // here (a restart restores it from M5NS.INI/flash) - but the page simply going dead looks like
  // a crash, so say what will happen first. "ok=1" is the confirmed request coming back.
  if (w3srv.hasArg("param") && String(w3srv.arg("param")).equals("disable_web_server")
      && !w3srv.hasArg("ok")) {
    bool wouldDisable = haveVal ? (val == 0) : (cfg.disable_web_server == 0);
    if (wouldDisable) {
      String message;
      message.reserve(1536);
      pageHeadOpen(message, NULL);
      message += "<h2 class=\"warn\">Disable the internal web server?</h2>\r\n";
      message += "<p>This page <b>is</b> the internal web server. It will stop responding "
                 "immediately - this is expected, not a crash.</p>\r\n";
      message += "<p>The change is <b>not saved</b>: you will not be able to reach the Save "
                 "button, so simply <b>restart the device</b> and the web server comes back.</p>\r\n";
      message += "<p>To turn it off permanently, set <code>disable_web_server = 1</code> in "
                 "<code>M5NS.INI</code> on the SD card. You can still get back in by restarting "
                 "while holding the left button (or tapping CONFIG on touch models) to enter "
                 "setup mode, which always serves this page.</p>\r\n";
      message += "<p><a class=\"btn2\" href=\"switch?param=disable_web_server&v=0&ok=1&s=";
      message += sec; message += "\">Disable anyway</a>\r\n";
      message += "<a class=\"btn\" href=\"/all?s="; message += sec; message += "\">Keep it enabled</a></p>\r\n";
      message += "</body>\r\n</html>\r\n";
      w3srv.send(200, "text/html; charset=utf-8", message);
      return;
    }
  }

  for (uint8_t i = 0; i < w3srv.args(); i++) {
    if(String(w3srv.argName(i)).equals("param")) {
      String param = w3srv.arg(i);
      if(param.equals("data_source")) {
        int oldSource = cfg.data_source;
        if(haveVal && val>=0 && val<=3)
          cfg.data_source = val;
        else {
          cfg.data_source++;
          if(cfg.data_source > 3)
            cfg.data_source = 0;
        }
        // Only one source is ever active - switching takes effect after the restart
        // that Save triggers, same as device name / WiFi changes.
        if(cfg.data_source != oldSource) restartPending = true;
        dexcomResetSession();
        libreResetSession();
        careLinkResetSession();
      }
      else if(param.equals("dexcom_server")) {
        if(haveVal && val>=0 && val<=2)
          cfg.dexcom_server = val;
        else {
          cfg.dexcom_server++;
          if(cfg.dexcom_server > 2)
            cfg.dexcom_server = 0;
        }
        dexcomResetSession();
      }
      else if(param.equals("libre_server")) {
        if(haveVal && val>=0 && val<=12)
          cfg.libre_server = val;
        else {
          cfg.libre_server++;
          if(cfg.libre_server > 12)
            cfg.libre_server = 0;
        }
        libreResetSession();
      }
      else if(param.equals("show_mgdl")) {
        if(haveVal) cfg.show_mgdl = (val!=0);
        else cfg.show_mgdl = !cfg.show_mgdl;
      }
      else if(param.equals("sgv_only")) {
        cfg.sgv_only = !cfg.sgv_only;
      }
      else if(param.equals("default_page")) {
        if(haveVal && val>=0 && val<=maxPage)
          cfg.default_page = val;
        else {
          cfg.default_page++;
          if(cfg.default_page>maxPage)
            cfg.default_page = 0;
        }
        dispPage = cfg.default_page;
        setPageIconPos(dispPage);
      }
      else if(param.equals("show_current_time")) {
        if(haveVal) cfg.show_current_time = (val!=0);
        else cfg.show_current_time = !cfg.show_current_time;
      }
      else if(param.equals("show_COB_IOB")) {
        cfg.show_COB_IOB = !cfg.show_COB_IOB;
      }
      else if(param.equals("snd_loop_error")) {
        cfg.snd_loop_error = !cfg.snd_loop_error;
      }
      else if(param.equals("snd_warning_at_startup")) {
        cfg.snd_warning_at_startup = !cfg.snd_warning_at_startup;
      }
      else if(param.equals("snd_alarm_at_startup")) {
        cfg.snd_alarm_at_startup = !cfg.snd_alarm_at_startup;
      }
      else if(param.equals("info_line")) {
        if(haveVal && val>=0 && val<=3)
          cfg.info_line = val;
        else {
          cfg.info_line++;
          if(cfg.info_line>3)
            cfg.info_line = 0;
        }
      }
      else if(param.equals("time_format")) {
        if(haveVal && val>=0 && val<=1)
          cfg.time_format = val;
        else {
          cfg.time_format++;
          if(cfg.time_format>1)
            cfg.time_format = 0;
        }
      }
      else if(param.equals("date_format")) {
        if(haveVal && val>=0 && val<=1)
          cfg.date_format = val;
        else {
          cfg.date_format++;
          if(cfg.date_format>1)
            cfg.date_format = 0;
        }
      }
      else if(param.equals("display_rotation")) {
        if(haveVal && (val==1 || val==3 || val==5 || val==7)) {
          cfg.display_rotation = val;
        } else if(cfg.display_rotation == 1) {
          cfg.display_rotation = 3;
        } else if(cfg.display_rotation == 3) {
          cfg.display_rotation = 5;
        } else if(cfg.display_rotation == 5) {
          cfg.display_rotation = 7;
        } else {
          cfg.display_rotation = 1;
        }
        M5.Lcd.setRotation(cfg.display_rotation);
      }
      else if(param.equals("invert_display")) {
        if(haveVal && val>=-1 && val<=1)
          cfg.invert_display = val;
        else {
          cfg.invert_display++;
          if(cfg.invert_display>1)
            cfg.invert_display = -1;
        }
        if(cfg.invert_display != -1)
          M5.Lcd.invertDisplay(cfg.invert_display);
      }
      else if(param.equals("temperature_unit")) {
        if(haveVal && val>=1 && val<=3)
          cfg.temperature_unit = val;
        else {
          cfg.temperature_unit++;
          if(cfg.temperature_unit>3)
            cfg.temperature_unit = 1;
        }
      }
      else if(param.equals("LED_strip_mode")) {
        if(haveVal && val>=0 && val<=3)
          cfg.LED_strip_mode = val;
        else {
          cfg.LED_strip_mode++;
          if(cfg.LED_strip_mode>3)
            cfg.LED_strip_mode = 0;
        }
        if(cfg.LED_strip_mode==0) {
          pixels.clear();
          pixels.show();
        }
      }
      else if(param.equals("vibration_mode")) {
        cfg.vibration_mode++;
        if(cfg.vibration_mode>1) {
          cfg.vibration_mode = 0;
        }
      }
      else if(param.equals("micro_dot_pHAT")) {
        cfg.micro_dot_pHAT++;
        if(cfg.micro_dot_pHAT>1) {
          cfg.micro_dot_pHAT = 0;
          MD.writeString("      ");
        } else {
          MD.begin();
          MD.writeString("*M5NS*");
        }
      }
      else if(param.equals("dev_mode")) {
        cfg.dev_mode = !cfg.dev_mode;
      }
      else if(param.equals("disable_web_server")) {
        // rowSeg's "v" is 1=Enabled/0=Disabled (the state shown to the user), which is the
        // inverse of disable_web_server itself.
        if(haveVal) cfg.disable_web_server = (val==0);
        else cfg.disable_web_server = !cfg.disable_web_server;
      }
    }
  }

  // Instant redirect instead of the old 1s meta-refresh page - dropdown/toggle changes
  // feel immediate and each switch no longer costs ~0.5KB of throwaway HTML. Carries the
  // section id back so only the section just changed stays expanded.
  w3srv.sendHeader("Location", "/all?s=" + sec);
  w3srv.send(303, "text/plain", "");

  if (!cfg.is_task_bootstrapping) {
    M5.Lcd.fillScreen(BLACK);
    draw_page();
    if(cfg.LED_strip_mode != 0) {
      pixels.show();
    }
  }
}

void handleEditConfigItem() {
  String sgvUnits = cfg.show_mgdl?"mg/dL":"mmol/L";
  int decpl = cfg.show_mgdl?0:1;
  int mult = cfg.show_mgdl?18:1;
  String sec = w3srv.hasArg("s") ? w3srv.arg("s") : String("st");

  String message;
  message.reserve(2048);
  pageHeadOpen(message, NULL);
  message += "<p>Edit configuration item.</p>\r\n";
  message += "<form action=\"/getedititem\" method=\"post\" accept-charset=\"utf-8\">\r\n";
  message += "<input type=\"hidden\" name=\"s\" value=\""; message += sec; message += "\">\r\n";
  if(String(w3srv.argName(0)).equals("param")) {
    if(String(w3srv.arg(0)).equals("userName")) {
      editRow(message, "User name", "<input type=\"text\" name=\"" + String(w3srv.arg(0)) + "\" value=\"" + String(cfg.userName) + "\" size=\"20\" maxlength=\"32\">");
    }
    if(String(w3srv.arg(0)).equals("NSurl")) {
      editRow(message, "Nightscout URL", "<input type=\"text\" name=\"url\" value=\"" + String(cfg.url) + "\" size=\"28\" maxlength=\"128\">", "e.g. sitename.herokuapp.com");
      editRow(message, "Security token", "<input type=\"text\" name=\"token\" value=\"" + String(cfg.token) + "\" size=\"28\" maxlength=\"64\">", "empty if not used");
    }
    if(String(w3srv.arg(0)).equals("dexcom")) {
      editRow(message, "Dexcom account", "<input type=\"text\" name=\"dexcom_user\" value=\"" + String(cfg.dexcom_user) + "\" size=\"28\" maxlength=\"63\">", "use the account that SHARES glucose data, not a follower account");
      editRow(message, "Dexcom password", "<input type=\"password\" name=\"dexcom_pass\" value=\"" + String(cfg.dexcom_pass) + "\" size=\"28\" maxlength=\"63\">", "stored on the device SD card / flash in plain text");
    }
    if(String(w3srv.arg(0)).equals("libre")) {
      editRow(message, "LibreLinkUp email", "<input type=\"text\" name=\"libre_user\" value=\"" + String(cfg.libre_user) + "\" size=\"28\" maxlength=\"63\">", "use a LibreLinkUp follower account, not the LibreView account itself");
      editRow(message, "LibreLinkUp password", "<input type=\"password\" name=\"libre_pass\" value=\"" + String(cfg.libre_pass) + "\" size=\"28\" maxlength=\"63\">", "stored on the device SD card / flash in plain text");
    }
    if(String(w3srv.arg(0)).equals("carelink")) {
      editRow(message, "CareLink country", "<input type=\"text\" name=\"cl_country\" value=\"" + String(cfg.cl_country) + "\" size=\"3\" maxlength=\"2\">", "two-letter code of the country the CareLink account is in, e.g. IE, GB, DE, US");
      editRow(message, "Patient to follow", "<input type=\"text\" name=\"cl_patient\" value=\"" + String(cfg.cl_patient) + "\" size=\"28\" maxlength=\"63\">", "the patient's CareLink username; leave empty to follow the first linked patient");
    }
    if(String(w3srv.arg(0)).equals("deviceName")) {
      editRow(message, "Device name", "<input type=\"text\" name=\"deviceName\" value=\"" + String(cfg.deviceName) + "\" size=\"12\" maxlength=\"32\">.local");
      message += "<p class=\"warn\">Applied after Save - the device will restart automatically.</p>\r\n";
    }
    if(String(w3srv.arg(0)).equals("timeZoneDST")) {
      editRow(message, "Time zone offset", "<input type=\"text\" name=\"timeZone\" value=\"" + String(cfg.timeZone) + "\" size=\"6\" maxlength=\"6\"> s", "e.g. 3600 for most of Europe - see epochconverter.com/timezones");
      editRow(message, "DST offset", "<input type=\"text\" name=\"dst\" value=\"" + String(cfg.dst) + "\" size=\"6\" maxlength=\"6\"> s", "usually 0 in winter, 3600 in summer");
      message += "<p class=\"warn\">Applied after Save - the device will restart automatically.</p>\r\n";
    }
    if(String(w3srv.arg(0)).equals("restartAt")) {
      editRow(message, "Restart at time", "<input type=\"text\" name=\"restart_at_time\" value=\"" + String(cfg.restart_at_time) + "\" size=\"5\" maxlength=\"5\">", "24h HH:MM, or NORES for never");
      editRow(message, "Restart after errors", "<input type=\"text\" name=\"restart_at_logged_errors\" value=\"" + String(cfg.restart_at_logged_errors) + "\" size=\"5\" maxlength=\"5\">", "0 = never");
    }
    if(String(w3srv.arg(0)).equals("alarmTiming")) {
      editRow(message, "Snooze timeout", "<input type=\"text\" name=\"snooze_timeout\" value=\"" + String(cfg.snooze_timeout) + "\" size=\"5\" maxlength=\"5\"> min");
      editRow(message, "Repeat alarm every", "<input type=\"text\" name=\"alarm_repeat\" value=\"" + String(cfg.alarm_repeat) + "\" size=\"5\" maxlength=\"5\"> min", "0 = as fast as it goes");
    }
    if(String(w3srv.arg(0)).equals("dispColors")) {
      editRow(message, "Yellow below", "<input type=\"text\" name=\"yellow_low\" value=\"" + String(cfg.yellow_low*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "y");
      editRow(message, "Yellow above", "<input type=\"text\" name=\"yellow_high\" value=\"" + String(cfg.yellow_high*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "y");
      editRow(message, "Red below", "<input type=\"text\" name=\"red_low\" value=\"" + String(cfg.red_low*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "r");
      editRow(message, "Red above", "<input type=\"text\" name=\"red_high\" value=\"" + String(cfg.red_high*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "r");
    }
    if(String(w3srv.arg(0)).equals("sndAlarms")) {
      editRow(message, "Warning sound below", "<input type=\"text\" name=\"snd_warning\" value=\"" + String(cfg.snd_warning*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "t");
      editRow(message, "Warning sound above", "<input type=\"text\" name=\"snd_warning_high\" value=\"" + String(cfg.snd_warning_high*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "t");
      editRow(message, "Alarm sound below", "<input type=\"text\" name=\"snd_alarm\" value=\"" + String(cfg.snd_alarm*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "t");
      editRow(message, "Alarm sound above", "<input type=\"text\" name=\"snd_alarm_high\" value=\"" + String(cfg.snd_alarm_high*mult, decpl) + "\" size=\"5\" maxlength=\"5\"> " + sgvUnits, NULL, "t");
      editRow(message, "No-readings warning", "<input type=\"text\" name=\"snd_no_readings\" value=\"" + String(cfg.snd_no_readings) + "\" size=\"5\" maxlength=\"5\"> min", NULL, "t");
      editRow(message, "Warning sound volume", "<input type=\"text\" name=\"warning_volume\" value=\"" + String(cfg.warning_volume) + "\" size=\"5\" maxlength=\"5\"> %", NULL, "t");
      editRow(message, "Alarm sound volume", "<input type=\"text\" name=\"alarm_volume\" value=\"" + String(cfg.alarm_volume) + "\" size=\"3\" maxlength=\"3\"> %", NULL, "t");
    }
    if(String(w3srv.arg(0)).equals("brightness")) {
      editRow(message, "Step 1 (default)", "<input type=\"text\" name=\"brightness1\" value=\"" + String(cfg.brightness1) + "\" size=\"3\" maxlength=\"3\"> %");
      editRow(message, "Step 2", "<input type=\"text\" name=\"brightness2\" value=\"" + String(cfg.brightness2) + "\" size=\"3\" maxlength=\"3\"> %");
      editRow(message, "Step 3", "<input type=\"text\" name=\"brightness3\" value=\"" + String(cfg.brightness3) + "\" size=\"3\" maxlength=\"3\"> %");
    }
    if(String(w3srv.arg(0)).equals("led_strip")) {
      editRow(message, "LED strip pin", "<input type=\"text\" name=\"LED_strip_pin\" value=\"" + String(cfg.LED_strip_pin) + "\" size=\"3\" maxlength=\"3\">", "15 = Fire internal, 26 = PORT B, 17 = PORT C (21/PORT A collides with I2C)");
      editRow(message, "LED strip count", "<input type=\"text\" name=\"LED_strip_count\" value=\"" + String(cfg.LED_strip_count) + "\" size=\"3\" maxlength=\"3\">");
      editRow(message, "LED strip brightness", "<input type=\"text\" name=\"LED_strip_brightness\" value=\"" + String(cfg.LED_strip_brightness) + "\" size=\"3\" maxlength=\"3\"> %", "use 5-10% for more than 10 LEDs, or load current will crash the M5Stack");
    }
    if(String(w3srv.arg(0)).equals("vibration_motor")) {
      editRow(message, "Vibration motor pin", "<input type=\"text\" name=\"vibration_pin\" value=\"" + String(cfg.vibration_pin) + "\" size=\"3\" maxlength=\"3\">", "26 = PORT B, 17 = PORT C - do not use 21/PORT A (collides with I2C)");
      editRow(message, "Vibration strength", "<input type=\"text\" name=\"vibration_strength\" value=\"" + String(cfg.vibration_strength) + "\" size=\"3\" maxlength=\"3\">", "0-1023, recommended 256-512 - higher can crash the M5Stack");
    }
    if(String(w3srv.arg(0)).equals("wlans")) {
      // Only scan here, not on every edit-item page: on ESP32 an AP-mode scan needs to
      // channel-hop, which can knock already-connected SoftAP clients (like the phone
      // filling out this very form) off the network mid-scan.
      int numSsids = WiFi.scanNetworks();
      // A <datalist> only offers scanned networks as suggestions - unlike a <select>, it
      // still lets you type any SSID, so hidden or out-of-range networks can be entered too.
      message += "<datalist id=\"wlanscan\">";
      for (int j=0; j<numSsids; j++) {
        message += "<option value=\"" + String(WiFi.SSID(j)) + "\">";
      }
      message += "</datalist>\r\n";
      for(int i=0; i<10; i++) {
        String ssidInput = "<input type=\"text\" name=\"wlanssid" + String(i) + "\" list=\"wlanscan\" value=\"" + String(cfg.wlanssid[i]) + "\" size=\"12\" maxlength=\"63\" placeholder=\"SSID (or type a hidden/out-of-range one)\">";
        ssidInput += " <input type=\"password\" name=\"wlanpass" + String(i) + "\" value=\"" + String(cfg.wlanpass[i]) + "\" size=\"12\" maxlength=\"63\" placeholder=\"password\">";
        char lbl[10]; snprintf(lbl, sizeof(lbl), "wlan%d", i);
        editRow(message, lbl, ssidInput);
      }
      message += "<p class=\"warn\">Applied after Save - the device will restart automatically.</p>\r\n";
    }
  }
  message += "<div style=\"margin-top:10px\">\r\n";
  message += "<input type=\"submit\" class=\"btn\" name=\"Submit\" value=\"Apply\">\r\n";
  message += "<input type=\"button\" class=\"btn2\" name=\"Cancel\" value=\"Cancel\" onClick=\"window.location.href='/all?s=" + sec + "';\" />\r\n";
  message += "</div>\r\n";
  message += "</form>\r\n";
  message += "</body>\r\n";
  message += "</html>\r\n";
  w3srv.send(200, "text/html; charset=utf-8", message);
  if(cfg.LED_strip_mode != 0) {
    pixels.show();
  }
}

void handleGetEditConfigItem() {
  // String sgvUnits = cfg.show_mgdl?"mg/dL":"mmol/L";
  // int decpl = cfg.show_mgdl?0:1;
  float mult = cfg.show_mgdl?18.0:1.0;
  String tmpStr;
  bool wlanChanged = false;

  String sec = "st";
  for (uint8_t i = 0; i < w3srv.args(); i++) {
    if(String(w3srv.argName(i)).equals("s")) {
      sec = String(w3srv.arg(i));
    }
  }

  String message;
  message.reserve(768);
  String meta = "<meta http-equiv=\"refresh\" content=\"1;url=/all?s=" + sec + "\" />\r\n";
  pageHeadOpen(message, meta.c_str());
  message += "<p>Updating edited items in M5Stack Nightscout monitor configuration (NOT saving to M5NS.INI file).</p>\r\n";
  for (uint8_t i = 0; i < w3srv.args(); i++) {
    if(String(w3srv.argName(i)).equals("userName")) {
      strncpy(cfg.userName, String(w3srv.arg(i)).c_str(), 32);
    }
    if(String(w3srv.argName(i)).equals("url")) {
      strncpy(cfg.url, String(w3srv.arg(i)).c_str(), 128);
    }
    if(String(w3srv.argName(i)).equals("token")) {
      strncpy(cfg.token, String(w3srv.arg(i)).c_str(), 64);
    }
    if(String(w3srv.argName(i)).equals("dexcom_user")) {
      strncpy(cfg.dexcom_user, String(w3srv.arg(i)).c_str(), 64);
      dexcomResetSession();
    }
    if(String(w3srv.argName(i)).equals("dexcom_pass")) {
      strncpy(cfg.dexcom_pass, String(w3srv.arg(i)).c_str(), 64);
      dexcomResetSession();
    }
    if(String(w3srv.argName(i)).equals("libre_user")) {
      strncpy(cfg.libre_user, String(w3srv.arg(i)).c_str(), 64);
      libreResetSession();
    }
    if(String(w3srv.argName(i)).equals("libre_pass")) {
      strncpy(cfg.libre_pass, String(w3srv.arg(i)).c_str(), 64);
      libreResetSession();
    }
    if(String(w3srv.argName(i)).equals("cl_country")) {
      String c = String(w3srv.arg(i));
      c.trim();
      c.toUpperCase();
      strlcpy(cfg.cl_country, c.c_str(), 4);
      careLinkResetSession();
    }
    if(String(w3srv.argName(i)).equals("cl_patient")) {
      String pt = String(w3srv.arg(i));
      pt.trim();
      strlcpy(cfg.cl_patient, pt.c_str(), 64);
      careLinkResetSession();
    }
    if(String(w3srv.argName(i)).equals("deviceName")) {
      String newVal = String(w3srv.arg(i));
      if(!newVal.equals(cfg.deviceName)) restartPending = true;
      strncpy(cfg.deviceName, newVal.c_str(), 32);
    }
    if(String(w3srv.argName(i)).equals("timeZone")) {
      long newVal = String(w3srv.arg(i)).toInt();
      if(newVal != cfg.timeZone) restartPending = true;
      cfg.timeZone = newVal;
    }
    if(String(w3srv.argName(i)).equals("dst")) {
      long newVal = String(w3srv.arg(i)).toInt();
      if(newVal != cfg.dst) restartPending = true;
      cfg.dst = newVal;
    }
    if(String(w3srv.argName(i)).equals("restart_at_time")) {
      strncpy(cfg.restart_at_time, String(w3srv.arg(i)).c_str(), 10);
    }
    if(String(w3srv.argName(i)).equals("restart_at_logged_errors")) {
      cfg.restart_at_logged_errors = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("snooze_timeout")) {
      cfg.snooze_timeout = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("alarm_repeat")) {
      cfg.alarm_repeat = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("yellow_low")) {
      cfg.yellow_low = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("yellow_high")) {
      cfg.yellow_high = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("red_low")) {
      cfg.red_low = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("red_high")) {
      cfg.red_high = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("snd_warning")) {
      cfg.snd_warning = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("snd_warning_high")) {
      cfg.snd_warning_high = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("snd_alarm")) {
      cfg.snd_alarm = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("snd_alarm_high")) {
      cfg.snd_alarm_high = String(w3srv.arg(i)).toFloat()/mult;
    }
    if(String(w3srv.argName(i)).equals("snd_no_readings")) {
      cfg.snd_no_readings = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("warning_volume")) {
      cfg.warning_volume = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("alarm_volume")) {
      cfg.alarm_volume = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("brightness1")) {
      if(lcdBrightness==cfg.brightness1) {
        // changing selected brightness, so update it
        lcdBrightness = String(w3srv.arg(i)).toInt();
        M5.Display.setBrightness(map(lcdBrightness, 0, 100, 0, 255));
      }
      cfg.brightness1 = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("brightness2")) {
      if(lcdBrightness==cfg.brightness2) {
        // changing selected brightness, so update it
        lcdBrightness = String(w3srv.arg(i)).toInt();
        M5.Display.setBrightness(map(lcdBrightness, 0, 100, 0, 255));
      }
      cfg.brightness2 = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("brightness3")) {
      if(lcdBrightness==cfg.brightness3) {
        // changing selected brightness, so update it
        lcdBrightness = String(w3srv.arg(i)).toInt();
        M5.Display.setBrightness(map(lcdBrightness, 0, 100, 0, 255));
      }
      cfg.brightness3 = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("LED_strip_pin")) {
      int oldpin = cfg.LED_strip_pin;
      cfg.LED_strip_pin = halSafeLedPin(String(w3srv.arg(i)).toInt());
      if(oldpin != cfg.LED_strip_pin) {
        pixels.clear();
        pixels.show();
        pixels.setPin(cfg.LED_strip_pin);
      }
    }
    if(String(w3srv.argName(i)).equals("LED_strip_count")) {
      int oldcnt = cfg.LED_strip_count;
      cfg.LED_strip_count = String(w3srv.arg(i)).toInt();
      if(oldcnt != cfg.LED_strip_count) {
        pixels.clear();
        pixels.show();
        pixels.updateLength(cfg.LED_strip_count);
      }
    }
    if(String(w3srv.argName(i)).equals("LED_strip_brightness")) {
      cfg.LED_strip_brightness = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("vibration_pin")) {
      cfg.vibration_pin = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).equals("vibration_strength")) {
      cfg.vibration_strength = String(w3srv.arg(i)).toInt();
    }
    if(String(w3srv.argName(i)).startsWith("wlanssid")) {
      tmpStr = String(w3srv.argName(i));
      tmpStr.remove(0, 8);
      int nr = tmpStr.toInt();
      String newVal = String(w3srv.arg(i));
      if(!newVal.equals(cfg.wlanssid[nr])) { restartPending = true; wlanChanged = true; }
      message += "WLAN SSID [" + tmpStr + "] = " + newVal + " (" + newVal.length() + ")<br />\r\n";
      strncpy(cfg.wlanssid[nr], newVal.c_str(), 32);
    }
    if(String(w3srv.argName(i)).startsWith("wlanpass")) {
      tmpStr = String(w3srv.argName(i));
      tmpStr.remove(0, 8);
      int nr = tmpStr.toInt();
      String newVal = String(w3srv.arg(i));
      if(!newVal.equals(cfg.wlanpass[nr])) { restartPending = true; wlanChanged = true; }

      // message += "WLAN PASS [" + tmpStr + "] = " + String(w3srv.arg(i)) + " (" + String(w3srv.arg(i)).length() + ")<br />\r\n";

      message += "WLAN PASS [" + tmpStr + "] = ";
      for(int j=0; j< String(w3srv.arg(i)).length(); j++)
        message += "*";
      message += " (" + String(w3srv.arg(i).length()) + ")<br />\r\n";
      strncpy(cfg.wlanpass[nr], String(w3srv.arg(i)).c_str(), 64);
    }
  }

  // WiFi networks can't wait for a separate trip to the "Save configuration" button like every
  // other setting: losing an entered network here traps the user in an endless re-bootstrap
  // loop (a fresh random SoftAP passphrase every reboot, no way back to what was just typed).
  // So in bootstrap mode, persist to SD + flash immediately and tell the user plainly that a
  // restart is what's needed next - this also means the credentials are safely on disk before
  // the HTTP response is even sent, in case the page itself never finishes loading.
  if(wlanChanged && cfg.is_task_bootstrapping) {
    persistConfigToDisk();
    message += "<p><b>WiFi settings saved.</b> Restart the device now to connect using the new network.</p>\r\n";
  }

  message += "</body>\r\n";
  message += "</html>\r\n";
  w3srv.send(200, "text/html; charset=utf-8", message);

  if (!cfg.is_task_bootstrapping) {
    M5.Lcd.fillScreen(BLACK);
    draw_page();
    if(cfg.LED_strip_mode != 0) {
      pixels.show();
    }
  }
}

// Full SD M5NS.INI (with a M5NS.BAK backup) + NVS flash write of the current in-RAM cfg.
// Shared by handleSaveConfig() (the explicit "Save" button) and, in bootstrap mode,
// handleGetEditConfigItem() (WiFi networks only - see the call site there for why).
static void persistConfigToDisk() {
  String sgvUnits = cfg.show_mgdl?"mg/dL":"mmol/L";
  int decpl = cfg.show_mgdl?0:1;
  int mult = cfg.show_mgdl?18:1;

  M5.Lcd.drawJpgFile(SD, cfg.bootPic);

  File srcFil, dstFil;

  Serial.println("Creating backup copy of M5NS.INI to M5NS.BAK");

  if(!SD.remove("/M5NS.BAK")) {
    Serial.println("Error removing M5NS.BAK");
  }
  srcFil = SD.open("/M5NS.INI", FILE_READ);
  if(!srcFil) {
    Serial.println("Error opening M5NS.INI for read");
  } else {
    dstFil = SD.open("/M5NS.BAK", FILE_WRITE);
    if(!dstFil) {
      Serial.println("Error opening M5NS.BAK for write");
    } else {
      if(srcFil.size()>0) {
        size_t n;
        uint8_t buf[256];
        while ((n = srcFil.read(buf, sizeof(buf))) > 0) {
          dstFil.write(buf, n);
        }
        srcFil.close();
        dstFil.close();
        Serial.println("INI to BAK should be copied");
      } else {
        Serial.println("Source file M5NS.INI is empty.");
      }
    }
  }

  if(!SD.remove("/M5NS.INI")) {
    Serial.println("Error removing M5NS.BAK");
  }
  dstFil = SD.open("/M5NS.INI", FILE_WRITE);
  if(!dstFil) {
    Serial.println("Error opening M5NS.INI for write");
  } else {
    Serial.println("Writing configuration to M5NS.INI file on SD card");
    dstFil.print("[config]\r\n");
    dstFil.print("nightscout = "); dstFil.print(cfg.url); dstFil.print("\r\n");
    dstFil.print("token = "); dstFil.print(cfg.token); dstFil.print("\r\n");
    dstFil.print("data_source = "); dstFil.print(cfg.data_source); dstFil.print("\r\n");
    dstFil.print("dexcom_user = "); dstFil.print(cfg.dexcom_user); dstFil.print("\r\n");
    dstFil.print("dexcom_pass = "); dstFil.print(cfg.dexcom_pass); dstFil.print("\r\n");
    dstFil.print("dexcom_server = "); dstFil.print(cfg.dexcom_server); dstFil.print("\r\n");
    dstFil.print("libre_user = "); dstFil.print(cfg.libre_user); dstFil.print("\r\n");
    dstFil.print("libre_pass = "); dstFil.print(cfg.libre_pass); dstFil.print("\r\n");
    dstFil.print("libre_server = "); dstFil.print(cfg.libre_server); dstFil.print("\r\n");
    dstFil.print("cl_country = "); dstFil.print(cfg.cl_country); dstFil.print("\r\n");
    dstFil.print("cl_patient = "); dstFil.print(cfg.cl_patient); dstFil.print("\r\n");
    dstFil.print("bootpic = "); dstFil.print(cfg.bootPic); dstFil.print("\r\n");
    dstFil.print("name = "); dstFil.print(cfg.userName); dstFil.print("\r\n");
    dstFil.print("device_name = "); dstFil.print(cfg.deviceName); dstFil.print("\r\n");
    dstFil.print("time_zone = "); dstFil.print(cfg.timeZone); dstFil.print("\r\n");
    dstFil.print("dst = "); dstFil.print(cfg.dst); dstFil.print("\r\n");
    dstFil.print("show_mgdl = "); dstFil.print(cfg.show_mgdl); dstFil.print("\r\n");
    dstFil.print("show_current_time = "); dstFil.print(cfg.show_current_time); dstFil.print("\r\n");
    dstFil.print("show_COB_IOB = "); dstFil.print(cfg.show_COB_IOB); dstFil.print("\r\n");
    dstFil.print("default_page = "); dstFil.print(cfg.default_page); dstFil.print("\r\n");
    dstFil.print("restart_at_time = "); dstFil.print(cfg.restart_at_time); dstFil.print("\r\n");
    dstFil.print("restart_at_logged_errors = "); dstFil.print(cfg.restart_at_logged_errors); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("snooze_timeout = "); dstFil.print(cfg.snooze_timeout); dstFil.print("\r\n");
    dstFil.print("alarm_repeat = "); dstFil.print(cfg.alarm_repeat); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("yellow_low = "); dstFil.print(String(cfg.yellow_low*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("yellow_high = "); dstFil.print(String(cfg.yellow_high*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("red_low = "); dstFil.print(String(cfg.red_low*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("red_high = "); dstFil.print(String(cfg.red_high*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("snd_warning = "); dstFil.print(String(cfg.snd_warning*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("snd_alarm = "); dstFil.print(String(cfg.snd_alarm*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("snd_warning_high = "); dstFil.print(String(cfg.snd_warning_high*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("snd_alarm_high = "); dstFil.print(String(cfg.snd_alarm_high*mult, decpl)); dstFil.print("\r\n");
    dstFil.print("snd_no_readings = "); dstFil.print(cfg.snd_no_readings); dstFil.print("\r\n");
    dstFil.print("snd_loop_error = "); dstFil.print(cfg.snd_loop_error); dstFil.print("\r\n");
    dstFil.print("snd_warning_at_startup = "); dstFil.print(cfg.snd_warning_at_startup); dstFil.print("\r\n");
    dstFil.print("snd_alarm_at_startup = "); dstFil.print(cfg.snd_alarm_at_startup); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("warning_volume = "); dstFil.print(cfg.warning_volume); dstFil.print("\r\n");
    dstFil.print("alarm_volume = "); dstFil.print(cfg.alarm_volume); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("brightness1 = "); dstFil.print(cfg.brightness1); dstFil.print("\r\n");
    dstFil.print("brightness2 = "); dstFil.print(cfg.brightness2); dstFil.print("\r\n");
    dstFil.print("brightness3 = "); dstFil.print(cfg.brightness3); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("sgv_only = "); dstFil.print(cfg.sgv_only); dstFil.print("\r\n");
    dstFil.print("info_line = "); dstFil.print(cfg.info_line); dstFil.print("\r\n");
    dstFil.print("date_format = "); dstFil.print(cfg.date_format); dstFil.print("\r\n");
    dstFil.print("time_format = "); dstFil.print(cfg.time_format); dstFil.print("\r\n");
    dstFil.print("display_rotation = "); dstFil.print(cfg.display_rotation); dstFil.print("\r\n");
    if(cfg.invert_display!=-1) {
      dstFil.print("invert_display = "); dstFil.print(cfg.invert_display); dstFil.print("\r\n");
    }
    dstFil.print("temperature_unit = "); dstFil.print(cfg.temperature_unit); dstFil.print("\r\n");
    dstFil.print("LED_strip_mode = "); dstFil.print(cfg.LED_strip_mode); dstFil.print("\r\n");
    dstFil.print("LED_strip_pin = "); dstFil.print(cfg.LED_strip_pin); dstFil.print("\r\n");
    dstFil.print("LED_strip_count = "); dstFil.print(cfg.LED_strip_count); dstFil.print("\r\n");
    dstFil.print("LED_strip_brightness = "); dstFil.print(cfg.LED_strip_brightness); dstFil.print("\r\n");
    dstFil.print("vibration_mode = "); dstFil.print(cfg.vibration_mode); dstFil.print("\r\n");
    dstFil.print("vibration_pin = "); dstFil.print(cfg.vibration_pin); dstFil.print("\r\n");
    dstFil.print("vibration_strength = "); dstFil.print(cfg.vibration_strength); dstFil.print("\r\n");
    dstFil.print("micro_dot_pHAT = "); dstFil.print(cfg.micro_dot_pHAT); dstFil.print("\r\n");
    dstFil.print("disable_web_server = "); dstFil.print(cfg.disable_web_server); dstFil.print("\r\n");
    dstFil.print("\r\n");
    dstFil.print("developer_mode = "); dstFil.print(cfg.dev_mode); dstFil.print("\r\n");
    dstFil.print("\r\n");
    for(int i=0; i<10; i++) {
      if(cfg.wlanssid[i][0] != 0) {
        dstFil.print("[wlan"); dstFil.print(i); dstFil.print("]\r\n");
        dstFil.print("SSID = "); dstFil.print(cfg.wlanssid[i]); dstFil.print("\r\n");
        if(cfg.wlanpass[i][0]!=0) {
          dstFil.print("PASS = "); dstFil.print(cfg.wlanpass[i]); dstFil.print("\r\n");
        }
      }
    }
    dstFil.flush();
    dstFil.close();
    Serial.println("New M5NS.INI saved to SD card.");
  }

  saveConfigToFlash(&cfg);
}

void handleSaveConfig() {
  // Bootstrap mode always restarts once configured (existing behavior); otherwise restart
  // only if an edited item that needs it (device name, time zone/DST, WiFi networks) was
  // actually changed - tracked in handleGetEditConfigItem() via restartPending.
  bool willRestart = cfg.is_task_bootstrapping || restartPending;

  String message;
  message.reserve(768);
  pageHeadOpen(message, willRestart
    ? "<meta http-equiv=\"refresh\" content=\"5;url=/\" />\r\n"
    : "<meta http-equiv=\"refresh\" content=\"2;url=/\" />\r\n");
  message += "<p>Saving configuration to M5NS.INI file.</p>\r\n";
  message += "<p>Backup copy of current M5NS.INI should be blaced in M5NS.BAK file.</p>\r\n";
  if (willRestart) {
    message += "<p><b>Restarting to apply changes...</b></p>\r\n";
  }
  message += "</body>\r\n";
  message += "</html>\r\n";

  persistConfigToDisk();

  w3srv.send(200, "text/html; charset=utf-8", message);
  delay(100);

  if (cfg.is_task_bootstrapping) {
    M5.update();
    WiFi.softAPdisconnect(true);
    delay(1000);
    ESP.restart();
  } else if (restartPending) {
    M5.update();
    delay(1000);
    ESP.restart();
  } else {
    M5.Lcd.fillScreen(BLACK);
    draw_page();
    if(cfg.LED_strip_mode != 0) {
      pixels.show();
    }
  }
}

// Medtronic CareLink sign-in page (/carelink). CareLink only issues logins through its own
// web sign-in, which the device can't show, so the user signs in on their phone or computer
// and pastes back the address the browser ends on. The device then exchanges the code in it
// for a long-lived login, kept in flash and renewed automatically.
void handleCareLink() {
  String result;
  bool resultOk = false;
  if (w3srv.method() == HTTP_POST) {
    String action = w3srv.hasArg("action") ? w3srv.arg("action") : String("");
    if (action.equals("signout")) {
      careLinkSignOut();
      result = "Signed out. The CareLink login was deleted from this device.";
      resultOk = true;
    } else if (w3srv.hasArg("pasted")) {
      resultOk = careLinkSignIn(&cfg, w3srv.arg("pasted").c_str(), result);
    }
  }

  String message;
  message.reserve(4096);
  pageHeadOpen(message, NULL);
  message += "<a class=\"bk\" href=\"/\">"; message += WG_BACK_EARLY; message += " All settings</a><p class=\"cnt\">2 of 5</p>\r\n";
  bool justSignedIn = resultOk && w3srv.method() == HTTP_POST && !w3srv.hasArg("action");
  bool signedIn = careLinkHasLogin() && !careLinkNeedsSignIn();
  if (justSignedIn || signedIn) {
    // Done: say so clearly and lead on to the next step, with signing in again tucked away.
    message += "<div class=\"card\"><h2>CareLink sign-in</h2>\r\n";
    message += "<p class=\"ok\"><b>";
    message += justSignedIn ? "Signed in to CareLink." : "Wee Glo is signed in to CareLink.";
    message += "</b> Readings show on Wee Glo within about 5 minutes. You can close the CareLink tab.</p>\r\n";
    message += "<div class=\"nav\"><a class=\"btn o\" href=\"/\">"; message += WG_BACK_EARLY; message += "All settings</a>"
               "<a class=\"btn g\" href=\"/card?c=alarms\">Next: alarm levels</a></div></div>\r\n";  // "Next", so it isn't mixed up with the guide's own Done, next step
    message += "<details style=\"margin-top:14px\"><summary>Sign in again</summary><div style=\"padding:0 12px 12px\">";
  } else {
    message += "<div class=\"card\"><h2>CareLink sign-in</h2>\r\n";
    if (result.length()) {
      message += "<p class=\"bad\">"; message += htmlEscape(result);
      message += " Sign in once more on the CareLink tab, and copy the new red line straight away.</p>\r\n";
    }
  }
  if (cfg.is_task_bootstrapping)
    message += "<p class=\"bad\">Set up your home Wi-Fi first. CareLink sign-in needs the internet.</p>\r\n";
  message += "<p><b>Do this on a computer</b> with Chrome or Edge. Phones hide the sign-in code Wee Glo needs. "
             "Use the care partner account you made just for Wee Glo, not the one in your CareLink Connect app.</p>\r\n";
  // Wee Glo: the video lives in the DiabetesHub guide (no room for video on Wee Glo itself)
  message += "<p><a href=\"" WG_SITE "/wee-glo/setup?step=3\" target=\"_blank\" rel=\"noopener\">"
             "<svg viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linejoin=\"round\" style=\"vertical-align:-3px;margin-right:6px\" aria-hidden=\"true\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M10 8.5l5.5 3.5-5.5 3.5z\"/></svg>"
             "Watch the video</a> of these steps. It opens the DiabetesHub guide in a new tab.</p>\r\n";
  String err;
  String loginUrl = careLinkLoginUrl(&cfg, err);
  message += "<ol style=\"padding-left:22px;margin:8px 0\">\r\n";
  message += "<li>Keep this page open. Click <b>Sign in to CareLink</b> below. CareLink opens in a new tab.</li>\r\n";
  message += "<li>On the CareLink tab, press <b>Ctrl+Shift+J</b> (on a Mac, Cmd+Option+J). A panel called <b>Console</b> opens.</li>\r\n";
  message += "<li>Type the email and password. Check the password with the eye icon first: too many wrong tries locks the account.</li>\r\n";
  message += "<li>Tick <b>I'm not a robot</b>, then click <b>Sign in</b> straight away. The page seems to do nothing. That's normal.</li>\r\n";
  message += "<li>In the Console, a red line appears starting <b>Failed to launch</b>. Click it three times to select it, then press <b>Ctrl+C</b> (on a Mac, Cmd+C).</li>\r\n";
  message += "<li>Come back to this tab, paste it in the box below (<b>Ctrl+V</b>, or Cmd+V on a Mac), and click <b>Finish sign-in</b> within a minute.</li>\r\n";
  message += "</ol>\r\n";
  if (loginUrl.length()) {
    message += "<a class=\"btn\" style=\"width:100%\" href=\""; message += htmlEscape(loginUrl);
    message += "\" target=\"_blank\" rel=\"noopener\">Sign in to CareLink</a>\r\n";
  } else {
    message += "<p class=\"bad\">The CareLink sign-in page isn't available right now: "; message += htmlEscape(err); message += "</p>\r\n";
  }
  message += "<form action=\"/carelink\" method=\"post\" accept-charset=\"utf-8\" data-wait=\"Signing in to CareLink. This takes a few seconds.\">\r\n";
  message += "<label class=\"fld\" for=\"pasted\">Paste the red line here</label>"
             "<textarea class=\"inp\" id=\"pasted\" name=\"pasted\" rows=\"4\" style=\"padding:10px\" placeholder=\"Failed to launch 'com.medtronic.carepartner:/sso?code=...' because...\"></textarea>\r\n";
  message += "<div class=\"nav\"><a class=\"btn o\" href=\"/\">"; message += WG_BACK_EARLY; message += "Back</a>"
             "<button class=\"btn g\" type=\"submit\">Finish sign-in</button></div>\r\n";
  message += "</form>";
  message += (justSignedIn || signedIn) ? "</div></details>\r\n" : "</div>\r\n";
  message += "<details style=\"margin-top:14px\"><summary>More</summary><div style=\"padding:0 12px 12px\">";
  message += "<p class=\"small\">You can also paste the contents of a logindata.json file made by the carelink-python-client login script.</p>";
  if (careLinkHasLogin()) {
    message += "<form action=\"/carelink\" method=\"post\"><input type=\"hidden\" name=\"action\" value=\"signout\">"
               "<button class=\"btn2\" type=\"submit\" style=\"margin:0\">Sign out of CareLink</button></form>";
  }
  message += "<p class=\"small\">The sign-in is kept only on Wee Glo itself.</p></div></details>\r\n";
  message += "</main></body>\r\n</html>\r\n";
  w3srv.send(200, "text/html; charset=utf-8", message);
}

void handleClearConfigFlash() {
  Preferences prefs;
  if( !prefs.begin("M5NSconfig", false) ) {
    Serial.println("Error opening Preferences M5NSconfig");
  } else {
    Serial.println("Clearing configuration Preferences M5NSconfig");
    prefs.clear();
    prefs.end();
  }
  ESP.restart();
}

void handleNotFound() {
  String message = "M5Stack Nighscout monitor ERROR 404 File Not Found\n\n";
  message += "URI: ";
  message += w3srv.uri();
  message += "\nMethod: ";
  message += (w3srv.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += w3srv.args();
  message += "\n";
  for (uint8_t i = 0; i < w3srv.args(); i++) {
    message += " " + w3srv.argName(i) + ": " + w3srv.arg(i) + "\n";
  }
  w3srv.send(404, "text/plain", message);
}

// =====================================================================================
// Wee Glo (DiabetesHub Ireland): the settings checklist.
// "/" shows five cards in order (Home Wi-Fi, CareLink sign-in, Alarm levels, Side lights,
// Screen and sound). Each card opens its own short page and saves on its own. The full
// list of every setting is still there for confident users, at "/all".
// =====================================================================================

static const char* WG_TICK = "<svg viewBox=\"0 0 24 24\" width=\"16\" height=\"16\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M5 12l5 5 9-10\"/></svg>";
static const char* WG_CHEV = "<svg viewBox=\"0 0 24 24\" width=\"20\" height=\"20\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M9 6l6 6-6 6\"/></svg>";
static const char* WG_BACK = "<svg viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M15 6l-6 6 6 6\"/></svg>";

// Which of the alarm, light and screen cards have been saved at least once (for the ticks).
static int wgSeen() {
  Preferences p; int v = 0;
  if (p.begin("weeglo", true)) { v = p.getInt("seen", 0); p.end(); }
  return v;
}
static void wgMarkSeen(int bit) {
  Preferences p;
  if (p.begin("weeglo", false)) { p.putInt("seen", p.getInt("seen", 0) | bit); p.end(); }
}

static bool wgWifiDone() {
  if (cfg.is_task_bootstrapping) return false;
  for (int i = 0; i < 10; i++) if (cfg.wlanssid[i][0]) return true;
  return false;
}
static bool wgCareLinkDone() { return careLinkHasLogin() && !careLinkNeedsSignIn(); }

static void wgCard(String& m, int n, bool done, const char* href, const char* title, const String& sub) {
  m += "<a class=\"cl"; if (done) m += " dn"; m += "\" href=\""; m += href; m += "\"";
  if (strcmp(href, "/card?c=wifi") == 0) m += " data-wait=\"Looking for Wi-Fi networks near Wee Glo. This takes about 10 seconds.\"";
  m += "><span class=\"ci\">";
  if (done) m += WG_TICK; else m += n;
  m += "</span><span class=\"tx\"><b>"; m += title; m += "</b><span>"; m += sub; m += "</span></span><span class=\"ar\">";
  m += WG_CHEV; m += "</span></a>\r\n";
}

static void wgPageEnd(String& m, const char* backHref, const char* backLabel) {
  if (backHref) { m += "<a class=\"btn o\" href=\""; m += backHref; m += "\">"; m += WG_BACK; m += backLabel; m += "</a>\r\n"; }
  m += "</main></body>\r\n</html>\r\n";
}

// Wee Glo: a short page in the Wee Glo look, for pages built elsewhere (such as the side light test)
void wgSendSimplePage(const char* title, const String& body, const char* backHref, const char* backLabel) {
  String m; m.reserve(2048);
  pageHeadOpen(m, NULL);
  m += "<h1>"; m += title; m += "</h1>\r\n<div class=\"card\" style=\"margin-bottom:16px\">"; m += body; m += "</div>\r\n";
  wgPageEnd(m, backHref, backLabel);
  w3srv.send(200, "text/html; charset=utf-8", m);
}

static String wgNum(float v) { return String(v * (cfg.show_mgdl ? 18 : 1), cfg.show_mgdl ? 0 : 1); }
static const char* wgUnits() { return cfg.show_mgdl ? "mg/dL" : "mmol/L"; }

// Wee Glo finish screen (after the last setup step, or from the link on the settings page).
// Built from this Wee Glo's own settings. "Play" buttons call /soundtest, which plays the sound once with the lights.
static String wgSv(const char* d, int w) {
  String s = "<svg viewBox=\"0 0 24 24\" width=\""; s += w; s += "\" height=\""; s += w;
  s += "\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">"; s += d; s += "</svg>";
  return s;
}
static void wgRow(String& m, const String& ic, const String& b, const char* sub) {
  m += "<div class=\"r\">"; m += ic; m += "<div><b>"; m += b; m += "</b><span>"; m += sub; m += "</span></div></div>";
}
void wgFinishPage(String& m) {
  const String iWifi = wgSv("<path d=\"M2 9a15 15 0 0 1 20 0M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0\"/><circle cx=\"12\" cy=\"19.5\" r=\"1\"/>", 22);
  const String iUser = wgSv("<circle cx=\"12\" cy=\"8\" r=\"4\"/><path d=\"M4 21c0-4 3.6-7 8-7s8 3 8 7\"/>", 22);
  const String iBell = wgSv("<path d=\"M6 16V11a6 6 0 0 1 12 0v5l2 2H4z\"/><path d=\"M10 21h4\"/>", 22);
  const String iLight = wgSv("<path d=\"M9 18h6M10 21h4\"/><path d=\"M12 3a6 6 0 0 0-4 10.5c.7.7 1 1.5 1 2.5h6c0-1 .3-1.8 1-2.5A6 6 0 0 0 12 3z\"/>", 22);
  const String iCheck = wgSv("<circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M8 12.5l2.7 2.7L16 9.8\"/>", 22);
  const String iPlay = wgSv("<circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M10 8.5l5.5 3.5-5.5 3.5z\"/>", 20);
  const String iClock = wgSv("<circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 7v5l3 2\"/>", 22);
  const String iNote = wgSv("<path d=\"M9 18V6l10-2v12\"/><circle cx=\"6.5\" cy=\"18\" r=\"2.5\"/><circle cx=\"16.5\" cy=\"16\" r=\"2.5\"/>", 20);
  m += "<style>.fin .sec{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:16px 18px;margin:0 0 14px}"
       ".fin h2{font-size:18px;margin:0 0 10px;display:flex;align-items:center;gap:8px}.fin h2 svg{color:var(--blue);flex:none}"
       ".fin .rows{display:grid;gap:8px}.fin .r{display:flex;gap:10px;align-items:flex-start}.fin .r svg{flex:none;color:var(--blue);margin-top:2px}"
       ".fin .r b{display:block;color:var(--text)}.fin .r span{color:var(--muted);font-size:15px}"
       ".fin .key{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:8px 16px}.fin .k{display:flex;gap:10px;align-items:center;font-size:15px}"
       ".fin .dot{flex:none;width:18px;height:18px;border-radius:50%}.fin .sub{font-weight:700;margin:12px 0 6px;font-size:15px}"
       ".fin .btns{display:flex;gap:10px;flex-wrap:wrap}.fin .pb{display:inline-flex;align-items:center;gap:8px;min-height:46px;padding:0 18px;border-radius:12px;font:inherit;font-weight:700;font-size:15.5px;border:2px solid var(--blue);background:var(--card);color:var(--blue);cursor:pointer}"
       ".fin .pb.p{background:var(--blue);color:#fff}:root[data-theme=dark] .fin .pb.p{color:#0B1220}.fin .pb[disabled]{opacity:.6;cursor:wait}"
       ".fin .hero{text-align:center;margin:4px 0 16px}.fin .tk{width:56px;height:56px;border-radius:50%;background:var(--green);display:flex;align-items:center;justify-content:center;margin:0 auto 8px}"
       ".fin ol{margin:0;padding-left:22px}.fin ol li{margin:4px 0}.fin h1:focus{outline:none}"
       ".fin .go{display:flex;align-items:center;justify-content:center;width:100%;min-height:50px;border-radius:12px;background:var(--green);color:#fff;font-weight:700;font-size:16px;text-decoration:none;margin-top:6px}"
       ".fin .stay{display:flex;align-items:center;justify-content:center;min-height:44px;color:var(--blue);font-weight:600;text-decoration:none;margin-top:6px}</style>\r\n";
  m += "<div class=\"fin\"><div class=\"hero\"><div class=\"tk\"><svg viewBox=\"0 0 24 24\" width=\"32\" height=\"32\" fill=\"none\" stroke=\"#fff\" stroke-width=\"3\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\"><path d=\"M5 12l5 5 9-10\"/></svg></div>"
       "<h1 style=\"margin:0\" id=\"fint\" tabindex=\"-1\">Wee Glo is all set up</h1><p style=\"margin:6px 0 0;color:var(--muted)\">Here's what it's doing now, and what to expect tonight.</p></div>\r\n";
  // 1: what Wee Glo is doing now
  m += "<div class=\"sec\"><h2>"; m += iCheck; m += "What Wee Glo is doing now</h2><div class=\"rows\">";
  String w = "On your Wi-Fi, "; w += htmlEscape(WiFi.SSID());
  wgRow(m, iWifi, w, "It works anywhere in the house that Wi-Fi reaches.");
  wgRow(m, iUser, htmlEscape(careLinkStatus()), "A new reading arrives about every 5 minutes.");
  String a = "Alarm below "; a += wgNum(cfg.snd_alarm); a += " and above "; a += wgNum(cfg.snd_alarm_high); a += " "; a += wgUnits();
  wgRow(m, iBell, a, "Change these any time under Alarm levels.");
  const char* lt[4] = {"Side lights off", "Side lights flash with each sound", "Side lights for warnings and alarms", "Side lights always on"};
  const char* ls[4] = {"Turn them on under Side lights, if you have the battery base.", "They stay off the rest of the time.",
                       "They stay off while the reading is in range.", "Green while the reading is in range."};
  int lm = (cfg.LED_strip_mode >= 0 && cfg.LED_strip_mode < 4) ? cfg.LED_strip_mode : 0;
  wgRow(m, iLight, lt[lm], ls[lm]);
  m += "</div></div>\r\n";
  // 3: what the lights and sounds mean
  m += "<div class=\"sec\"><h2>"; m += iLight; m += "What the lights and sounds mean</h2>";
  if (lm != 0) {
    m += "<div class=\"key\">"
         "<div class=\"k\"><i class=\"dot\" style=\"background:#EF4444\"></i><span><b>Red:</b> low</span></div>"
         "<div class=\"k\"><i class=\"dot\" style=\"background:#F59E0B\"></i><span><b>Amber:</b> high</span></div>"
         "<div class=\"k\"><i class=\"dot\" style=\"background:#3B82F6\"></i><span><b>Blue:</b> no new readings</span></div>";
    if (lm == 3) m += "<div class=\"k\"><i class=\"dot\" style=\"background:#22C55E\"></i><span><b>Green:</b> in range</span></div>";
    m += "</div><p class=\"sub\">Sounds</p>";
  }
  m += "<div class=\"rows\">";
  wgRow(m, iNote, "Soft four-note chime", "Wee Glo has just started.");
  wgRow(m, iNote, "3 short high beeps", "A warning, before it becomes an alarm, or no new readings for a while.");
  wgRow(m, iNote, lm != 0 ? "6 longer beeps, lights flashing" : "6 longer beeps", "An alarm: low or high. Press the middle button to snooze.");
  m += "</div></div>\r\n";
  // 5: hear it now
  m += "<div class=\"sec\"><h2>"; m += iPlay; m += "Hear it now, not at 3am</h2>"
       "<p style=\"margin:0 0 10px\">Play each sound once";
  if (lm != 0) m += ", with the lights,";
  m += " so you know what to listen for. Change the loudness under Screen and sound.</p>"
       "<div class=\"btns\"><button type=\"button\" class=\"pb p\" data-k=\"alarm\">"; m += iPlay; m += "Play the alarm</button>"
       "<button type=\"button\" class=\"pb\" data-k=\"warning\">"; m += iPlay; m += "Play the warning</button></div>"
       "<p class=\"small\" id=\"pbmsg\" role=\"status\" style=\"margin:8px 0 0\"></p></div>\r\n";
  // 7: is it working?
  m += "<div class=\"sec\"><h2>"; m += iClock; m += "Is it working?</h2><ol>"
       "<li>Look at the small number of <b>minutes</b> beside the reading. It should go back to 0 about every 5 minutes.</li>"
       "<li>If it shows <b>Err</b>, or the minutes keep going up: check the Wi-Fi is on, the phone app that sends readings to CareLink is still running, and CareLink sign-in still shows a tick on the settings page.</li>"
       "<li>Still stuck? See <b>diabeteshub.ie/wee-glo</b> or email <b>weeglo@diabeteshub.ie</b>.</li></ol></div>\r\n";
  m += "<a class=\"go\" id=\"fingo\" href=\"" WG_SITE "/wee-glo/setup?step=done\">Finish on DiabetesHub</a>"
       "<a class=\"stay\" href=\"/\">Stay on Wee Glo settings</a></div>\r\n";
  m += "<script>(function(){try{history.replaceState(null,'','/?done=1');}catch(e){}"
       "var msg=document.getElementById('pbmsg');document.querySelectorAll('.fin .pb').forEach(function(b){b.onclick=function(){"
       "var all=document.querySelectorAll('.fin .pb');all.forEach(function(x){x.disabled=true;});msg.textContent='Playing on Wee Glo now.';"
       "fetch('/soundtest?k='+b.getAttribute('data-k')).catch(function(){}).then(function(){setTimeout(function(){all.forEach(function(x){x.disabled=false;});msg.textContent='';},b.getAttribute('data-k')==='alarm'?4200:1500);});};});"
       "var t=document.getElementById('fint');if(t)try{t.focus({preventScroll:true});}catch(e){}})();</script>\r\n";
  wgPageEnd(m, NULL, NULL);
  w3srv.send(200, "text/html; charset=utf-8", m);
}

void handleWeeGloHome() {
  String m;
  m.reserve(4096);
  pageHeadOpen(m, NULL);
  bool allDone = !cfg.is_task_bootstrapping && wgWifiDone() && wgCareLinkDone() && (wgSeen() & 7) == 7;
  if (allDone && w3srv.hasArg("done")) { wgFinishPage(m); return; }
  m += "<h1>Wee Glo settings</h1>\r\n";
  if (allDone) {
    static const char* WG_NEXT =
         "<ol style=\"margin:0;padding-left:22px\">"
         "<li>You can unplug Wee Glo from the computer. It keeps all its settings, and joins your Wi-Fi anywhere in the house.</li>"
         "<li>Put Wee Glo where you'll see and hear it, such as by the bed, plugged into a USB charger or on the battery base.</li>"
         "<li>Keep the pump and phone alarms on too. Wee Glo is an extra display.</li>"
         "<li>You can close this page. To change a setting later, tap the right-hand button on Wee Glo until you see the square code, and scan it.</li>"
         "</ol>";
    m += "<div class=\"ok\" id=\"done\" style=\"margin:0 0 16px\"><p style=\"margin:0 0 6px\"><b>Wee Glo is all set up.</b> "
         "It shows the readings, and the alarms are on.</p><p style=\"margin:0 0 4px\"><b>What's next</b></p>";
    m += WG_NEXT; m += "</div>\r\n";
    // Wee Glo: straight after the last step is saved, a finish screen explains what Wee Glo is doing now,
    // what the lights and sounds mean, lets you hear the alarm once, and how to tell it's working.
    m += "<p style=\"margin:-6px 0 16px\"><a href=\"/?done=1\">What Wee Glo is doing now, and hear the alarm</a></p>\r\n";
    m += "<p class=\"lead\">Your settings. Tap one to change it.</p>\r\n";
  } else if (cfg.is_task_bootstrapping) {
    m += "<p class=\"lead\">Start with your home Wi-Fi. The other steps need Wee Glo to be on your Wi-Fi.</p>\r\n";
  } else {
    m += "<p class=\"lead\">Tap each step in order. Each one saves on its own.</p>\r\n";
  }
  m += "<div class=\"list\">\r\n";
  String wsub = "Not set up yet";
  if (wgWifiDone()) { wsub = "Connected to "; wsub += htmlEscape(WiFi.SSID()); }
  wgCard(m, 1, wgWifiDone(), "/card?c=wifi", "Home Wi-Fi", wsub);
  String csub = cfg.is_task_bootstrapping ? String("After Wi-Fi") : htmlEscape(careLinkStatus());
  wgCard(m, 2, wgCareLinkDone(), "/carelink", "CareLink sign-in", csub);
  int seen = wgSeen();
  String asub = "Low "; asub += wgNum(cfg.snd_alarm); asub += ", high "; asub += wgNum(cfg.snd_alarm_high); asub += " "; asub += wgUnits();
  wgCard(m, 3, seen & 1, "/card?c=alarms", "Alarm levels", asub);
  const char* ledNames[4] = {"Off", "Light with sound", "Warnings and alarms", "Always on"};
  String lsub = (cfg.LED_strip_mode >= 0 && cfg.LED_strip_mode < 4) ? String(ledNames[cfg.LED_strip_mode]) : String("Off");
  lsub += ", battery base only";
  wgCard(m, 4, seen & 2, "/card?c=lights", "Side lights", lsub);
  wgCard(m, 5, seen & 4, "/card?c=screen", "Screen and sound", "Brightness, time, volume");
  m += "</div>\r\n";
  m += "<p class=\"small\" style=\"margin-top:18px\"><a href=\"/all\">Advanced settings</a> &middot; <a href=\"/fwcheck\" data-wait=\"Checking for updates. This takes a few seconds.\">Check for updates</a></p>\r\n";
  m += "<p class=\"small\">Help: diabeteshub.ie/wee-glo &middot; weeglo@diabeteshub.ie<br>Wee Glo "; m += M5NSversion; m += " &middot; "; m += cfg.deviceName; m += "</p>\r\n";
  wgPageEnd(m, NULL, NULL);
  w3srv.send(200, "text/html; charset=utf-8", m);
}

static void wgField(String& m, const char* id, const char* label, const String& input, const char* note = NULL) {
  m += "<div><label class=\"fld\" for=\""; m += id; m += "\">"; m += label; m += "</label>"; m += input;
  if (note) { m += "<p class=\"small\" style=\"margin:4px 0 0\">"; m += note; m += "</p>"; }
  m += "</div>\r\n";
}
static String wgInput(const char* id, const String& value, const char* type = "text", const char* extra = "") {
  String s = "<input class=\"inp\" id=\""; s += id; s += "\" name=\""; s += id; s += "\" type=\""; s += type;
  s += "\" value=\""; s += htmlEscape(value); s += "\" "; s += extra; s += ">";
  return s;
}
static String wgSelect(const char* id, const char* const opts[], const int vals[], int n, int cur) {
  String s = "<select class=\"inp\" id=\""; s += id; s += "\" name=\""; s += id; s += "\">";
  for (int i = 0; i < n; i++) {
    s += "<option value=\""; s += vals[i]; s += "\""; if (vals[i] == cur) s += " selected"; s += ">"; s += opts[i]; s += "</option>";
  }
  s += "</select>";
  return s;
}

static void wgCardForm(String& m, const char* c, int n, const char* title) {
  m += "<a class=\"bk\" href=\"/\">"; m += WG_BACK; m += " All settings</a><p class=\"cnt\">"; m += n; m += " of 5</p>\r\n";
  m += "<form class=\"card\" action=\"/card\" method=\"post\" accept-charset=\"utf-8\" data-wait=\"";
  m += strcmp(c, "wifi") == 0 ? "Saving. Wee Glo restarts and joins your Wi-Fi." : "Saving.";
  m += "\"><input type=\"hidden\" name=\"c\" value=\""; m += c; m += "\">\r\n";
  m += "<h2>"; m += title; m += "</h2>\r\n";
}
static void wgCardFormEnd(String& m, const char* save) {
  m += "<div class=\"nav\"><a class=\"btn o\" href=\"/\">"; m += WG_BACK; m += "Back</a><button class=\"btn g\" type=\"submit\">";
  m += WG_TICK; m += " "; m += save; m += "</button></div></form>\r\n";
}

static void wgShowCard(const String& c, const String& msg, bool msgOk) {
  String m;
  m.reserve(6144);
  pageHeadOpen(m, NULL);
  String note;
  if (msg.length()) { note = "<p class=\""; note += msgOk ? "ok" : "bad"; note += "\">"; note += msg; note += "</p>\r\n"; }

  if (c.equals("wifi")) {
    wgCardForm(m, "wifi", 1, "Home Wi-Fi");
    m += note;
    String saved;
    for (int i = 0; i < 10; i++) if (cfg.wlanssid[i][0]) { if (saved.length()) saved += ", "; saved += htmlEscape(cfg.wlanssid[i]); }
    m += "<p>Wee Glo can remember up to 10 Wi-Fi networks, such as home and the grandparents' house.";
    if (saved.length()) { m += " Saved: <b>"; m += saved; m += "</b>."; }
    m += "</p>\r\n";
    int n = WiFi.scanNetworks();
    m += "<datalist id=\"scan\">";
    for (int i = 0; i < n; i++) { m += "<option value=\""; m += htmlEscape(WiFi.SSID(i)); m += "\">"; }
    m += "</datalist>\r\n";
    WiFi.scanDelete();
    wgField(m, "ssid", "Wi-Fi name", wgInput("ssid", "", "text", "list=\"scan\" maxlength=\"63\" autocomplete=\"off\" required"), "Tap the box to pick from the networks Wee Glo can see.");
    wgField(m, "pass", "Wi-Fi password", wgInput("pass", "", "password", "maxlength=\"63\""));
    const char* srcOpts[1] = {"Medtronic CareLink"}; const int srcVals[1] = {3};
    wgField(m, "src", "Readings from", wgSelect("src", srcOpts, srcVals, 1, 3));
    const char* ctyOpts[2] = {"Ireland", "United Kingdom"}; const int ctyVals[2] = {0, 1};
    wgField(m, "cty", "Country", wgSelect("cty", ctyOpts, ctyVals, 2, strcmp(cfg.cl_country, "GB") == 0 ? 1 : 0));
    m += "<p class=\"small\">Wee Glo restarts and joins your Wi-Fi when you save.</p>\r\n";
    wgCardFormEnd(m, "Save Wi-Fi");
  } else if (c.equals("alarms")) {
    wgCardForm(m, "alarms", 3, "Alarm levels");
    m += note;
    m += "<p>Use the levels your diabetes team gave you. Each side has an early warning and a main alarm.</p>\r\n";
    m += "<h3 class=\"fld\" style=\"margin-top:14px\">Lows, in " + String(wgUnits()) + "</h3><div class=\"row2\">";
    wgField(m, "al", "Low alarm", wgInput("al", wgNum(cfg.snd_alarm), "text", "inputmode=\"decimal\""));
    wgField(m, "wl", "Low warning", wgInput("wl", wgNum(cfg.snd_warning), "text", "inputmode=\"decimal\""));
    m += "</div><h3 class=\"fld\" style=\"margin-top:14px\">Highs, in " + String(wgUnits()) + "</h3><div class=\"row2\">";
    wgField(m, "wh", "High warning", wgInput("wh", wgNum(cfg.snd_warning_high), "text", "inputmode=\"decimal\""));
    wgField(m, "ah", "High alarm", wgInput("ah", wgNum(cfg.snd_alarm_high), "text", "inputmode=\"decimal\""));
    m += "</div><h3 class=\"fld\" style=\"margin-top:14px\">Timing and volume</h3><div class=\"row2\">";
    wgField(m, "nr", "No readings (minutes)", wgInput("nr", String(cfg.snd_no_readings), "text", "inputmode=\"numeric\""));
    wgField(m, "rp", "Repeat alarm (minutes)", wgInput("rp", String(cfg.alarm_repeat), "text", "inputmode=\"numeric\""));
    wgField(m, "sz", "Snooze (minutes)", wgInput("sz", String(cfg.snooze_timeout), "text", "inputmode=\"numeric\""));
    wgField(m, "av", "Alarm volume (%)", wgInput("av", String(cfg.alarm_volume), "text", "inputmode=\"numeric\""));
    wgField(m, "wv", "Warning volume (%)", wgInput("wv", String(cfg.warning_volume), "text", "inputmode=\"numeric\""));
    m += "</div>\r\n";
    wgCardFormEnd(m, "Save alarm levels");
  } else if (c.equals("lights")) {
    wgCardForm(m, "lights", 4, "Side lights");
    m += note;
    m += "<p>For the battery base. The lights glow red for lows and amber for highs, so you can see them across the room. When Wee Glo comes on, they glow navy, green and white for a few seconds. At 5.5, our unicorn number, they run through the rainbow. It's a little celebration, not a warning.</p>\r\n";
    const char* ledOpts[4] = {"Warnings and alarms (our pick)", "Light with sound", "Always on", "Off"};
    const int ledVals[4] = {2, 1, 3, 0};
    wgField(m, "lm", "Show", wgSelect("lm", ledOpts, ledVals, 4, cfg.LED_strip_mode));
    wgField(m, "lb", "Brightness (%)", wgInput("lb", String(cfg.LED_strip_brightness), "text", "inputmode=\"numeric\""), "10 is gentle at night. 100 is very bright.");
    m += "<p style=\"margin-top:12px\"><a class=\"btn o\" href=\"/lighttest\">Run the light test</a></p><p class=\"small\">Save first, then run the test.</p>\r\n";
    wgCardFormEnd(m, "Save side lights");
  } else if (c.equals("screen")) {
    wgCardForm(m, "screen", 5, "Screen and sound");
    m += note;
    m += "<p>The left button under the screen steps through three brightness levels.</p><div class=\"row2\">\r\n";
    wgField(m, "b1", "Brightness 1 (%)", wgInput("b1", String(cfg.brightness1), "text", "inputmode=\"numeric\""));
    wgField(m, "b2", "Brightness 2 (%)", wgInput("b2", String(cfg.brightness2), "text", "inputmode=\"numeric\""));
    wgField(m, "b3", "Brightness 3 (%)", wgInput("b3", String(cfg.brightness3), "text", "inputmode=\"numeric\""));
    m += "</div>\r\n";
    const char* tfOpts[2] = {"24-hour", "AM/PM"}; const int tfVals[2] = {0, 1};
    wgField(m, "tf", "Time", wgSelect("tf", tfOpts, tfVals, 2, cfg.time_format));
    const char* suOpts[2] = {"On", "Off"}; const int suVals[2] = {1, 0};
    wgField(m, "ss", "Chime when Wee Glo starts", wgSelect("ss", suOpts, suVals, 2, cfg.snd_warning_at_startup ? 1 : 0));
    wgCardFormEnd(m, "Save");
  } else {
    w3srv.sendHeader("Location", "/");
    w3srv.send(302, "text/plain", "");
    return;
  }
  wgPageEnd(m, NULL, NULL);
  w3srv.send(200, "text/html; charset=utf-8", m);
}

static bool wgIntArg(const char* k, int lo, int hi, int& out) {
  if (!w3srv.hasArg(k)) return false;
  String v = w3srv.arg(k); v.trim();
  if (v.length() == 0) return false;
  for (size_t i = 0; i < v.length(); i++) if (!isDigit(v[i])) return false;
  int x = v.toInt();
  if (x < lo || x > hi) return false;
  out = x; return true;
}
static bool wgLevelArg(const char* k, float& out) {
  if (!w3srv.hasArg(k)) return false;
  String v = w3srv.arg(k); v.trim(); v.replace(",", ".");
  if (v.length() == 0) return false;
  float x = v.toFloat() / (cfg.show_mgdl ? 18.0f : 1.0f);
  if (x < 1.5f || x > 30.0f) return false;
  out = x; return true;
}

static void wgRestartPage(const char* title, const char* text) {
  String m;
  pageHeadOpen(m, "<meta http-equiv=\"refresh\" content=\"20;url=/\" />\r\n");
  m += "<h1>"; m += title; m += "</h1><p>"; m += text; m += "</p>\r\n";
  wgPageEnd(m, NULL, NULL);
  w3srv.send(200, "text/html; charset=utf-8", m);
}

void handleWeeGloCard() {
  String c = w3srv.hasArg("c") ? w3srv.arg("c") : String("");
  if (w3srv.method() != HTTP_POST) { wgShowCard(c, "", true); return; }

  if (c.equals("wifi")) {
    String ssid = w3srv.arg("ssid"); ssid.trim();
    String pass = w3srv.arg("pass");
    if (ssid.length() == 0 || ssid.length() > 63 || pass.length() > 63) { wgShowCard(c, "Type or pick your Wi-Fi name.", false); return; }
    int slot = -1;
    for (int i = 0; i < 10; i++) if (ssid.equals(cfg.wlanssid[i])) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < 10; i++) if (cfg.wlanssid[i][0] == 0) { slot = i; break; }
    if (slot < 0) slot = 0;
    strlcpy(cfg.wlanssid[slot], ssid.c_str(), sizeof(cfg.wlanssid[slot]));
    strlcpy(cfg.wlanpass[slot], pass.c_str(), sizeof(cfg.wlanpass[slot]));
    cfg.data_source = 3;
    strcpy(cfg.cl_country, w3srv.arg("cty").equals("1") ? "GB" : "IE");
    persistConfigToDisk();
    String t = "Wee Glo is restarting and joining <b>"; t += htmlEscape(ssid);
    t += "</b>. Put your phone back on your home Wi-Fi. In a minute, tap the right-hand button on Wee Glo until you see a code, and scan it to come back here.";
    wgRestartPage("Saved", t.c_str());
    delay(500);
    if (cfg.is_task_bootstrapping) WiFi.softAPdisconnect(true);
    delay(1000);
    ESP.restart();
    return;
  }

  if (c.equals("alarms")) {
    float al, wl, wh, ah; int nr, rp, sz, av, wv;
    if (!wgLevelArg("al", al) || !wgLevelArg("wl", wl) || !wgLevelArg("wh", wh) || !wgLevelArg("ah", ah)) {
      wgShowCard(c, "Check the levels: use numbers such as 4.0 or 14.0.", false); return;
    }
    if (!(al < wl && wl < wh && wh <= ah)) {
      wgShowCard(c, "The levels need to go up in order: low alarm, low warning, high warning, high alarm.", false); return;
    }
    if (!wgIntArg("nr", 5, 240, nr) || !wgIntArg("rp", 1, 60, rp) || !wgIntArg("sz", 5, 240, sz) || !wgIntArg("av", 0, 100, av) || !wgIntArg("wv", 0, 100, wv)) {
      wgShowCard(c, "Check the minutes and volumes: whole numbers, volume from 0 to 100.", false); return;
    }
    cfg.snd_alarm = al; cfg.snd_warning = wl; cfg.snd_warning_high = wh; cfg.snd_alarm_high = ah;
    cfg.snd_no_readings = nr; cfg.alarm_repeat = rp; cfg.snooze_timeout = sz; cfg.alarm_volume = av; cfg.warning_volume = wv;
    persistConfigToDisk(); wgMarkSeen(1);
  } else if (c.equals("lights")) {
    int lm, lb;
    if (!wgIntArg("lm", 0, 3, lm) || !wgIntArg("lb", 1, 100, lb)) { wgShowCard(c, "Check the brightness: a number from 1 to 100.", false); return; }
    cfg.LED_strip_mode = lm; cfg.LED_strip_brightness = lb;
    if (cfg.LED_strip_count <= 0) cfg.LED_strip_count = 10;   // the M5GO battery base has 10 lights
    cfg.LED_strip_pin = halSafeLedPin(cfg.LED_strip_pin);
    persistConfigToDisk(); wgMarkSeen(2);
    if (lm == 0) { pixels.clear(); pixels.show(); }
  } else if (c.equals("screen")) {
    int b1, b2, b3, tf, ss;
    if (!wgIntArg("b1", 1, 100, b1) || !wgIntArg("b2", 1, 100, b2) || !wgIntArg("b3", 1, 100, b3) || !wgIntArg("tf", 0, 1, tf) || !wgIntArg("ss", 0, 1, ss)) {
      wgShowCard(c, "Check the brightness: numbers from 1 to 100.", false); return;
    }
    cfg.brightness1 = b1; cfg.brightness2 = b2; cfg.brightness3 = b3; cfg.time_format = tf; cfg.snd_warning_at_startup = ss;
    lcdBrightness = b1; lcdSetBrightness(b1);
    persistConfigToDisk(); wgMarkSeen(4);
  } else {
    w3srv.sendHeader("Location", "/"); w3srv.send(302, "text/plain", ""); return;
  }
  M5.Lcd.fillScreen(BLACK);
  draw_page();
  // Wee Glo: once the last step is saved, the home page opens with the "all set up" pop-up
  bool nowDone = !cfg.is_task_bootstrapping && wgWifiDone() && wgCareLinkDone() && (wgSeen() & 7) == 7;
  w3srv.sendHeader("Location", nowDone ? "/?done=1" : "/");
  w3srv.send(303, "text/plain", "");
}
