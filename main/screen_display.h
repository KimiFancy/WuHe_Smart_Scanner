#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

/* Initialize LCD panel + LVGL, build WuHeYun UI, start LVGL task.
 * The disp argument is ignored (kept for signature compatibility); pass NULL. */
void display_start_up_pagevoid(lv_display_t *disp);

/* Thread-safe wrapper: updates the barcode label from any task.
 * Acquires the LVGL lock so it is safe to call from the HID driver task. */
void wuhe_ui_set_barcode_safe(const char *barcode_str);

void wuhe_ui_set_warehouse_safe(const char *warehouse_str);
void wuhe_ui_set_post_safe(const char *post_str);

void wifi_signal_set_rssi_safe(int rssi);

void wuhe_ui_set_workid_safe(const char *workid_str);
void wuhe_ui_set_count_safe(const char *count_str);
