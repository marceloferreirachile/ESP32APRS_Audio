# AR-APRS v2.1.6-lu6jmf

APRS 1.2 behaviour, more APRS queries, our own VPN server and stability fixes for the ESP32 APRS audio digipeater / iGate (esp32-nodisp).
Upgrading from 2.1.5 keeps your configuration.

## New
- **APRS 1.2:** the digipeater inserts its own callsign on WIDEn-N, ignores repeated copies for 30 s and never repeats its own packets. Query replies follow the spec (no message number); reply-ack supported; general queries `?APRS?` and `?IGATE?` heard on RF are answered.
- **APRS queries** (message to the station): `?APRS` (list), `?VER`, `?APRSP` (position now), `?APRSS` (status), `?APRST` / `?PING?` (path), `?APRSD` (stations heard direct), `?APRSO` (objects again), `?IGATE`, `?DX`.
- **Messages without a message number** are shown in the chat and answered.
- **VPN:** new server `vpn.tracktotal.ar`. The private key is generated on the board and never leaves it. Saving the VPN page restarts the board so on/off takes effect at once.
- **Header** shows the Host Name, enabled modes and the radio frequency.
- **`/txdebug`:** LAST CRASH card (where the last crash happened).

## Fixes
- Reboot with I2C sensors (BME280 and others) after a WiFi reconnect.
- Crash when building a long AX.25 frame.
- A digipeated packet could cancel another packet waiting in the TX queue.
- A message repeated by the sender gets the ACK again but is not answered twice.
- PHG calculator shows the saved values.

## Install
- **New board (USB):** https://marceloferreirachile.github.io/ESP32APRS_Audio/
- **Existing board (keeps configuration):** About → Manual Firmware Update → `ESP32_NODISP_v2.1.6-lu6jmf.bin`. Do not flash `littlefs.bin` on a configured board: it erases the configuration.

## Source code
GNU GPL v3, based on ESP32APRS by Somkiat Nakhonthai (HS5TQA). Source of this version: tag [v2.1.6-lu6jmf](https://github.com/marceloferreirachile/ESP32APRS_Audio/tree/v2.1.6-lu6jmf).

73 de LU6JMF
