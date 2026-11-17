# AR-APRS v2.1.7-lu6jmf

Official APRS device identifier and iGate improvements for the ESP32 APRS audio digipeater / iGate (esp32-nodisp).
Upgrading from 2.1.6 keeps your configuration.

## New
- **Own TOCALL `APJMF1`:** AR-APRS is now registered in the APRS device identification list, so maps and tools show the station as AR-APRS.
- **iGate** follows the aprsc iGate implementation hints: packets go to APRS-IS unmodified (cut only at the end of the line), RF packets are not filtered as duplicates (APRS-IS does that) and nothing is sent late after a lost APRS-IS connection.
- **Query replies** (`?VER`, `?APRSP`...) are sent twice on RF, about 10 s apart, because a reply without a message number has no ACK.

## Fixes
- NEWS bulletins: the schedule preview now shows the real send times.
- NEWS intervals saved by older versions are converted to minutes.

## Install
- **New board (USB):** https://marceloferreirachile.github.io/ESP32APRS_Audio/
- **Existing board (keeps configuration):** About → Manual Firmware Update → `ESP32_NODISP_v2.1.7-lu6jmf.bin`, or the "Update" button of the USB installer. Web files did not change. Do not flash `littlefs.bin` on a configured board: it erases the configuration.

## Source code
GNU GPL v3, based on ESP32APRS by Somkiat Nakhonthai (HS5TQA). Source of this version: tag [v2.1.7-lu6jmf](https://github.com/marceloferreirachile/ESP32APRS_Audio/tree/v2.1.7-lu6jmf).

73 de LU6JMF
