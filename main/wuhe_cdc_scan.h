#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file wuhe_cdc_scan.h
 * @brief CDC-ACM Host RX layer for the Newland NLS-FM430-EX barcode scanner.
 *
 * Replaces the former HID-keyboard-mode scanning path with a CDC-ACM (virtual
 * serial) byte pipe. The scanner emits UTF-8 directly — no GBK→UTF-8 transcode
 * is needed, and every byte (including 0x80-0xBF UTF-8 continuation bytes)
 * flows through untouched.
 *
 * wuhe_cdc_start() spawns a FreeRTOS task that:
 *   1. Installs the CDC-ACM host driver (the USB Host Library must already be
 *      running — see usb_lib_task in hid_host_example.c).
 *   2. Opens the scanner by VID/PID (scanner_config.h), retrying until the
 *      device appears.
 *   3. Lets the driver's data_cb accumulate raw bytes into a barcode buffer
 *      until an ENTER terminator (\r or \n) is seen, then dispatches the
 *      barcode to the LVGL UI and the cloud upload queue.
 *   4. On USB disconnect, closes the device and loops back to step 2.
 */

/**
 * @brief Start the CDC-ACM scanner task (non-blocking).
 *
 * Creates the background task that installs the CDC driver, opens the scanner,
 * and processes incoming barcode data. Safe to call once from app_main after
 * the USB Host Library task has been started.
 */
void wuhe_cdc_start(void);

#ifdef __cplusplus
}
#endif
