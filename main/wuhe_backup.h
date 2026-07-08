#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file wuhe_backup.h
 * @brief Offline FIFO backup of scanned barcodes on the LittleFS data partition.
 *
 * Power-fail-safe: every entry is a separate file on LittleFS, the on-disk index
 * (head/tail/count) is written AFTER the data file, so a crash between the two
 * leaves at worst an orphan entry file (never a corrupted index pointing at
 * missing data that would crash pop).
 *
 * When the device is offline, the cloud task pushes scans here; on reconnect it
 * drains them FIFO before sending fresh scans (drain-on-reconnect policy).
 *
 * All functions are thread-unsafe by design — they are only ever invoked from
 * the cloud task (see plan § "Must NOT do": never called from HID task).
 */

/**
 * Mount the LittleFS backup partition via VFS.
 *
 * Tries to register at @ref WUHE_BACKUP_MOUNT; on failure attempts format +
 * remount. On hard failure the module degrades to read-only mode (subsequent
 * push/pop/count all return false / 0) but the device keeps scanning locally.
 *
 * @return true on successful mount, false on hard failure (logged).
 */
bool wuhe_backup_init(void);

/**
 * Append one (code, sid) pair to the FIFO.
 *
 * If the FIFO is full (count == WUHE_BACKUP_FILE_MAX) the oldest entry is
 * overwritten (head advances) and a "dropped" counter is logged.
 *
 * @param code  NUL-terminated barcode string (≤ 120 bytes including multibyte
 *              UTF-8 sequences; see WUHE_BARCODE_MAX_BYTES in scanner_config.h).
 * @param sid   Scan ID captured at scan time (1..65535).
 * @return true on success, false on write error or unmounted module.
 */
bool wuhe_backup_push(const char *code, uint16_t sid);

/**
 * Pop the oldest entry from the FIFO and delete its backing file.
 *
 * @param code_out  Destination buffer for the barcode (NUL-terminated).
 * @param cap       Capacity of @p code_out (incl. NUL).
 * @param sid_out   Receives the SID; may be NULL.
 * @return true on success, false if FIFO empty, module unmounted, or read error.
 */
bool wuhe_backup_pop(char *code_out, size_t cap, uint16_t *sid_out);

/**
 * @return Current FIFO entry count (0 if index unreadable / unmounted).
 */
size_t wuhe_backup_count(void);

#ifdef __cplusplus
}
#endif
