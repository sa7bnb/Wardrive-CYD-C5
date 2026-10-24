# WARDRIVE CYD-C5

![WARDRIVE CYD-C5](https://raw.githubusercontent.com/sa7bnb/Wardrive-CYD-C5/main/cyd-c5.png)

A simple Wi-Fi wardriver for the **NM-CYD-C5** board (ESP32-C5, 2.8" ST7789 LCD with resistive touch, microSD slot). It scans 2.4 GHz and/or 5 GHz Wi-Fi, shows GPS status on the display, and logs every network it finds to the SD card in **WiGLE CSV** format.

Everything runs on the board. There is no web server and no Wi-Fi access point. The UI is shown on the LCD and operated with the touch screen.

![Main screen](images/main_screen.png)

*Simulated render of the display layout, not a photo.*

## Features

- Scans **2.4 GHz**, **5 GHz**, or **both** (selectable on the screen and remembered after restart)
- Logs to the SD card in **WiGLE CSV 1.4** format, ready for upload at [wigle.net](https://wigle.net)
- GPS status on screen: fix state, satellites, HDOP, coordinates, altitude, and a satellite bar graph
- Reads **GPS and BeiDou** NMEA sentences (GGA, RMC, GSA)
- Touch calibration with four corner targets. Values are saved in flash, so you only calibrate once
- Duplicate filter: each BSSID is written once per session, unless `LOG_ALL_SIGHTINGS` is set
- Green-on-black "hacker" UI with a 2 Hz status refresh and minimal screen flicker

## Hardware

Where to buy (AliExpress):

- https://www.aliexpress.com/item/1005012284431494.html
- https://www.aliexpress.com/item/1005002106176312.html

| Part | Notes |
|---|---|
| NM-CYD-C5 board | ESP32-C5, 2.8" ST7789 display, XPT2046 touch, microSD slot |
| GPS module | GPS+BDS Beidou dual-mode module (ATGM336H-compatible, NEO-M8N replacement). NMEA at 9600 baud by default. Needs an antenna with clear sky view |
| microSD card | Formatted as FAT32 |

### Wiring

| Function | GPIO / pin | Notes |
|---|---|---|
| GPS RX (from GPS module TX) | **GPIO8** | Header pin. Connect only the module's TX line |
| GPS power | 3.3 V and GND | Header pins next to GPIO8 |
| SD card CS | GPIO10 | Shares the SPI bus with display and touch |
| SPI bus | SCK 6, MISO 2, MOSI 7 | Shared by LCD, touch and SD |
| LCD CS / DC / backlight | 23 / 24 / 25 | Backlight on when HIGH |
| Touch CS | GPIO1 | Touch IRQ not used |

## Arduino IDE setup

1. Install **Arduino IDE 2.x**.
2. Install the board package **esp32 by Espressif Systems**, version **3.3.x**. Select the board **ESP32C5 Dev Module**.
3. Install the libraries from the Library Manager:
   - **GFX Library for Arduino** (by moononournation), version 1.6.9 or later
   - **XPT2046_Touchscreen** (by Paul Stoffregen)
4. Set these options under **Tools**:

   | Option | Value |
   |---|---|
   | USB CDC On Boot | Enabled |
   | PSRAM | Disabled |
   | Flash Size | 16MB |

5. Open `wardrive_cal.ino` and click **Upload**.

## First start and calibration

The touch screen must be calibrated once. On first boot the calibration screen appears automatically.

![Calibration screen](images/calibration_screen.png)

1. Tap the centre of each cross in turn: top left, top right, bottom right, bottom left.
2. When all four are done the screen shows **CALIBRATED** and the main screen opens.
3. If it shows **CALIBRATION FAILED**, the taps were too weak or too close together. It restarts automatically.
4. Run calibration again at any time with the **[CAL]** button in the top right corner.

The screen detects whether X and Y are swapped and whether an axis is inverted, so you don't need to change any rotation settings.

## Using the device

- **Band buttons:** `2.4G`, `5G`, `2.4+5G`. The selected band is highlighted.
- **START / STOP:** Starts or stops logging. A new file is created on the SD card for each session.
- **Statistics:**
  - `UNIQ`: unique networks this session
  - `ROWS`: lines written to the file
  - `UTC`: GPS time
  - `24G` / `5G`: networks seen in the last scan
  - `SCAN`: number of scans done
  - `SD`: card status
- **Latest new SSIDs:** the three most recently found networks.

### GPS status

| Status | Meaning |
|---|---|
| Green dot, `GPS FIX` | Position fix. Coordinates are written to the log |
| Yellow dot, `SEARCHING SATS` | Module has data but no fix yet |
| Red dot, `NO NMEA` | No data from the module. Check wiring and baud rate |

A cold start can take 2 to 5 minutes outdoors. Indoors, a fix is often not possible.

## Log files

Files are created on the SD card when logging starts:

- `wd_YYYYMMDD_HHMMSS.csv`: name uses GPS time when available (UTC)
- `wd_bootNNNNNNNNNN.csv`: used when there is no GPS time yet

Each file starts with the WiGLE header, followed by one row per network:

```
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
24:4B:FE:4D:3C:00,ExampleNet,[WPA2-PSK][CCMP][ESS],2026-10-24 09:43:20,9,2452,-62,59.3293000,18.0686000,21.0,7.5,WIFI
```

All times are UTC. Stop logging before removing the SD card so the file is closed properly.

## Settings

Constants at the top of `wardrive_cal.ino`:

| Constant | Default | Purpose |
|---|---|---|
| `SCAN_MS_PER_CH` | 100 | Time per channel in ms. 200 finds more networks but is slower. 60 is the fastest |
| `TOUCH_Z_MIN` | 1000 | Minimum touch pressure. Raise it if ghost touches occur, lower it if taps are missed |
| `LOG_ALL_SIGHTINGS` | 0 | 0 = one row per network per session, 1 = a row every scan |
| `GPS_RX_PIN` | 8 | GPIO connected to the GPS module TX |
| `GPS_BAUD` | 9600 | GPS module baud rate |
| `GPS_DEBUG_RAW` | 0 | 1 = print every raw NMEA sentence to Serial Monitor (testing only) |

Serial Monitor runs at **115200 baud**. It prints a GPS status line every 5 seconds.

## Troubleshooting

| Problem | What to try |
|---|---|
| Touch does not respond | Run calibration again with **[CAL]**. Check the serial output for raw x, y and z values |
| `SD CARD MISSING` | Format the card as FAT32, reseat it, check SD wiring. The program retries every 10 seconds |
| `GPS: NO DATA` in serial | Check module TX to GPIO8, 3.3 V, GND, and 9600 baud. Set `GPS_DEBUG_RAW` to 1 to see sentences |
| GPS has data but no fix | Normal indoors. Move outdoors with clear sky view and wait 2 to 5 minutes. Check the antenna is connected |
| `ANTENNA OPEN` in raw NMEA | The module reports no antenna. Connect or replace the antenna |
| Random PSRAM error | Set Tools → PSRAM to Disabled |
| No serial output | Set Tools → USB CDC On Boot to Enabled |

## Legal and ethical use

Wardriving can be legal or illegal depending on where you are and what you do with the data. Only record networks where you are allowed to, and treat logged data as sensitive. Network names and locations can reveal private information.

## Credits

Pin mapping and display configuration are based on the NM-CYD-C5 `sdkconfig`. WiGLE CSV format per [wigle.net](https://wigle.net).

## Author

Developed by **Anders Isaksson (SA7BNB)**.

## License

Released under the MIT License. You are free to use and copy the software. See [LICENSE](LICENSE).
