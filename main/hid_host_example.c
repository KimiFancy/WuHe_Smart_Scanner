/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * This file previously hosted the USB HID Host demo (keyboard/mouse report
 * processing). It has been fully migrated to CDC-ACM mode for the Newland
 * NLS-FM430-EX barcode scanner. All HID-specific code (keycode tables,
 * report callbacks, HID driver install/uninstall, the HID event queue) has
 * been removed. The remaining responsibilities are:
 *
 *   1. USB Host Library lifecycle (usb_lib_task) — the CDC-ACM driver
 *      registers as a client of this library.
 *   2. BOOT button GPIO wiring (available for future graceful shutdown).
 *   3. Application init sequencing: LCD/LVGL → LittleFS → WiFi → cloud →
 *      USB Host Library → CDC scanner.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "errno.h"
#include "driver/gpio.h"

#include "screen_display.h"
#include "wuhe_cloud.h"
#include "wuhe_backup.h"
#include "internet_mgr.h"
#include "wuhe_cdc_scan.h"

/* GPIO Pin number for quit from example logic */
#define APP_QUIT_PIN                GPIO_NUM_0

static const char *TAG = "example";

/**
 * @brief Start USB Host install and handle common USB host library events.
 *
 * Installs the USB Host Library, notifies the calling task (app_main) so it
 * can proceed to start CDC clients, then runs the event-handling loop for the
 * lifetime of the application. The CDC-ACM driver (started via
 * wuhe_cdc_start()) registers as a USB Host client and relies on this loop
 * to process all USB transfers and device events.
 *
 * @param[in] arg  Handle of the task to notify once usb_host_install() returns OK.
 */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // Once the last client deregisters, free all devices and exit.
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

/**
 * @brief BOOT button pressed callback (GPIO ISR).
 *
 * The BOOT button (GPIO0) is wired with a falling-edge ISR for potential
 * graceful-shutdown use. Currently a no-op stub — the CDC scanner, cloud, and
 * LVGL tasks run indefinitely. Add shutdown signaling here if needed.
 */
static void gpio_isr_cb(void *arg)
{
    (void)arg;
}

void app_main(void)
{
    BaseType_t task_created;
    ESP_LOGI(TAG, "WuHeYun CDC scanner example");

    ESP_LOGI(TAG, "Initialize LCD and LVGL UI");
    display_start_up_pagevoid(NULL);

    /* Mount LittleFS BEFORE WiFi/lwip init: littlefs must finish its VFS
     * registration + index-file open before lwip claims the socket FD range
     * [LWIP_SOCKET_OFFSET, MAX_FDS), or an FD-table race breaks socket fcntl. */
    wuhe_backup_init();

    ESP_LOGI(TAG, "Start WiFi + cloud tasks");
    wifi_start_task();      /* async: does not block app_main */
    wuhe_cloud_start();     /* creates cloud queue + task */

    // Init BOOT button: configured for future graceful-shutdown use.
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_isr_cb, NULL));

    /*
    * Create usb_lib_task to:
    * - initialize USB Host library
    * - handle USB Host events (the CDC-ACM driver is a client of this library)
    */
    task_created = xTaskCreatePinnedToCore(usb_lib_task,
                                           "usb_events",
                                           4096,
                                           xTaskGetCurrentTaskHandle(),
                                           2, NULL, 0);
    assert(task_created == pdTRUE);

    // Wait for notification from usb_lib_task to proceed
    ulTaskNotifyTake(false, 1000);

    // Start CDC-ACM scanner task (replaces the former HID Host driver).
    // The CDC task installs the CDC driver, opens the scanner by VID/PID,
    // and dispatches barcodes to the LVGL UI and cloud upload queue.
    wuhe_cdc_start();

    // app_main returns — the CDC, cloud, WiFi, and LVGL tasks run indefinitely.
    ESP_LOGI(TAG, "All subsystems started");
}
