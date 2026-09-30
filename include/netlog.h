#ifndef NETLOG_H
#define NETLOG_H

/*
 * v2.1.3.1-lu6jmf: rotating on-flash diagnostic log.
 *
 * Purpose: let the RFTX/TXDIAG diagnostics added for the 2026-09 field test
 * survive a run on 13.8V-only power (no USB connected, so Serial.printf is
 * invisible) by also writing them to LittleFS, retrievable afterwards via
 * the web UI's File tab or a direct HTTP GET (e.g. http://<ip>/netlog0.txt).
 *
 * Budget: the min_spiffs.csv LittleFS partition on this board is only 128KB
 * total (confirmed from the actual build: littlefs.bin = 131072 bytes), and
 * icons.dat alone (~100KB) + default.cfg (~6.5KB) already use ~107KB of
 * that - leaving only ~20KB free before LittleFS's own block/metadata
 * overhead, not the ~60KB originally assumed. So this budget is deliberately
 * small: 2 files x 6KB = 12KB total, comfortably inside the real free space
 * WITH icons.dat present - no need to remove icons.dat for this at all.
 *
 * Rotation is whole-file only (never mid-line): once the current file
 * reaches NETLOG_FILE_MAX_SIZE, writing moves to the next file index
 * (wrapping around), clearing that file first. This keeps every file
 * internally consistent - downloading all present files always gives a
 * clean (if bounded) history, never a truncated line.
 *
 * Log content restarts fresh (files cleared) on every boot - this is a
 * diagnostic aid for a single test run, not a persistent record across
 * power cycles.
 */

#define NETLOG_FILE_COUNT 2
#define NETLOG_FILE_MAX_SIZE (6 * 1024) // 6KB per file, 12KB total budget

/**
 * @brief Clear any previous netlog files and start rotation fresh at file 0.
 * @warning Call once, after LITTLEFS.begin() succeeds in setup().
 */
void NetLogInit(void);

/**
 * @brief Append one line (no trailing newline needed) to the current netlog
 * file, rotating to the next file first if the current one is full.
 * @warning Safe to call before NetLogInit() (no-op until initialized).
 */
void NetLogWrite(const char *line);

#endif /* NETLOG_H */
