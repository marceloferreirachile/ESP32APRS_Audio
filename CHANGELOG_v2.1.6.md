# AR-APRS v2.1.6 — development log

Branch `dev/2.1.6` (private repo `ESP32APRS_Audio-src`), created from tag `v2.1.5-lu6jmf`.
Status: **in development — not installed on LU6JMF-10 yet.**

Rule: `v2.1.5` is frozen and identical to the published release. All new audio work goes here.

## Changes so far

### Setup WiFi AP is open by default
- **Before:** on a new board or after Factory Reset, the firmware's own default for the setup AP password was the upstream `aprsthnetwork`.
  - The 2.1.4/2.1.5 browser installer was not affected, because it ships a generic `default.cfg` with an empty AP password.
  - It applied to any install without that file.
- **Now:** the setup AP has no password by default. Access control is the web login (admin/admin by default).
- **Files:**
  - `src/main.cpp`: `defaultConfig()`.
  - `src/config.cpp`: the `WiFiAP_PASS` fallback when the key is missing.
- Existing configurations keep whatever password they already have.

### PHG calculator shows the saved values (IGATE and DIGI pages)
- **Before:** Power/Antenna Gain/Height/Direction always opened as 1 W / 6 dBi / 10 ft / Omni, because those values were hardcoded in the HTML.
  - Only the PHG text is stored in the config, so after any save the page looked as if the values had been lost.
- **Now:** on page load, a small script decodes the saved `PHGphgd` text and fills the calculator fields from it.
  - p: power code, W = p²
  - h: height = 10·2^h ft
  - g: gain in dBi
  - d: direction index
- **Limits of the PHG format:**
  - Gain is an integer, so 2.5 dBi comes back as 3.
  - Watt values that share the same code (10 W and 15 W) come back as the first one in the list.
- **File:** `src/webservice.cpp`, IGATE and DIGI pages.
- The same change is already in AR-LORA (`lu6jmf/from-2.1.5`).

### Dashboard: long WiFi SSID no longer breaks the layout
- **Before:** a long network name (e.g. "Wifi ALVAREZ-2.4GHz") stretched the WiFi table and pushed the side column out of place.
- **Now:** the SSID cell wraps inside the table.
- **File:** `src/webservice.cpp`, dashboard WiFi table.
- Same change in AR-LORA (`lu6jmf/from-2.1.5`), found by the Wireless Tracker tester.

## Before release (checklist)
- [ ] Set `-DVERSION="2.1.6"` (now `2.1.6-dev`) and update `version.json` (still describes 2.1.5).
- [ ] Build `pio run -e esp32-nodisp`, install on LU6JMF-10, and validate:
  - [ ] PHG fields come back with the saved values.
  - [ ] After a Factory Reset on a spare board, the setup AP comes up open.
- [ ] Release notes in English with the GPL v3 written offer.
- [ ] Installer littlefs **with** the generic `data/default.cfg` (NOCALL, open AP, admin/admin). Never remove it.
- [ ] Flash usage: esp32-nodisp was at 94.7% in 2.1.5. Check what can be trimmed before adding features.
