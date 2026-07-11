/**
 * scanner_config.h — Central configuration for the USB barcode scanner.
 *
 * Single source of truth for:
 *   - Scanner USB VID/PID (used by CDC-ACM open path)
 *   - Maximum barcode byte width (drives buffer sizing in wuhe_cloud.c,
 *     wuhe_backup.c, and the CDC RX layer)
 *
 * The barcode width is sized for 40 CJK characters in UTF-8
 * (3 bytes per CJK char + 1 NUL terminator = 121 bytes).
 * ASCII-only barcodes of up to 120 chars also fit.
 */
#pragma once

#include <stdint.h>

/* ---- USB device identity ----------------------------------------- */

/** CX70 barcode scanner USB Vendor ID. */
#define SCANNER_VID   0x0218u
/** CX70 barcode scanner USB Product ID (CDC-ACM mode). */
#define SCANNER_PID   0x0212u

/* ---- Barcode buffer sizing --------------------------------------- */

/**
 * Maximum barcode length in bytes, INCLUDING the NUL terminator.
 * 40 CJK chars (3 bytes/char in UTF-8) + 1 NUL = 121 bytes.
 *
 * This macro is the SINGLE source of truth — wuhe_cloud.c (scan_item_t.code,
 * dedup_code), wuhe_backup.c (WUHE_BACKUP_CODE_FIELD_LEN), and the CDC RX
 * accumulator buffer all reference it. Bump here to widen every layer.
 */
#define WUHE_BARCODE_MAX_BYTES   121u
