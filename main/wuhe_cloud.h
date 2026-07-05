#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Request / Response structs                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t sid;              /* Submission sequence number (unique within window) */
    uint16_t wid;              /* Work mode: 1=scan, 2=heartbeat, 3=power-on      */
    const char **scancodes;    /* Array of scanned barcode strings                */
    uint8_t scancode_count;   /* Number of entries in scancodes                  */
    char mac[13];              /* Device MAC address (12 hex chars + NUL)         */
    char mno[21];              /* Device batch number (20 chars + NUL)            */
    uint8_t port;              /* Scanner port (1-4)                              */
} wuhe_request_t;

typedef struct {
    uint16_t sid;              /* Echo of request SID                             */
    uint8_t rid;               /* Result: 0 = success, non-0 = error              */
    uint16_t wid;              /* Echo of request work mode                       */
    char rcode[21];            /* English error code when rid != 0                */
    char note01[21];           /* Display field 1 (e.g. warehouse, 4 chars)      */
    char note02[21];           /* Display field 2 (e.g. station, 2 chars)        */
    char note03[21];           /* Display field 3 (e.g. employee ID, 4 chars)    */
    char note04[21];           /* Display field 4 (e.g. count, 4 digits)         */
    uint8_t restart;           /* 1 = device should restart                      */
    uint8_t need_output;       /* 0=none, 1=output after reply, 2=output first   */
    char voice_output[21];      /* Reserved: voice prompt string                   */
    uint8_t pack_interval;      /* Pack frequency in 100 ms units                 */
    uint8_t pack_max;           /* Max codes per pack                              */
    uint8_t beep_loop;          /* Beep repeat count (0 = silent)                 */
    uint8_t beep_time;          /* Beep duration per cycle (units of 100 ms?)     */
    uint16_t beep_interval;     /* Gap between beeps (ms)                          */
} wuhe_response_t;

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define WUHE_ENDPOINT_LIST     "http://192.168.2.15:5000/openWeb/SaveScanDataList"
#define WUHE_ENDPOINT_BEEP     "http://192.168.2.15:5000/openWeb/SaveScanDataWithBeep"
// #define WUHE_ENDPOINT_LIST     "https://test.wuhe1.com/openWeb/SaveScanDataList"
// #define WUHE_ENDPOINT_BEEP     "https://test.wuhe1.com/openWeb/SaveScanDataWithBeep"

#define WUHE_WORK_MODE_SCAN      1
#define WUHE_WORK_MODE_HEARTBEAT 2
#define WUHE_WORK_MODE_POWERON   3

#define WUHE_DEFAULT_PORT           1
#define WUHE_SID_MAX                65535
#define WUHE_HEARTBEAT_SEC          60
#define WUHE_PACK_INTERVAL_DEFAULT_MS  100
#define WUHE_PACK_MAX_DEFAULT       10
#define WUHE_HTTP_TIMEOUT_MS        15000

#define WUHE_BACKUP_PARTITION_LABEL  "wuhe_bak"
#define WUHE_BACKUP_MOUNT             "/wuhe_bak"
#define WUHE_BACKUP_FILE_MAX          2000

#define WUHE_RETRY_MAX               5
#define WUHE_RETRY_BACKOFF_BASE_MS    1000
#define WUHE_RETRY_BACKOFF_CAP_MS     60000

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * Initialise the wuhe-cloud subsystem (starts background tasks,
 * connects Wi-Fi if needed).
 */
void wuhe_cloud_start(void);

/**
 * Submit a single scanned barcode to the cloud queue.
 * Returns true if enqueued successfully.
 */
bool wuhe_cloud_submit_scan(const char *code);

/**
 * Apply a server response: update display fields, trigger beeps,
 * handle restart / output flags, etc.
 */
void wuhe_cloud_apply_response(const wuhe_response_t *r);
