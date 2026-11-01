# DiabetesHub display

A small bedside or desk display that shows live glucose readings from a **Medtronic CareLink** care partner account, with alarms for lows and highs. It runs on an M5Stack Core2 and is set up from a phone, with no coding needed.

Built for families in Ireland by [DiabetesHub Ireland](https://diabeteshub.ie). The step-by-step guide, what to buy and the installer are on **diabeteshub.ie**, in Devices, DIY Display.

> **Not a medical device.** This display is a convenience only. Never use it to make treatment decisions. Always check readings on your pump, sensor app or a finger-prick test, and keep your phone app's alarms switched on.

### What it does

- Shows the latest sensor reading in mmol/L, with a trend arrow and how long ago it was taken
- Sounds an alarm for low and high readings, and when no new readings arrive
- Signs in with a CareLink **care partner** account, and keeps the sign-in on the display only
- Joins 2.4 GHz home Wi-Fi (the Core2 can't use 5 GHz networks)

### Based on the M5Stack Nightscout monitor

This project is a modified version of the M5Stack Nightscout monitor, and is free software under the same licence.

M5Stack Nightscout monitor<br/>Copyright (C) 2018-2023 Martin Lukasek <martin@lukasek.cz><br/>Copyright (C) 2021-2026 Patrick Sonnerat and contributors<br/>Copyright (C) 2026 DiabetesHub Ireland (Medtronic CareLink data source and DiabetesHub changes)

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.

**Changes made by DiabetesHub Ireland (October and November 2026):** a Medtronic CareLink data source, an M5Stack Core2 build, and an automatic firmware build on GitHub. See [License and credits](#license-and-credits) below for the third-party code this project includes and the people who contributed to it.

---

## Technical documentation

The rest of this page is the original project's documentation. It covers every setting, including the Nightscout, Dexcom and Libre data sources this code also supports.

<img width="240" src="https://raw.githubusercontent.com/mlukasek/M5_NightscoutMon/master/images/M5NS_mon_2019-06-20_page1.jpg">&nbsp;&nbsp;<img width="240" src="https://raw.githubusercontent.com/mlukasek/M5_NightscoutMon/master/images/M5NS_mon_2019-06-20_page2.jpg">&nbsp;&nbsp;<img width="240" src="https://raw.githubusercontent.com/mlukasek/M5_NightscoutMon/master/images/M5NS_mon_2019-06-20_page3.jpg">

### Contents
[What is this good for?](#m5stack-nightscout-monitor)  
[Wiki documentation](https://github.com/mlukasek/M5_NightscoutMon/wiki)  
[Latest software revisions (CHANGELOG) **WHAT IS NEW**](CHANGELOG.md)  
[Syntax of M5NS.INI configuration file](#the-syntax-of-m5nsini-configuration-file)  
[Medtronic CareLink](#medtronic-carelink)  
[Display](#display)  
[Buttons](#buttons)  
[Battery](#battery)  
[Installation and support](#installation-and-support)  
[Firmware updates (OTA)](#firmware-updates-ota)  
[License and credits](#license-and-credits)  
[Donations - support the project](#donations) :+1:


### M5Stack Nightscout Monitor

M5Stack is small development kit based on ESP32. It is in a nice plastic box, equipped with color display, micro SD card slot, 3 buttons, speaker and internal battery. It is great to monitor and display something, so I used it to monitor my daughter's glycemia. It is nice, comfortable and cheap solution.

We use Abbott Freestyle Libre, together with MiaoMiao transmitter. It transmits the glycemia values to her mobile phone (we used to use Spike on iPhone, but you can also use Tomato on iPhone or xDrip, Glimp and more on Android). The daughter’s mobile phone then sends data to the Nightscout, where we (parents) watch it using the web application or a mobile phone (Spike, Nightscout, …). We use the small M5Stack monitor with my software to monitor the glycemia during nights in our sleeping room, my wife has one in the kitchen and we take one always on our trips. It has different backlight intensity for night usage and alarms for hypo/hyper glycemia situations.

**In short it displays information from Nightscout site and provides visual and sound alarms if needed.**

Some people were asking for the software. I am not a professional programmer, I am a hardware engineer, but I work more with tables, numbers and e-mails recent years, so I was a little bit shy about the code. As there is quite a lot of people interested in something that shows current values and makes alarms during nights, I decided to release the code as an Open Source.

If you know what to do with it, just download the code, modify it any way it suits you and use it in M5Stack. If you do not know how to handle the source code, just find someone who can. It is easy, just open it in Arduino IDE, install the **M5Unified**, **ArduinoJson** and **Adafruit NeoPixel** libraries and build it to your M5Stack. The firmware uses the M5Unified library and auto-detects the board, so a single build supports the whole current 16&nbsp;MB lineup: **M5Stack Basic v2.7, Fire v2.7, Core2 v1.1 and CoreS3 (ESP32-S3)**. When building for the classic Basic (`M5Stack-Core-ESP32`) select the **Minimal SPIFFS** partition scheme so the firmware fits while keeping OTA updates; the other boards work with the default partition. The official M5Stack store is here https://m5stack.com/products/basic-core-iot-development-kit. The current price is about 27.95 USD (9 February 2020). You can also buy it on Aliexpress https://www.aliexpress.com/item/M5Stack-Official-Stock-Offer-ESP32-Basic-Core-Development-Kit-Extensible-Micro-Control-Wifi-BLE-IoT-Prototype/32837164440.html or elsewhere.

**You will need a microSD card.** It has to be formatted to FAT32 format and you have to put at least M5NS.INI file with configuration to the root of the microSD card. It is a good idea to put M5_NightscoutMon.jpg files to the SD card root too. You can replace M5_NightscoutMon.jpg with any 320x240 pixels picture if you want to customize your experience.

**You have to update M5NS.INI file to your requirements. As a minimum requirement you will need to update your Nightscout URL and your WiFi SSID and password.**<br/>
<br/>

### The syntax of M5NS.INI configuration file

```ini
[config]

data_source = 0                     ; glucose data source: 0 = Nightscout (default), 1 = Dexcom Share, 2 = LibreLinkUp,
                                    ; 3 = Medtronic CareLink
nightscout = yournightscoutsite.com ; use your Nightscout site URL (any host - Fly.io, Render, Azure, self-hosted, etc.)
token = security token              ; token for access secured Nightscout site, 
                                    ; if public Nightscout site is used, delete token line and do not use it
dexcom_user = your_dexcom_account   ; Dexcom Share account name, only used when data_source = 1
dexcom_pass = your_dexcom_password  ; Dexcom Share password, stored in plain text like token/wifi passwords above
                                    ; use the SHARING account, not a follower account
dexcom_server = 0                   ; Dexcom server region: 0 = US, 1 = outside US, 2 = Japan
libre_user = your_libre_email       ; LibreLinkUp account email, only used when data_source = 2
libre_pass = your_libre_password    ; LibreLinkUp password, stored in plain text like token/wifi passwords above
                                    ; use a LibreLinkUp FOLLOWER account, not the LibreView account itself
libre_server = 5                    ; LibreLinkUp region: 0=AE 1=AP 2=AU 3=CA 4=DE 5=EU(default) 6=EU2 7=FR 8=JP 9=US 10=LA 11=RU
                                    ; auto-corrects itself on first login if it doesn't match your account
cl_country = IE                     ; Medtronic CareLink country code, only used when data_source = 3
cl_patient =                        ; CareLink patient username to follow, empty = first linked patient
                                    ; the CareLink login itself is never written here, see Medtronic CareLink below
bootpic = /M5_NightscoutMon.jpg     ; boot picture
name = YourName                     ; display name
device_name = M5NS                  ; device name for mDNS, that can be used to access the device from local
                                    ; intranet, use .local domain. Can be omitted or left as the default M5NS:
                                    ; the device then picks its own name unique to it, M5NS-XXXX (XXXX derived
                                    ; from its MAC address), eg. m5ns-a1b2.local - this way several devices on
                                    ; the same network never collide on the same .local address. Set this key
                                    ; to anything else to pick your own fixed name instead.
disable_web_server = 0              ; To disable internal Web server enter 'disable_web_server = 1'
time_zone = 3600                    ; the time zone where you live in seconds (3600 is GMT+1, Prague)
dst = 3600                          ; your daylight saving time offset (3600s is +1 hour)
show_mgdl = 0                       ; use 0 to display values in mmol/L or 1 for mg/dl
sgv_only = 0                        ; set to 1 if you use xDrip, Spike or similar to filter out calibrations
                                    ; etc., do not use with Tomato (set 0)
default_page = 0                    ; page number displayed after startup
show_current_time = 1               ; show current time instead of last valid data time
restart_at_time = HH:MM             ; time in HH:MM format when the device will restart
restart_at_logged_errors = 0        ; restart device after particular number of errors in the log 
                                    ; (0 = do not restart)
show_COB_IOB = 1                    ; show COB and IOB, values are grayed out if COB/IOB value is 0
snooze_timeout = 30                 ; sound snooze time in minutes after press of the middle button
alarm_repeat = 5                    ; sound repeat interval in minutes (alarm repeats only if alarm/warning 
                                    ; conditions are met)
info_line = 1                       ; 0 = sensor info, 1 = button function icons, 
                                    ; 2 = loop info (Loop) + basal, 3 = OpenAPS loop info + basal
temperature_unit = 1                ; 1 = CELSIUS, 2 = KELVIN, 3 = FAHRENHEIT. Can be omitted (default Celsius).
display_rotation = 1                ; 1 = buttons down (default, can be omitted), 3 = buttons up, 
                                    ; 5 = mirror buttons up, 7 = mirror buttons down  
date_format = 0                     ; 0 = dd.mm. (default, can be omitted), 1 = MM/DD
time_format = 0                     ; 0 = 24 Hours (default, can be omitted), 1 = 12-hour AM/PM
brightness1 = 50                    ; the first (default) brightness value
brightness2 = 100                   ; the second brightness value
brightness3 = 10                    ; the third brightness value

;;;;;;;;;; following values are in mmol/L or mg/dL depending on the previous setting of "show_mgdl" key value_

yellow_low = 4.5           ; glycemia values under this value will be displayed in yellow color
yellow_high = 9            ; glycemia values over this value will be displayed in yellow color
red_low = 3.9              ; glycemia values under this value will be displayed in red color
red_high = 11              ; glycemia values over this value will be displayed in red color
snd_warning = 3.7          ; softer high beep sound will beep every 5 mins when under this value
snd_alarm = 3.0            ; strong alarm will sound every 5 min when under this value
snd_warning_high = 14.0    ; softer high beep sound will beep every 5 mins when over this value
snd_alarm_high = 20.0      ; strong alarm will sound every 5 min when over this value
snd_no_readings = 20       ; softer high beep sound will beep every 5 mins when time in minutes when last data was read 
                           ; is over this value
snd_loop_error = 1         ; 0 = no loop error alarm, 1 = stronger sound alarm to loop error
snd_warning_at_startup = 1 ; play test warning sound with its volume during startup (1 = play, 0 =  do not play)
snd_alarm_at_startup = 0   ; play test alarm sound with its volume during startup (1 = play, 0 =  do not play)
warning_volume = 20        ; volume of warning sound in range 0-100 (0=no sound, 1=silent, 100=max volume)
alarm_volume = 100         ; volume of alarm sound in range 0-100 (0=no sound, 1=silent, 100=max volume)

;;;;;;;;;; there are supported some external devices, take care and know what you are doing when you connect it

LED_strip_mode = 0         ; external RGB LED strip or internal M5Stack Fire RGB LED support
                           ; 0 = off, 1 = visualize sound, 2 = show warnings and alarms, 3 = light always
LED_strip_pin = 15         ; pin that controls LEDs, 15 = M5Stack Fire internal pin
                           ; 21 = red PORT A connector (not recommended, collides with I2C, first LEDs will blink occassionaly
                           ; 26 = black PORT B connector, 17 = blue PORT C connector
LED_strip_count = 10       ; 10 = number of M5Stack Fire internal LEDs,
                           ; 15 = 10 cm strip, 29 = 20 cm strip, 72 = 50 cm strip, 144 = 100 cm strip, 288 = 200 cm strip
LED_strip_brightness = 10  ; 1-100% brightness intesity (100 = max.) 
                           ; for more than 10 LEDs use 10%, 5% or even less otherwise loaded current will reset the M5Stack

vibration_mode = 0         ; VIBRATION MOTOR UNIT ; 0 = off, 1 = ON (vibrate during sound)
vibration_pin = 26         ; pin that controls VIBRATION UNIT motor, 26 = M5Stack Fire PORT B connector, 17 = blue PORT C connector
                           ; do not connect vibration motor to red PORT A, it will rotate always as it will be in collision with I2C
vibration_strength = 512   ; 10 bit PWM value for VIBRATION UNIT motor control, 512 = 1/2 of max. power, do not use more, full power
                           ; will reset the M5Stack, recommended range is 256-512

micro_dot_pHAT = 0         ; 0 = off, 1 = ON (display SGV and DELTA on I2C Pimoroni Micro Dot pHAT connected to I2C pins 21+22)

;;;;;;;;;;  you can connect up to 9 WiFi access points, no need to setup all, one is enough

[wlan1]
ssid = ssid1  
pass = pass1

[wlan2]  
ssid = ssid2  
pass = pass2

[wlan3]  
...
...
```

### Medtronic CareLink

`data_source = 3` reads sensor glucose straight from Medtronic CareLink, the same data the CareLink Connect app shows for a MiniMed 780G or Simplera. No Nightscout site and no Android bridge phone running xDrip+ are needed.

**What you need**

* A CareLink **care partner** account, linked to the patient's account in the MiniMed Mobile app, used **only** by this display. If the same care partner account also signs in to the CareLink Connect app or another tool, CareLink cancels the display's login.
* The patient's phone uploading to CareLink as usual.

**Setting it up**

1. On the web config page, choose *Medtronic CareLink* under *Data source*, set the country (two letters, e.g. `IE`), and press *Save* (the device restarts).
2. Open the config page again and follow *CareLink login: sign in*. The `/carelink` page links to the CareLink sign-in page for your country.
3. Sign in with the care partner account. When sign-in finishes, the browser tries to open the CareLink app and shows an error or a blank page. Copy the full address from the address bar and paste it into the `/carelink` page straight away (the code in it is single-use and expires within minutes). If a phone browser hides that address, do this step on a computer.
4. Alternatively, paste the contents of a `logindata.json` file made by the login script of [carelink-python-client](https://github.com/ondrej1024/carelink-python-client).

The device then keeps itself signed in: it renews its access with the stored refresh token and saves each new one to flash. The login is kept in its own flash namespace and is never written to `M5NS.INI` or the SD card. If CareLink rejects it (for example after the account was used elsewhere, or the device was offline for a long time), the status shows *Sign-in needed* and error 1302 appears in the log; sign in again on `/carelink`.

**What is shown:** current glucose, the Medtronic trend arrow (one arrow = Nightscout single 45° arrow, two = single arrow, three = double arrow, no arrow = flat), delta, the graph and active insulin (IOB). Pump-local times that CareLink labels as UTC are corrected the same way xDrip does.

**Error log codes:** 1301 CareLink communication/API error, 1302 sign-in needed, 1303 no patient linked to the care partner account, 1304 country not set or not supported, 1004 no current reading (sensor warming up, out of range).

CareLink access is unofficial and Medtronic can change it without notice. Treat the display as a secondary display and keep the official app's alarms on.

The parsing code (`M5NSCareLinkParse.h`) has host-side unit tests in `test/carelink_parse_test.cpp`:

```
g++ -std=c++17 -I. -I<path to ArduinoJson>/src test/carelink_parse_test.cpp -o cltest && ./cltest
```

### Display

There are currently 5 display pages, that can be changed by a short press of the right button. The button does not work when the device communicated with Nightscout (small blue WiFi icon is displayed).

**Page 0 - default page**  
What you see on the display is a matter of your M5NS.INI and Nightscout configuration. By default you will see current time, date and user name from config file in upper left corner. You will see IOB (Insulin On Board) an COB (Carbs On Board) values below the name. Right from the IOB/COB value is current BG delta (difference from the last reading, trend where the BG is going). Box with time difference from last valid data from Nighscout is displayed in upper right corner. Delay of 5 mins is OK and the box has grey color, 5-15 mins is warning and the box will have white background, more than 15 mins without a valid data is error and the box background will be red. There is also possibility to set sound warning on data delay in M5NS.INI.

Glycemia (BG) value and its direction are below the basic information. There is a mini graph created from 10 last BG values right of the glycemia value. The mini graph shows only values in range 3-12 mmol/L (48-216 mg/dL) and values below/above this will be displayed on mini graph as the min/max values.

There are also icons in upper side of display. Plug or battery icon displays battery status (on newer devices only). Red clock indicated silent "snooze" mode activated. Grey or yellow (more than 5 errors) shows that there are errors in error log (last page). Blue WiFi icon in 2 sizes (2 different queries) indicated that MStack is downloading data from Nightscout.

Last line contains icons for buttons or data source or Loop information (depending on M5NS.INI key "info_line").  

**Page 1 - simple page with large BG value**  
Long distance visible page with large BG value and larger clock. Top line contains current time, icons, delta value a BG direction arrow.

Bottom info line is the same as on page 0.

**Page 2 - analog clock with temperature and humidity**  
Large analog clock with date. BG value in top left corner with time difference from the last reading. BG delta value in top right corner and arrow pointing chinge direction below it.

This screen contatins environmental values form DHT12 sensor. Requires ENV Unit or BTC Standing Base connected to MStack. The temperature is in lower left corner and humidity in lower right corner. Temperature unit can be set in M5NS.INI.

Bottom info line is the same as on page 0.

**Error Log page**
Displays last 10 record errors and total number of errors record since last restart. Occasional communication errors, such as "timeout" or "connection refused" are normal and happen sometimes due to WiFi and internet connection and Nightscout site status. If you have a lot of errors every few minutes, consider to move your M5Stack to a different place or extend WiFi signal.  

**Web config QR code page**
Shows a QR code encoding the device's local IP address (`http://a.b.c.d`). Scan it with a phone camera to open the device's web configuration page directly, without needing to type in an address or rely on `<name>.local` (which doesn't work reliably if several M5NS devices are on the same network). If WiFi is disconnected or the internal web server is disabled in M5NS.INI, the page shows a message instead of the QR code. On entry the device checks GitHub for a newer firmware release; if one is available, the bottom row shows **UPDATE** and the middle button installs it directly from the device, with no browser needed.

Neither the Error Log page nor this page show the loop/basal status row (`L: ... B: ...`) that appears on the other pages - it isn't relevant here, and on this page the middle button has a different job.
<br/>

### Buttons

The **left button** changes the backlight in the 3 steps defined in M5NS.INI file.

The **middle button** snoozes M5Stack for defined time to prevent alarm or warning sound. You can "snooze" it even before the alarm is activated. Snooze active is indicated by red clock icon in status icon area. You can press "snooze" more times to make snooze time longer. It cycles 1x - 2x - 3x - 4x - OFF and again. You have 2s to press button again, otherwise it starts from 1x. The "snooze" function is distributed to all devices in your local network with the same Nightscout user (Nightscout URL).

On the **web config QR code page** only, the middle button does not snooze - it installs a firmware update if one is shown as available (see above), and does nothing otherwise. It goes back to snoozing as soon as you switch to any other page.

Short press of the **right button** switches display pages. Long press (4 seconds) puts M5Stack to the sleep as it is a little bit tricky to double click standard red power button.  
<br/>

### Battery

Battery icon feature works only on newer M5Stack units. Here are my battery measurements and battery icon statuses:

| internal battery total                   |  43 min |
| ---------------------------------------- | ------: |
| plug icon _(full charge or plugged in)_  |   1 min |
| gray full battery icon                   |  13 min |
| white 2/3 battery icon                   |  20 min |
| yellow 1/3 battery icon                  |   8 min |
| red empty battery icon                   |   1 min |

---

| extended battery total                   |  7h 49 min |
| ---------------------------------------- | ---------: |
| plug icon _(full charge or plugged in)_  |        53m |
| gray full battery icon                   | 2 h 59 min |
| white 2/3 battery icon                   | 1 h 55 min |
| yellow 1/3 battery icon                  | 1 h 48 min |
| red empty battery icon                   |     14 min |

### Installation and support

Installation can be done by download or git clone the code to the Arduino IDE environment. You have to download ESP32 board and M5Stack libraries, Arduino JSON library and build it to you M5Stack.

Another easier possibility is to download latest [M5Burner release](https://github.com/mlukasek/M5_NightscoutMon/releases). It is Windows executable with binary M5Stack firmware included. Just unzip it, start the M5Burner.exe, choose COM port where your M5Stack is connected and burn the firmware. Simple. Remember to add you microSD card with M5NS.INI configuration file. See the [wiki](https://github.com/mlukasek/M5_NightscoutMon/wiki) for details.

There is a Facebook group [M5STACK NIGHTSCOUT](https://www.facebook.com/groups/606295776549008/) where you can get support and installation guides in several languages prepared by members of the M5Stack Nightscout community. Please check the [Files section](https://www.facebook.com/groups/606295776549008/files/) first and search the group before asking questions. A lot of questions have been answered already. Big thanks to Patrick Sonnerat, Didier Frétigné, Peter Leimbach and more...

### Firmware updates (OTA)

Once your M5Stack is set up and connected to Wi-Fi, you don't need a computer to keep it up to date. Open the device's web page (see [Setup](https://github.com/psonnera/M5_NightscoutMon/wiki/Setup#update-your-firmware) in the wiki) and, if a newer version is available, a **click to update** link appears. Click it and the device downloads and flashes the new firmware itself, then reboots.

Firmware is served directly from this GitHub repository — the [`Binaries/`](Binaries) folder on `master` — over HTTPS; there is no separate update server. The device automatically requests the binary matching its own board (`Basic_4MB`, `ESP32_16MB` or `CoreS3`, see [`Scripts/README.md`](Scripts/README.md#the-three-firmwares)), so a Basic, Fire, Core2 or CoreS3 all get the correct image without any manual selection.

If you maintain your own fork, see [`Scripts/README.md`](Scripts/README.md#publishing-an-ota-update) for how to build and publish updates for it.

### License and credits

M5_NightscoutMon is free software under the **[GNU GPL v3 (or later)](LICENSE)**. Copyright (C) 2018-2023 Martin Lukasek, Copyright (C) 2021-2026 Patrick Sonnerat and contributors.

**Third-party code included in this repository:**

| Code | Origin | License |
| ---- | ------ | ------- |
| `IniFile.cpp/h` | [IniFile](https://github.com/stevemarple/IniFile) by Steve Marple (modified) | GNU LGPL v2.1 |
| `iot_iconset_16x16.c` | [IoT Icon Set](http://engsta.com/iot-icon-set-for-i2c-oled-displays/) by Artur Funk | GPL v3 |
| `DHT12.cpp/h` | DHT12 library by Bobadas | Public domain |
| `SHT3X.cpp/h` | M5Stack ENV unit example code | MIT |
| `Free_Fonts.h` | Font aliases from [Bodmer's TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) examples | FreeBSD |
| `hal_ws_touchlcd35.cpp/h` | ST7796 panel init list from [LovyanGFX](https://github.com/lovyan03/LovyanGFX) by lovyan03; ES8311 codec setup condensed from Espressif's [es8311 driver](https://github.com/espressif/esp-bsp/tree/master/components/es8311) (Waveshare ESP32-Touch-LCD-3.5 build only) | FreeBSD / Apache 2.0 |
| `M5NSDexcom.cpp/h` | Dexcom Share protocol adapted from [ktomy/nightscout-clock](https://github.com/ktomy/nightscout-clock) (`BGSourceDexcom`) | [GNU LGPL v2.1](https://github.com/ktomy/nightscout-clock/blob/main/LICENSE) |
| `M5NSLibre.cpp/h` | LibreLinkUp protocol adapted from [ktomy/nightscout-clock](https://github.com/ktomy/nightscout-clock) (`BGSourceLibreLinkUp`) | [GNU LGPL v2.1](https://github.com/ktomy/nightscout-clock/blob/main/LICENSE) |
| `M5NSCareLink.cpp/h`, `M5NSCareLinkParse.h` | Medtronic CareLink follower protocol as implemented by [NightscoutFoundation/xDrip](https://github.com/NightscoutFoundation/xDrip) (`cgm/carelinkfollow`) and [ondrej1024/carelink-python-client](https://github.com/ondrej1024/carelink-python-client) | written for this project; references are GPL v3 and MIT |

**Library dependencies** (installed separately, not part of this repository): [M5Unified](https://github.com/m5stack/M5Unified) and [M5GFX](https://github.com/m5stack/M5GFX) by M5Stack (MIT), [ArduinoJson](https://arduinojson.org/) by Benoit Blanchon (MIT), [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) (LGPL v3), and — for the Guition JC3248W535 build only — [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) by moononournation (BSD 2-Clause). The [web flasher page](https://psonnera.github.io/M5_NightscoutMon/Flasher/) uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Apache 2.0).

**Contributors and additions:** Peter Leimbach (Nightscout token), Sulka Haro (Nightscout API queries help), Ben West (AP WiFi configuration), Dominik Dzienia (refactoring, migration to Platform.IO), Patrick Sonnerat and Claude (M5Unified migration, Dexcom Share and LibreLinkUp data sources, web configuration, QR codes, JC3248W535 and Waveshare ESP32-Touch-LCD-3.5 ports, web flasher), DiabetesHub Ireland and A Parent (Medtronic CareLink data source), and everyone in the [commit history](https://github.com/psonnera/M5_NightscoutMon/graphs/contributors).

The [M5Stack Loader](https://github.com/psonnera/M5StackLoader) Android app that installs this firmware is a separate project, also GPL-3.0-or-later, with its own [third-party notices](https://github.com/psonnera/M5StackLoader/blob/master/THIRD-PARTY-NOTICES.md).

### Donations

Donations support the project's original author and maintainer, **Martin Lukasek**. If you find this project useful, he will appreciate if you donate him few bucks for further development or buy him a glass of beer or wine :wine_glass:.

https://paypal.me/8bity

He also collects and repairs vintage [8-bit computers](http://www.8bity.cz/), so if you have some really old computer, he could give it a new home and care :smile:. Thank you!

Don't forget you can also donate to the [Nightscout Foundation](https://www.nightscoutfoundation.org/donate).
