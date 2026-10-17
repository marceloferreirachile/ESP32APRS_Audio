# AR-APRS v2.1.6 — development log

Branch `dev/2.1.6` (private repo `ESP32APRS_Audio-src`), created from tag `v2.1.5-lu6jmf`.
Status: **in development — installed on LU6JMF-10 for testing (2026-10-15).**

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

### APRS queries: 6 new ones, and ?VER says AR-APRS
- **Before:** the station answered only ?APRSV / ?VER / ?ABOUT, ?IGATE and ?DX. The ?VER reply started with "ESP32APRS_CDU".
- **Now** it also answers:
  - `?APRS?`: list of accepted queries. `?APRS` gives the same list (ready-made query text on Yaesu radios; not in the spec).
  - `?APRSP`: sends the position now (iGate, Digi and Tracker, whatever is enabled).
  - `?APRSD`: "Directs=" stations heard direct on RF (no digi) in the last hour.
  - `?APRSS`: status (IGate / APRS-IS / Digi / Tracker, uptime).
  - `?APRST` / `?PING?`: the path the query came through.
  - `?APRSO`: re-sends the enabled objects, "N Objects Transmitted". Does not count toward the object send limit.
- The ?VER reply now starts with "AR-APRS".
- **Files:** `src/message.cpp` (`handleAPRSQuery` now also receives the full TNC2 line), `include/message.h`, `src/main.cpp` (`lu6jmfQueryBeacon` flag handled in `taskAPRS`).
- Built OK with `esp32-nodisp`: Flash 94.8%.
- Tested 2026-10-15 on LU6JMF-10 from the FTM-510 (build Oct 15 22:00:40): `?APRSP`, `?ver`, `?aprsd`, `?aprss` answered OK.
- [ ] Still to test: `?APRS?` / `?APRS`, `?APRST` / `?PING?`, `?APRSO`.

### Messages without a message number are no longer dropped
- **Before:** a message to the station without "{nn" was ignored: not in the chat, no query answer.
  - Seen on 2026-10-15 in aprs.fi raw packets: the FTM-510 (LU6JMF-1, APY510) sent `?APRSP`, `?dx`, `?ver`, `?aprs` without a number; only `abc{02` reached the chat.
- **Now:** it shows in the chat and queries are answered. No ACK is sent (nothing to ACK).
  - Not with encryption on (the IV needs the number). AT commands stay numbered-only.
  - Each unnumbered message takes its own chat slot (before, all would share msgID 0 and overwrite each other).
- **File:** `src/message.cpp` (`handleIncomingAPRS`, `pkgMsgUpdate`).

### Duplicate copies of the same message are ignored
- **Before:** each query from the FTM-510 was answered twice, 1-2 s apart.
  - Cause, seen in aprs.fi raw packets on 2026-10-15: every LU6JMF-1 message entered APRS-IS twice, via LU6JMF-10 (RF) and via KE9DJN-10, an iGate near Indianapolis (USA) that cannot hear it on RF. It re-gates it as `qAR` and changes the TOCALL `APY510` to `APRS`, so APRS-IS does not drop the copy and delivers it back to LU6JMF-10.
- **Now:** same station + same text + same message number (or none) within 30 s is a copy and is ignored. A real retry from the sender comes later and is still handled.
- Not solvable in firmware: YC3BQZ-10 (Indonesia, APZUAG) sends `ack` in LU6JMF-1's name via APRS-IS. A fake ack looks the same as a real one.
- **File:** `src/message.cpp` (`lu6jmfMsgSeenRecently`, called in `handleIncomingAPRS`).
- Tested 2026-10-15 (build 22:00:40): `?aprss` from the FTM-510 shown once, answered once.

### VPN: our own server (vpn.tracktotal.ar) instead of HS5TQA's
- **Before:** "New Register" on the VPN page asked HS5TQA's server (`http://hs1.hs5tqa.ampr.org:81/wg/create`, unencrypted) and the default server was `vpn.nakhonthai.net`.
- **Now:** it asks `https://vpn.tracktotal.ar/wg/create` (same reply format), default server `vpn.tracktotal.ar`. Our boards get 10.66.1.N and are reachable at `http://vpn.tracktotal.ar:20000+N`, shown on the VPN page.
  - Boards still registered on HS5TQA's server (10.44.x.y) keep working and keep their links until they press New Register again.
- Server side (TrackTotal VPS, not in the firmware): WireGuard `wgaprs` UDP 51820, registration service `vpn_api.py` (one slot per board MAC, max 5 new per internet address per hour), files in Radio Amador/vpn_aprs.
- New Register also sends the station callsign, so the PRIME VPN page shows (and pre-fills the note with) who owns each board.
- New Register now saves the settings and restarts the board by itself (no Apply Change / reboot by hand). If saving fails, the page says so with the error code.
- On boot, a board with HS5TQA's VPN settings and the VPN **off** gets them cleared and the server set to `vpn.tracktotal.ar` (so an OTA update leaves the VPN page ready for New Register). A board with HS5TQA's VPN **on** is left as it is.
- **Files:** `src/webservice.cpp` (VPN page), `src/main.cpp` (default server).
- Tested 2026-10-16 on LU6JMF-10 (build 20:15): New Register saved and restarted by itself, board online at 10.66.1.2, opened from outside at http://vpn.tracktotal.ar:20002 (also from Buenos Aires), shown online in PRIME > VPN.

### Icons and favicon come from the board, not from HS5TQA's server
- **Before:** LAST HEARD, the symbol pickers (IGATE, DIGI, OBJ) and the browser tab icon loaded images from `http://aprs.nakhonthai.net`. When that server answered, the pages showed its icons instead of the aprs.fi-style ones packed on the board; when it was down, the pickers showed no icon.
- **Now:** all icons come from the board (`/symbols/icons/...`, packed in `data/symbols/icons.dat`). The tab icon is the digi symbol from the same pack.
- LAST HEARD loads its icons from our server `https://vpn.tracktotal.ar/symbols/icons/<code>-<1|2>.png` (same names as before): the aprs.fi symbols (github.com/hessu/aprs-symbols) cut at 48 px, sharp at 20 px. Without internet it falls back to the board icons (12 px, to fit LittleFS), then to a drawn shape. Server side: `vpn_aprs/install_icons.sh`. Symbol pickers use the board icons.
- Boards that got `main_shell.html` in LittleFS from an older installer: the firmware replaces those addresses when it loads the template, so OTA alone is enough (no filesystem upload).
- **Files:** `src/webservice.cpp`, `data/templates/main_shell.html` (for new installs).

### Fewer dependencies on outside servers
- Factory default APRS-IS server: `rotate.aprs2.net` (was `aprs.nakhonthai.net`), same as the installer `default.cfg`. Only matters after a Factory Reset without that file.
- Factory default MQTT server: empty (was `mqtt.nakhonthai.net`).
- IGATE page help next to Server Host/Port: suggests rotate.aprs2.net / 14580 (was links to HS5TQA's servers).
- TNC2 and GNSS pages load jQuery from the board (`/jquery-3.7.1.js`) instead of `apps.bdimg.com`. The gauge charts still load Highcharts from `code.highcharts.com`.
- Kept on purpose: credits to HS5TQA (About, file headers), and the links for boards still on the old VPN (10.44.x.y).
- **Files:** `src/main.cpp`, `src/webservice.cpp`.

### Header shows which board you are on
- Left side of the page header: the Host Name from the System page, like the original firmware showed on top (while it is still the factory name: the iGate callsign + SSID, or "NOCALL - not configured"), the enabled modes (iGate / Digi / Tracker / WX) and, when the built-in radio module is enabled, its frequency. Asked by the Buenos Aires tester, who manages more than one board.
- Boards with an external radio (rf off) show no frequency: the firmware does not know it.
- **File:** `src/webservice.cpp` (`lu6jmfHeaderInfo`, both header paths).

## Before release (checklist)
- [ ] **Release waits for the TOCALL** (aprsorg/aprs-deviceid issue #378, APJMF1 = AR-APRS, APJMF2 = AR-LORA). After approval: replace `APE32A` / `APE32L` / `APE32I` with `APJMF1` everywhere the firmware builds a packet, rebuild, test on LU6JMF-10 that aprs.fi shows APJMF1, then release (decided 2026-10-16: ship 2.1.6 with everything new, TOCALL included).
- [ ] Still to test: `?aprs`, `?aprst`, `?aprso`; PHG fields; open setup AP after Factory Reset.
- [ ] Set `-DVERSION="2.1.6"` (now `2.1.6-dev`) and update `version.json` (still describes 2.1.5).
- [ ] Build `pio run -e esp32-nodisp`, install on LU6JMF-10, and validate:
  - [ ] PHG fields come back with the saved values.
  - [ ] After a Factory Reset on a spare board, the setup AP comes up open.
- [ ] Release notes in English with the GPL v3 written offer.
- [ ] Installer littlefs **with** the generic `data/default.cfg` (NOCALL, open AP, admin/admin, VPN server vpn.tracktotal.ar). Never remove it.
- [ ] Flash usage: esp32-nodisp was at 94.7% in 2.1.5, 94.9% now (2026-10-16). Check what can be trimmed before adding features.
