# Salah E-Ink

Prayer-time desk display on an ESP32-C3 Mini and a Waveshare 1.54" 3-colour (black/red/white) e-ink. It shows the start and end time of all five prayers, with a banner for when the current prayer ends.

## Features
- Live times from the [Aladhan API](https://aladhan.com/prayer-times-api) for any location
- Web dashboard with a draggable map, UTC offset, calculation method and Asr school (Standard/Hanafi)
- Banner with the exact end time of the current prayer
- Jumu'ah highlight on Fridays
- Forbidden-time banner for nafl prayer (after sunrise, at zawal, before sunset)
- Ramadan mode: day counter plus Suhoor and Iftar times
- Last third of the night
- Deep sleep: wakes only at prayer boundaries and skips Wi-Fi unless a new day needs fetching

## Hardware
- ESP32-C3 Mini
- Waveshare 1.54" e-Paper (B), 200x200, `GxEPD2_154_Z90c`

Pins are set in `include/config.h` (`EPD_SCK`, `EPD_MOSI`, `EPD_CS`, `EPD_DC`, `EPD_RST`, `EPD_BUSY`). Match them to your wiring.

## Setup
1. Install [PlatformIO](https://platformio.org/).
2. Upload with `pio run -t upload`.
3. With no saved Wi-Fi, or if the network is unreachable at power-up, the device starts the `Salah-Setup` access point (password `salah1234`, set in `include/config.h`; captive portal at `192.168.4.1`, 10 minutes). The screen shows the name and password. Join it, enter your Wi-Fi name and password, and press Save. The map needs internet, so enter coordinates by hand there.
4. Once connected, the dashboard is at `http://salah.local` (or the IP shown on the screen) for 5 minutes after each reset. Pick your location and settings, then press Save.

Settings and Wi-Fi credentials are stored on the device.

## Layout
```
src/main.cpp      firmware
include/config.h  pin settings
include/page.h    dashboard HTML
platformio.ini    build config
case/             3D-printable case (CadQuery script and STLs)
```

## Notes
- Forbidden-time windows are set by `FB_RISE`, `FB_ZAWAL` and `FB_SET` in `main.cpp`. Check them against your local scholars.
- Hijri dates come from Aladhan and may differ by a day from local moon sighting.
