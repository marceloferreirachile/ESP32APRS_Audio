#include <Arduino.h>
#include <LITTLEFS.h>
#include "netlog.h"

// v2.1.3.1-lu6jmf: see include/netlog.h for the full design rationale.
//
// Keeps ONE File handle open across writes instead of open()/close() on
// every single line - LITTLEFS.open() does a directory lookup that is much
// more expensive than the write itself.
//
// v2.1.3.1-lu6jmf (2026-09, root-caused via real crash + ESP-IDF/ESPAsyncWebServer
// issue reports): flush() on EVERY line was itself the bug, not the fix. On
// ESP32, a flash write disables the flash cache for its duration - nothing
// running from flash (i.e. almost the whole firmware, including the
// async_tcp task that runs the web server) can execute meanwhile. Calling
// flush() (a real synchronous flash write) on every single RFTX/TXDIAG line
// - i.e. on every packet taskAPRS handles - was stalling the whole chip
// repeatedly, which is consistent with the "web UI sluggish, reboots on
// navigate" symptom actually reported (async_tcp missing its watchdog
// deadline while stalled -> panic/reboot). See:
// https://github.com/me-no-dev/ESPAsyncWebServer/issues/686 and /772.
// Fix: print() (RAM-buffered, cheap) on every line as before, but flush()
// (the expensive part) only periodically - still bounds how much can be
// lost if power is cut, without paying the flash-cache-disabled stall on
// every single packet.
#define NETLOG_FLUSH_INTERVAL_MS 3000

static File netlogFile;
static uint8_t netlogCurFile = 0;
static size_t netlogCurSize = 0;
static bool netlogReady = false;
static unsigned long netlogLastFlush = 0;

static void netlogPath(uint8_t idx, char *out, size_t outSize)
{
    snprintf(out, outSize, "/netlog%u.txt", idx);
}

// Opens (truncating) the given rotation slot as the new active file for writing.
static void netlogOpenFile(uint8_t idx)
{
    if (netlogFile)
        netlogFile.close();
    char path[24];
    netlogPath(idx, path, sizeof(path));
    netlogFile = LITTLEFS.open(path, "w"); // "w" truncates - clears old contents
    netlogCurFile = idx;
    netlogCurSize = 0;
}

void NetLogInit(void)
{
    char path[24];
    for (uint8_t i = 0; i < NETLOG_FILE_COUNT; i++)
    {
        netlogPath(i, path, sizeof(path));
        if (LITTLEFS.exists(path))
            LITTLEFS.remove(path);
    }
    netlogOpenFile(0);
    netlogReady = (bool)netlogFile;
}

void NetLogWrite(const char *line)
{
    if (!netlogReady || line == NULL)
        return;

    size_t lineLen = strlen(line);

    // Rotate to the next file (whole-file only, never mid-line) if this
    // line would overflow the current file's budget.
    if (netlogCurSize + lineLen + 1 > NETLOG_FILE_MAX_SIZE)
        netlogOpenFile((netlogCurFile + 1) % NETLOG_FILE_COUNT);

    if (!netlogFile)
        return;

    netlogFile.print(line);
    netlogFile.print("\n");
    // v2.1.3.1-lu6jmf: flush() only every NETLOG_FLUSH_INTERVAL_MS, not on
    // every line - see comment at the top of this file. print() alone just
    // appends to an in-RAM buffer (cheap); flush() is the real flash write
    // (expensive, stalls the whole chip while the flash cache is disabled).
    unsigned long nowMs = millis();
    if (nowMs - netlogLastFlush >= NETLOG_FLUSH_INTERVAL_MS)
    {
        netlogFile.flush();
        netlogLastFlush = nowMs;
    }
    netlogCurSize += lineLen + 1;
}
