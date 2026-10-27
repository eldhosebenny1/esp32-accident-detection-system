# Smart Accident Detection System

An ESP32 that watches a vehicle for a crash, brakes the wheels by itself, and puts
the whole incident on a local web page you can open from any phone on the same
Wi-Fi. There is no app and no cloud account involved - the ESP32 hosts the
dashboard itself.

Built as a college exhibition project.

## What it does

An MPU6050 sits on the chassis. The sketch reads the accelerometer every loop and
works out the combined G value. When that crosses 4 G the board treats it as a
possible crash and starts a 20 second countdown:

- the servo pulls the brake on gradually, in small steps
- the red LED comes on and the buzzer starts beeping
- the dashboard switches to a "possible impact" banner
- a physical **I'M SAFE** button gives the driver a way out

Press nothing for 20 seconds and the accident is confirmed. The brake stays fully
applied, the buzzer holds, and the incident is written into the dashboard with the
GPS position, a Google Maps link and the nearest of three reference hospitals.
There is also a hard-wired **SOS** button that skips the countdown entirely.

Separately, the HC-SR04 keeps an eye on what is ahead. It brakes in four stages as
the gap closes and slams the brake on if the distance drops by more than 30 cm in
under 300 ms.

## Hardware

| Part | Purpose |
| --- | --- |
| ESP32 DevKit | Controller and web server |
| MPU6050 | Crash detection, read over I2C |
| NEO-6M GPS module | Position for the incident report |
| HC-SR04 ultrasonic | Distance to obstacles |
| SG90 / MG996R servo | Pulls the brake lever |
| Buzzer + red LED | Local alarm |
| 2 push buttons | SOS and I'M SAFE |
| Demo car chassis | Anything with wheels and a brake linkage |

## Wiring

| Signal | ESP32 GPIO | Notes |
| --- | --- | --- |
| MPU6050 SDA / SCL | 21 / 22 | I2C, address 0x68 |
| GPS TX to ESP32 RX | 17 | UART1, 9600 baud |
| GPS RX from ESP32 TX | 16 | |
| HC-SR04 TRIG | 2 | |
| HC-SR04 ECHO | 34 | Input-only pin |
| Brake servo | 32 | 50 Hz, 500-2400 us |
| Buzzer | 13 | |
| Red LED | 12 | |
| SOS button | 4 | Wire between the pin and GND, internal pull-up |
| I'M SAFE button | 5 | Wire between the pin and GND, internal pull-up |

The ECHO line of an HC-SR04 idles at 5 V. GPIO 34 is input-only and not 5 V
tolerant, so run it through a simple divider (1k to the pin, 2k to ground) rather
than connecting it straight.

The servo draws a lot of current when it stalls against the brake lever. Power it
from a separate 5 V supply with a common ground, not from the ESP32 3V3 rail.

## Libraries

| Library | Author |
| --- | --- |
| ESP32Servo | Kevin Harrington, John K. Bennett |
| TinyGPSPlus | Mikal Hart |

`Wire`, `WiFi` and `WebServer` come with the ESP32 board package.

```
Boards Manager URL: https://espressif.github.io/arduino-esp32/package_esp32_index.json
Board: ESP32 Dev Module
```

## Setup

1. Put your Wi-Fi name and password into `WIFI_SSID` and `WIFI_PASSWORD` at the top
   of `accident_monitor/accident_monitor.ino`. The committed placeholders are
   `YOUR_WIFI_SSID` and `YOUR_WIFI_PASSWORD`.
2. Flash the board and open the serial monitor at 115200 baud. It prints a
   `Dashboard URL` line as soon as it joins the network.
3. Open that address on a phone or laptop that is on the same Wi-Fi.
4. Wait for `GPS fix: YES` in the serial log before expecting a real location on
   the dashboard. Cold starts can take a couple of minutes with a clear sky view.

If the board loses Wi-Fi it retries every 10 seconds, and the dashboard itself
keeps serving on the local network either way.

## Dashboard

The page polls `http://<esp32-ip>/data` once a second and draws everything from
that JSON, so nothing needs to be pushed from the board.

- System status, current G, peak G and brake position
- An impact history graph, 60 samples at 100 ms each, so roughly the last six
  seconds
- Live latitude, longitude and satellite count
- The emergency report: incident status, nearest reference hospital, distance and
  a link that opens the position in Google Maps
- Vehicle safety flags: candidate, confirmed, manual SOS, LED/buzzer state
- GPS mode, which shows whether the report used a real fix or the fallback

The whole page is embedded in the sketch as a raw string literal, so there is no
filesystem or SPIFFS upload step.

## GET /data

| Field | Meaning |
| --- | --- |
| `gpsFix` | True when there is a valid fix younger than 10 s |
| `satellites` | Satellites in view |
| `lat`, `lon` | Current position, or `null` without a fix |
| `currentG`, `peakG` | Latest and highest acceleration magnitude |
| `brakePosition` | Servo angle, 0 to `BRAKE_MAX` |
| `accidentCandidate` | Inside the 20 s confirmation window |
| `accidentConfirmed` | Latched emergency state |
| `manualSOS` | Set when the SOS button was used |
| `alarmOn` | LED and buzzer active |
| `incidentDetailsAvailable` | An incident report has been built |
| `demoLocation` | The report fell back to the demo coordinates |
| `incidentLat`, `incidentLon` | Position stored in the report |
| `hospitalDistanceKm`, `hospitalName` | Nearest reference hospital |
| `mapLink` | Google Maps URL for the incident position |
| `incidentNumber`, `incidentStatus` | Counter and text status |
| `lastEvent` | Most recent event line |
| `history` | The impact graph samples, oldest first |

## Calibration

| Setting | Default | What to change it for |
| --- | --- | --- |
| `BRAKE_RELEASE` | 0 | Servo angle with the brake fully off |
| `BRAKE_MAX` | 110 | Servo angle with the brake hard on. Back this off if the servo buzzes at the end of travel |
| `IMPACT_G` | 4.0 | Lower it to trigger more easily, raise it if bumps set it off |
| `CONFIRM_TIME` | 20000 | Length of the countdown in milliseconds |

Calibrate both brake angles with the vehicle lifted off the ground so the wheels
can turn freely. Check the acceleration reading in the serial log while tapping
the chassis to pick a sensible `IMPACT_G`.

## Things worth knowing

- The three hospitals are placeholders generated a short distance from the
  incident, not real hospital records. The names say "Demo Hospital" on purpose.
  Swap `createDemoHospitals()` for a real list or an API if you need actual ones.
- Without a GPS fix the report falls back to a fixed demo coordinate, and the
  dashboard marks the source as DEMO so it is obvious the position is not real.
- The original version sent the alert to Telegram. That has been taken out so the
  project runs with no internet service and no bot token. All incident data now
  lives on the dashboard instead.
- Once an accident is confirmed the state is latched. Only a power cycle or a
  reflash clears it, which is deliberate so the demo cannot silently reset.

## Repo layout

```
accident_monitor/accident_monitor.ino   Firmware and embedded dashboard
```

## License

MIT, see [LICENSE](LICENSE).
