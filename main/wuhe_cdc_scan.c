/**
 * @file wuhe_cdc_scan.c
 * @brief CDC-ACM Host RX layer for the barcode scanner (CX70 / FM430-EX).
 *
 * === Design overview =================================================
 *
 * The scanner operates in CDC-ACM (virtual serial port) mode and emits a raw
 * GBK byte stream for every scan, terminated by ENTER (\r, \n, or \r\n).
 *
 * This module is a GBK byte pipe with firmware-side transcoding:
 *   - GBK → UTF-8 transcoding happens in dispatch_barcode() via gbk_to_utf8()
 *     (gbk_utf8.c). LVGL and the cloud JSON layer only ever see UTF-8.
 *   - No byte filtering — GBK lead/trail bytes (0x81–0xFE) pass through the
 *     accumulator untouched. The old HID path filtered `>= 0x20`; that would
 *     drop legitimate GBK trail bytes here.
 *   - The only framing logic is terminator detection: \r or \n ends a barcode.
 *     A \r\n pair produces exactly one dispatch (the second byte sees an empty
 *     accumulator and is a no-op).
 *
 * === Task lifecycle ==================================================
 *
 *   wuhe_cdc_start()
 *     └─ xTaskCreatePinnedToCore(wuhe_cdc_task)
 *          ├─ cdc_acm_host_install()          (once)
 *          └─ loop:
 *               ├─ cdc_acm_host_open(VID, PID)  → success: block on sem
 *               │                               → failure: vTaskDelay, retry
 *               ├─ [data_cb accumulates + dispatches barcodes]
 *               ├─ disconnect (event_cb) → xSemaphoreGive
 *               └─ cdc_acm_host_close() → back to open
 *
 * The data_cb runs in the CDC driver's own background task, so calls are
 * serialized (no re-entrancy). Dispatch to wuhe_ui_set_barcode_safe (acquires
 * LVGL lock internally) and wuhe_cloud_submit_scan (FreeRTOS queue) is
 * thread-safe by design.
 *
 * === Buffer sizing ===================================================
 *
 * s_barcode_buf holds raw GBK bytes (up to WUHE_BARCODE_MAX_BYTES-1 = 120).
 * dispatch_barcode() transcodes to a UTF-8 buffer of the same width; since GBK
 * CJK is 2 bytes and UTF-8 CJK is 3 bytes, at most 40 CJK characters survive
 * the transcode (120 UTF-8 bytes + NUL = 121). On overflow the partial barcode
 * is dispatched (truncated on a character boundary) with a warning and the
 * accumulator resets; we never write past the buffer end.
 */

#include "wuhe_cdc_scan.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/cdc_acm_host.h"
#include "usb/cdc_host_types.h"
#include "usb/usb_host.h"

#include "scanner_config.h"   /* SCANNER_VID, SCANNER_PID, WUHE_BARCODE_MAX_BYTES */
#include "gbk_utf8.h"         /* gbk_to_utf8 */
#include "wuhe_cloud.h"       /* wuhe_cloud_submit_scan */
#include "screen_display.h"   /* wuhe_ui_set_barcode_safe */

static const char *TAG = "wuhe.cdc";

/* ---- RX accumulator (CDC driver task context, single-threaded) ------ */

/** Barcode byte accumulator. Sized for 40 CJK chars in UTF-8 + NUL. */
static uint8_t s_barcode_buf[WUHE_BARCODE_MAX_BYTES];
/** Current write offset into s_barcode_buf. */
static size_t  s_barcode_len = 0;

/* ---- Connection state ---------------------------------------------- */

/** Handle of the currently-open CDC device (NULL when closed). */
static cdc_acm_dev_hdl_t   s_cdc_dev = NULL;
/** Posted by event_cb on disconnect; task blocks on this while connected. */
static SemaphoreHandle_t   s_disconnect_sem = NULL;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief NUL-terminate the accumulator and dispatch the barcode to the
 *        UI + cloud queue, then reset the accumulator.
 *
 * Called only from cdc_data_cb (CDC driver task — single-threaded, no lock
 * needed). A zero-length dispatch (e.g. the \n of a \r\n pair) is a no-op.
 */
static void dispatch_barcode(void)
{
    s_barcode_buf[s_barcode_len] = '\0';

    if (s_barcode_len > 0) {
        /* Scanner emits GBK; LVGL and the cloud JSON layer require UTF-8.
         * Output is capped to WUHE_BARCODE_MAX_BYTES (121), truncating on a
         * character boundary so downstream buffers never split a multibyte
         * sequence. Worst case 80 GBK bytes (40 CJK) -> 120 UTF-8 + NUL. */
        char utf8[WUHE_BARCODE_MAX_BYTES];
        size_t utf8_len = gbk_to_utf8(s_barcode_buf, s_barcode_len,
                                      utf8, sizeof(utf8));

        size_t dump_len = utf8_len < 32 ? utf8_len : 32;
        ESP_LOGI(TAG, "scan received (%zu GBK -> %zu UTF-8 bytes): '%s'",
                 s_barcode_len, utf8_len, utf8);
        ESP_LOG_BUFFER_HEXDUMP(TAG, utf8, dump_len, ESP_LOG_INFO);

        wuhe_ui_set_barcode_safe(utf8);
        wuhe_cloud_submit_scan(utf8);
    }

    s_barcode_len = 0;
}

/* ------------------------------------------------------------------ */
/*  CDC driver callbacks                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief CDC-ACM data RX callback.
 *
 * Accumulates raw GBK bytes into s_barcode_buf. On \r or \n the accumulated
 * barcode is dispatched (dispatch_barcode transcodes GBK→UTF-8 before
 * forwarding). GBK lead/trail bytes flow through untouched (no >= 0x20
 * filter — that would drop legitimate GBK trail bytes 0x40-0xFE).
 *
 * Returns true = data consumed → flush the driver's internal RX buffer.
 */
static bool cdc_data_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    (void)user_arg;
    ESP_LOGI(TAG, "RX %zu bytes", data_len);
    ESP_LOG_BUFFER_HEX(TAG, data, data_len > 32 ? 32 : data_len);

    for (size_t i = 0; i < data_len; i++) {
        uint8_t b = data[i];

        if (b == '\r' || b == '\n') {
            /* ENTER terminator: dispatch whatever we have accumulated. */
            dispatch_barcode();
            continue;
        }

        if (s_barcode_len < WUHE_BARCODE_MAX_BYTES - 1) {
            s_barcode_buf[s_barcode_len++] = b;
        } else {
            /* Buffer full before a terminator: dispatch the truncated
             * barcode, warn, then start the accumulator fresh with the
             * current byte. We never write past the buffer end. */
            ESP_LOGW(TAG, "barcode buffer overflow at %zu bytes, dispatching truncated",
                     s_barcode_len);
            dispatch_barcode();
            s_barcode_buf[s_barcode_len++] = b;
        }
    }

    return true;
}

/**
 * @brief CDC-ACM device event callback.
 *
 * On disconnect we signal the task (via semaphore) so it can close the handle
 * and loop back to cdc_acm_host_open(). Errors are logged; other events
 * (serial-state, suspend/resume) are ignored.
 */
static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;

    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "device disconnected");
        s_cdc_dev = NULL;
        if (s_disconnect_sem) {
            xSemaphoreGive(s_disconnect_sem);
        }
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "serial state update: 0x%04x", event->data.serial_state);
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        break;
    default:
        /* CDC_ACM_HOST_SERIAL_STATE, suspend/resume, network — not used. */
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  CDC scanner task                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Background task: install CDC driver, open scanner, wait for scans,
 *        reconnect on disconnect. Runs for the lifetime of the application.
 */
static void wuhe_cdc_task(void *arg)
{
    (void)arg;

    /* --- One-time CDC-ACM driver install -------------------------------
     * USB Host Library must already be installed (done by usb_lib_task in
     * hid_host_example.c before this task starts). */
    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = 5,
        .xCoreID                = 0,
        .new_dev_cb             = NULL,
    };
    esp_err_t ret = cdc_acm_host_install(&driver_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM driver installed");

    s_disconnect_sem = xSemaphoreCreateBinary();
    if (s_disconnect_sem == NULL) {
        ESP_LOGE(TAG, "failed to create disconnect semaphore");
        vTaskDelete(NULL);
        return;
    }

    /* --- Open / reconnect loop ---------------------------------------- */
    while (true) {
        const cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = 2000,
            .out_buffer_size       = 64,
            .in_buffer_size        = 0,
            .event_cb              = cdc_event_cb,
            .data_cb               = cdc_data_cb,
            .user_arg              = NULL,
        };

        ESP_LOGI(TAG, "opening scanner VID=0x%04X PID=0x%04X ...",
                 SCANNER_VID, SCANNER_PID);

        cdc_acm_dev_hdl_t dev = NULL;
        ret = cdc_acm_host_open(SCANNER_VID, SCANNER_PID, 0,
                                &dev_config, &dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "open failed: %s — retrying in 1 s",
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_cdc_dev = dev;

        /* Dump full USB descriptor for debugging interface/endpoint selection. */
        cdc_acm_host_desc_print(dev);

        /* Post-open setup: scanners gate their "ready to scan" state on the
         * host asserting DTR (Data Terminal Ready). Without this, the FM430's
         * S LED flashes red and the trigger does nothing — open succeeds but
         * the device thinks no host is listening.
         *
         * Line coding is set to 9600 8N1 first (the byte-pipe content is
         * encoding-agnostic, but some scanner firmware refuses to scan until
         * any line coding has been applied). DTR=1 then signals "host ready".
         * ESP_ERR_NOT_SUPPORTED is non-fatal: a few CDC-like devices don't
         * implement these requests but still work over the bulk pipe. */
        const cdc_acm_line_coding_t line_coding = {
            .dwDTERate  = 9600,
            .bCharFormat = 0,     /* 1 stop bit */
            .bParityType = 0,     /* none */
            .bDataBits   = 8,
        };
        ret = cdc_acm_host_line_coding_set(dev, &line_coding);
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "line_coding_set not supported by device, continuing");
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "line_coding_set failed: %s", esp_err_to_name(ret));
        }

        ret = cdc_acm_host_set_control_line_state(dev, true, true);  /* DTR=1, RTS=1 */
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "set_control_line_state not supported by device, continuing");
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set_control_line_state failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "DTR+RTS asserted (scanner should be ready, S LED should go green)");
        }

        ESP_LOGI(TAG, "scanner connected, waiting for scans");

        /* Block until the event_cb signals a disconnect. */
        (void)xSemaphoreTake(s_disconnect_sem, portMAX_DELAY);

        /* Close and free the handle, then loop back to open. */
        if (s_cdc_dev != NULL) {
            cdc_acm_host_close(s_cdc_dev);
            s_cdc_dev = NULL;
        }
        ESP_LOGI(TAG, "scanner closed, reopening");
    }

    /* Unreachable — task loops forever. */
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void wuhe_cdc_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(wuhe_cdc_task, "wuhe_cdc",
                                            4096, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(wuhe_cdc) failed");
    }
}
