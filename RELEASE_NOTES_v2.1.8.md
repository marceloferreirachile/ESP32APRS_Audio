# AR-APRS v2.1.8-lu6jmf

Safe one-click updates, project logo and messaging improvements for the ESP32 APRS audio digipeater / iGate (esp32-nodisp).
Upgrading from 2.1.7 keeps your configuration.

## New
- **Safe update from the DashBoard:** the board checks for new versions once a day and shows a green notice "vX → vY". One button installs it: the board restarts, downloads and checks the new version, and restarts again (about 3 minutes). Your configuration is kept. If anything fails, nothing changes and the board keeps running the current version (the reason is shown).
- **Station logo:** the AR-APRS project mark is the default logo in the page header. Click it to use your own image (any size, resized automatically). Your logo stays after firmware updates.
- **Header:** callsign, active modes and frequency on the right, next to [LOG OUT].
- **Reply button** for received messages (MSG page): fills in the callsign, you only type the text.
- **Browser tab icon** with the project mark.

## Fixes
- Message numbers no longer restart at 1 after a reboot (the other station could drop a new message as a repeat).
- INET→RF uses the standard third-party format (no APRS-IS path or q constructs on RF), and stations without SSID are listed and gated too.
- Manual Firmware Update shows the real reason when it fails, instead of "Update DONE".
- Long digipeater paths in LAST HEARD no longer stretch the page.
- AUDIO column colours follow the VU meter bands.
- About page: a section could be missing on low memory.

## Install
- **New board (USB):** https://marceloferreirachile.github.io/ESP32APRS_Audio/
- **Existing board (keeps configuration):** About → Manual Firmware Update → `ESP32_NODISP_v2.1.8-lu6jmf.bin`, or the "Update" button of the USB installer. Tip: restart the board first and send the file right after it comes back. Web files did not change. Do not flash `littlefs.bin` on a configured board: it erases the configuration.

## Source code
GNU GPL v3, based on ESP32APRS by Somkiat Nakhonthai (HS5TQA). Source of this version: tag [v2.1.8-lu6jmf](https://github.com/marceloferreirachile/ESP32APRS_Audio/tree/v2.1.8-lu6jmf).

73 de LU6JMF
