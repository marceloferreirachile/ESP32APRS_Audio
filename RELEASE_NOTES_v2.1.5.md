# AR-APRS v2.1.5-lu6jmf

Stability and diagnostics release for the ESP32 APRS audio digipeater / iGate (esp32-nodisp).
Upgrading from 2.1.4 keeps your configuration.

## Fixes

- **INET2RF filters reset to zero after saving a page.** Disabled fields were not sent by the browser, so saving a page wiped the iGate filters and other settings. No field is disabled anymore: everything stays editable, and you can prepare a configuration before turning a function on.
- **PTT stuck / TX stall.** The root cause is fixed: the audio timers are no longer paused or restarted while a TX is in progress. As an extra safety net, a stuck PTT is released after 10 s.
- **Clock not set after boot.** If the first NTP attempt failed, the next try came 24 h later. NTP now retries every minute until the first sync. The default server is `pool.ntp.org`.
- **Telemetry.** TLM is sent once (it was doubled). TLM headers are staggered and retried if the TX queue is busy. Objects wait until the clock is synced.
- **Messages.** ACK handling and callsign length are hardened. Query replies (`?VER`, `?IGATE`, `?DX`…) now use the normal retry policy.
- **About page.** Fixed a text overflow with longer version strings.

## New

- **Black box in `/txdebug`.** After any restart, a **LAST RESTART** banner shows:
  - the reason (software, watchdog, power…),
  - when the previous run ended (local time),
  - how long it ran.
- **Health cards in `/txdebug`:**
  - **SYSTEM HEALTH:** heap and task stacks.
  - **HEAP HISTORY:** 24 hourly samples with a trend line.
  - **WATCHDOG RECOVERIES:** state, stack and heap.
  - **Boot time:** when the current run started.
- **TX audio calibration:**
  - The tone test pauses the TX scheduler while it plays; queued packets go out right after it ends.
  - The test is refused while the radio is transmitting.
  - No reboot is needed.
  - The measuring method is documented in `doc/TX_AUDIO_CALIBRATION.md`.
- **Status text per function.** iGate, Digi and Tracker each get their own default status: `AR-APRS iGate/Digi/Tracker - Argentina APRS System`.

## Under the hood

- Large web pages are sent in chunks, which uses much less heap.
- The task watchdog is fed in all web handlers.
- `taskAPRS` stack raised to 8192.

## Install

**New device / full reset:** use the one-click browser installer: https://marceloferreirachile.github.io/ESP32APRS_Audio/

**Existing device (no USB needed, configuration is preserved):**
1. About tab -> Manual Firmware Update -> upload `ESP32_NODISP_v2.1.5-lu6jmf.bin`. The device reboots.
2. File tab -> upload, one at a time: `icons.dat`, `main_shell.html`, `style.css`.

Do not flash `littlefs.bin` on an existing device: it is the full filesystem image and would erase your configuration. It is only used by the browser installer.

## Source code (GPL v3)

This firmware is a modified version of ESP32APRS_Audio by nakhonthai and is distributed under the GNU GPL v3.
The complete corresponding source code of this release is available on request, free of charge, for at least 3 years from its release date (written offer, GPL v3 section 6(b)).
To request it, write to **marcelobrachile@gmail.com** and include the version, v2.1.5-lu6jmf.

73 de LU6JMF
