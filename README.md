# MBTA Boston Station Board (ESP8266)

A tiny departure board for a 16x2 I2C LCD, driven by an ESP8266. It shows live
MBTA arrivals and departures for **South Station** and **North Station** in
Boston, plus service alerts, using the [MBTA V3 API](https://api-v3.mbta.com/).

| Station | Screens |
|---|---|
| South Station | Red Line, Silver Line (SL1-SL5), Commuter Rail |
| North Station | Orange Line, Green Line (B/C/D/E), Commuter Rail |

After the arrival screens, the display scrolls any active service alerts for
those lines (plus the Blue Line and Mattapan trolley), most severe first.

## What the display looks like

```
Alewife        3min
Ashmont/Brai   7min
```

- Destination on the left (scrolls if longer than 10 characters), countdown on the right.
- Commuter rail destinations end with the track, e.g. `Worcester T5`.
- Silver Line shows the route (`SL1 Logan Airport`) and Green Line shows the branch (`E Heath Street`).
- `*` after a time means it is **scheduled**, not a real-time prediction.
- `BRD` means boarding now. Anything over 99 minutes is shown as a clock time.
- When the display moves to a different station, a title card shows for 1.5 seconds.
- Screens with no departures are skipped.

## Hardware

- Any ESP8266 board (NodeMCU, Wemos/LOLIN D1 mini, etc.)
- 16x2 character LCD with an I2C backpack (PCF8574, address `0x27` by default)

| LCD backpack | ESP8266 |
|---|---|
| VCC | 5V (or 3.3V if your module allows) |
| GND | GND |
| SDA | GPIO4 (D2) |
| SCL | GPIO5 (D1) |

Pins and the I2C address are set at the top of the sketch (`SDA_PIN`, `SCL_PIN`,
`LCD_ADDR`). If the screen stays blank, adjust the contrast pot on the backpack,
and try address `0x3F` (some backpacks use it instead of `0x27`).

## Software requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x or `arduino-cli`
- **ESP8266 Arduino core** (esp8266 by ESP8266 Community)
- **ArduinoJson** v7 (the sketch uses `JsonDocument`, which does not exist in v6)
- **LiquidCrystal I2C** (by Frank de Brabander)

The `LiquidCrystal_I2C` "claims to run on all architectures" warning during
compilation is harmless.

## Installation

### 1. Get the code

Arduino requires the sketch folder and the `.ino` file to share a name:

```
.gitignore
README.md
mbta_boston_stations/
├── mbta_boston_stations.ino
└── secrets.h.example
```

```bash
git clone <your-repo-url>
cd <your-repo>
```

### 2. Install the ESP8266 core

Arduino IDE: **File > Preferences > Additional boards manager URLs**, add:

```
http://arduino.esp8266.com/stable/package_esp8266com_index.json
```

Then **Tools > Board > Boards Manager**, search for `esp8266`, and install it.

### 3. Install the libraries

**Sketch > Include Library > Manage Libraries**, then install:

- `ArduinoJson` (version 7.x)
- `LiquidCrystal I2C` (Frank de Brabander)

### 4. Configure

Your credentials go in a `secrets.h` file that is **not** committed
(it is listed in `.gitignore`). Copy the example next to the sketch and fill it in:

```bash
cp mbta_boston_stations/secrets.h.example mbta_boston_stations/secrets.h
```

```cpp
#define SECRET_WIFI_SSID "your-wifi-name"
#define SECRET_WIFI_PASS "your-wifi-password"
#define SECRET_MBTA_KEY  "your-mbta-api-key"   // "" to run without a key
```

Get a free API key at <https://api-v3.mbta.com/>. It is optional, but
unauthenticated requests are heavily rate limited, and this sketch makes
several requests per minute.

The sketch will not compile until `secrets.h` exists. In the Arduino IDE it
appears as a second tab once the file is in the sketch folder.

### 5. Build and upload

Arduino IDE:

- **Tools > Board:** your ESP8266 board (e.g. *NodeMCU 1.0 (ESP-12E Module)* or *LOLIN(WEMOS) D1 R2 & mini*)
- **Tools > Port:** your serial port
- Click **Upload**, then open the Serial Monitor at **115200 baud** to watch the logs.

arduino-cli:

```bash
arduino-cli core update-index --additional-urls http://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core install esp8266:esp8266 --additional-urls http://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "ArduinoJson" "LiquidCrystal I2C"

arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 mbta_boston_stations
arduino-cli upload  --fqbn esp8266:esp8266:nodemcuv2 -p /dev/ttyUSB0 mbta_boston_stations
```

Use `esp8266:esp8266:d1_mini` instead of `nodemcuv2` for a D1 mini. Use
`arduino-cli lib list` to confirm ArduinoJson is 7.x.

## How it works

- **Predictions** (real-time) are fetched per station every 30 s. **Schedules**
  are fetched per station every 5 minutes, for commuter rail only, to fill in
  trains that don't have a prediction yet. Subway and Silver Line trains always
  have predictions.
- Requests are staggered, one per loop tick, so the display doesn't freeze.
- Time comes from NTP, with the `EST5EDT` timezone rule (handles daylight saving).
- Alerts refresh every 2 minutes, and commuter rail track numbers every 10 minutes.

### Memory

The ESP8266 has very little RAM, so the sketch is built around it:

- No Arduino `String` in stored data. IDs are 32-bit hashes, and display text is
  fixed `char` buffers built once at parse time.
- The merged event list holds only pointers into the prediction/schedule arrays.
- JSON is streamed through an ArduinoJson filter, so only the needed fields are kept.
- URLs, LCD text and `printf` formats live in flash (`PROGMEM`).
- At boot the sketch probes whether the MBTA server supports TLS
  **Max Fragment Length Negotiation**. If it does, the TLS buffers drop from
  16 KB to 512-4096 bytes. The result prints to Serial as `TLS buffer: N`.
  `0` means the server doesn't support it and the full-size buffers are used.
- TLS certificates are **not validated** (`setInsecure()`). That saves RAM and
  flash, but it means the connection is encrypted but not authenticated.

The main tuning knobs are at the top of the sketch: `PRED_PER_STATION`,
`SCHED_PER_STATION`, `SCHED_WINDOW_MIN`, `MAX_ALERTS`, and `ALERT_LEN`.

## Customizing

### Add or change a station

Stations are defined in a table near the top of the sketch:

```cpp
static const char ST1_ID[]     PROGMEM = "place-north";
static const char ST1_NAME[]   PROGMEM = "NORTH STATION";
static const char ST1_SUBWAY[] PROGMEM = "Orange,Green-B,Green-C,Green-D,Green-E";
static const char ST1_CR[]     PROGMEM = "CR-Fitchburg,CR-Lowell,CR-Haverhill,CR-Newburyport";
```

1. Add a new set of `STn_*` strings and a row in `STATIONS[]`.
2. Add the station's screens to `SCREENS[]`, e.g. `{2, BLUE_LINE}`.
3. Find the right IDs with the API:
   - Station ID: `https://api-v3.mbta.com/stops?filter[location_type]=1&filter[name]=State` (or look it up on the MBTA site)
   - Routes serving it: `https://api-v3.mbta.com/routes?filter[stop]=place-state`

Array sizes scale automatically with the number of stations. More stations mean
more RAM and more requests per minute, so use an API key.

Red, Orange, Blue, Green branches, Silver Line (741/742/743/749/751) and
commuter rail (`CR-*`) are recognized. Bus routes are ignored.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Blank LCD | Wrong I2C address (`0x27` vs `0x3F`), contrast pot, or SDA/SCL swapped |
| Stuck on "SYNCING CLOCK" | No internet, or NTP blocked by the network |
| `HTTP -1` or `-2` in Serial | TLS or connection failure. Check the heap values in the log. A low `block` value means fragmentation |
| `HTTP 429` | Rate limited. Add an API key, or raise the refresh intervals |
| `HTTP 400` on alerts | Remove `sort=-severity` from the alerts URL in `buildURL()` |
| `JSON error: NoMemory` | Out of RAM. Lower `PRED_PER_STATION` / `ALERT_LEN` and check whether the TLS buffer probe succeeded |
| Missing trains | The per-request limit was hit. Raise `PRED_PER_STATION` if RAM allows |
| Resets or crashes | Almost always out of heap. Watch the `heap:` values in the Serial log |

## Limitations

- Results are limited per request, and the API's ordering isn't specified, so a
  very busy station can drop a train. Raise `PRED_PER_STATION` if RAM allows.
- Commuter rail schedules cover only the next `SCHED_WINDOW_MIN` minutes.
- Trip and stop IDs are 32-bit hashes, so a collision is theoretically possible
  but very unlikely at this scale.
- TLS certificates are not verified (see Memory).

## Data and license

Transit data comes from the [MBTA V3 API](https://api-v3.mbta.com/) and is
subject to the
[MassDOT Developers License Agreement](https://www.mbta.com/developers). This
project is not affiliated with or endorsed by the MBTA.

Choose and add a license for this code (for example MIT) in a `LICENSE` file.
