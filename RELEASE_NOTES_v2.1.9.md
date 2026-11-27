# AR-APRS v2.1.9-lu6jmf

One firmware for every classic ESP32 board (no screen, 0.96" or 1.3" OLED), SOS emergency and many fixes.
Upgrading from 2.1.8 keeps your configuration.

## New
- **One firmware for all ESP32 boards:** no screen, 0.96" (SSD1306) or 1.3" (SH1106). The screen is found automatically at start-up; you can also choose it on System → Display Setting. Add, change or remove a screen later and keep the same firmware.
- **Screens change by themselves** every 10 s (Display Setting → Auto Change). A short press on the BOOT button keeps the current screen for 5 minutes; another press goes on. Hold 2 s on the GPS screen: GPS/compass; on the statistics screen: send a beacon now.
- **SOS Emergency** (Tracker enabled): DashBoard button, or an APRS message `SOS` / `SOS OFF` from your own callsign (any SSID) or from the SOS contact. Sends the APRS emergency position every minute until cancelled.
- **Flip Rotate** turns the screen 180°. **Menu Button GPIO**: choose another pin or none (-1).
- Boot screen with the AR-APRS name.

## Fixes
- **Menu button did not work:** setting up the PTT left GPIO0 (BOOT button) as an output held low. Fixed.
- "Check version" (About) could restart the board (watchdog). Fixed.
- Pages are built with much less memory (a section of the System page could disappear).
- VPN: stays reachable from the Internet (keepalive); the board link opens correctly.
- Manual update refuses a file made for another board.
- Holding the button no longer switches the WiFi off or powers the board off (a station could go off by accident).
- AT commands and PPP over serial removed. The firmware is about 75 KB smaller than 2.1.8, even with screen support.

## Install
- **Board on 2.1.8:** DashBoard → green notice → "Install update now".
- **New board (USB):** https://marceloferreirachile.github.io/ESP32APRS_Audio/
- **Existing board (keeps configuration):** About → Manual Firmware Update → `ESP32_v2.1.9-lu6jmf.bin` (`ESP32_NODISP_v2.1.9-lu6jmf.bin` is the same file), or the "Update" button of the USB installer. Boards with a screen use the same file. Web files did not change. Do not flash `littlefs.bin` on a configured board: it erases the configuration.

## Source code
GNU GPL v3, based on ESP32APRS by Somkiat Nakhonthai (HS5TQA). Source of this version: tag [v2.1.9-lu6jmf](https://github.com/marceloferreirachile/ESP32APRS_Audio/tree/v2.1.9-lu6jmf).

73 de LU6JMF
