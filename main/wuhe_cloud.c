/**
 * wuhe_cloud.c — WuHeYun cloud HTTPS protocol layer.
 *
 * Implements (Tasks 5/6/7):
 *   - cJSON request serialization   (wuhe_build_request_json / wuhe_request_to_string)
 *   - cJSON response parsing        (wuhe_parse_response_json)
 *   - HTTPS POST client             (wuhe_http_post)
 *
 * The higher-level orchestrators (wuhe_cloud_start / wuhe_cloud_submit_scan /
 * wuhe_cloud_apply_response) are added by Task 9; this file deliberately omits
 * them so the cloud queue/state-machine can be layered on later.
 */

#include "wuhe_cloud.h"
#include "wuhe_storage.h"
#include "wuhe_backup.h"
#include "internet_mgr.h"
#include "screen_display.h"
#include "scanner_config.h"   /* WUHE_BARCODE_MAX_BYTES */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/param.h>           /* MIN */

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"            /* esp_efuse_mac_get_default */
#include "esp_system.h"         /* esp_restart */
#include "esp_timer.h"          /* esp_timer_get_time */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "wuhe.cloud";

/* ------------------------------------------------------------------ */
/*  Forward declarations (static helpers)                              */
/* ------------------------------------------------------------------ */

static void wuhe_get_mac_hex(char out[13]);
static bool get_number(const cJSON *root, const char *key, uint32_t *out);
static void get_string(const cJSON *root, const char *key, char *dst, size_t cap);

/* ------------------------------------------------------------------ */
/*  Task 5 — request serialization                                     */
/* ------------------------------------------------------------------ */

/* MAC as 12 uppercase hex chars + NUL. ESP-IDF stores the base MAC in eFuse;
 * esp_efuse_mac_get_default() is declared in esp_mac.h (despite its name). */
static void wuhe_get_mac_hex(char out[13])
{
    uint8_t m[6];
    esp_efuse_mac_get_default(m);
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

/*
 * Build a cJSON object from a wuhe_request_t. Returns a newly allocated root
 * (caller owns / must cJSON_Delete), or NULL on allocation failure.
 * ScanCode is always an array — empty ([]) when scancode_count == 0, which is
 * the heartbeat / power-on form.
 */
cJSON *wuhe_build_request_json(const wuhe_request_t *req)
{
    if (req == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "SID",  req->sid);
    cJSON_AddNumberToObject(root, "WID",  req->wid);
    cJSON_AddNumberToObject(root, "Port", req->port);
    cJSON_AddStringToObject(root, "Mac",  req->mac);
    cJSON_AddStringToObject(root, "MNo",  req->mno);

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (uint8_t i = 0; i < req->scancode_count; i++) {
        if (req->scancodes && req->scancodes[i]) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(req->scancodes[i]));
        }
    }
    cJSON_AddItemToObject(root, "ScanCode", arr);

    return root;
}

/*
 * Serialize a request to a heap string (unformatted JSON). Caller MUST free()
 * the returned pointer. Returns NULL on allocation failure.
 */
char *wuhe_request_to_string(const wuhe_request_t *req)
{
    cJSON *root = wuhe_build_request_json(req);
    if (root == NULL) {
        return NULL;
    }
    char *s = cJSON_PrintUnformatted(root);   /* heap-allocated, or NULL */
    cJSON_Delete(root);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Task 6 — response parsing (defensive)                              */
/* ------------------------------------------------------------------ */

/* Fetch a numeric field. Returns true and stores the value when the key exists
 * and holds a JSON number; otherwise returns false and leaves *out untouched. */
static bool get_number(const cJSON *root, const char *key, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == NULL || !cJSON_IsNumber(item)) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

/* Fetch a string field into a fixed buffer via strlcpy (NUL-safe, capped).
 * No-op when the key is missing or not a string. */
static void get_string(const cJSON *root, const char *key, char *dst, size_t cap)
{
    if (cap == 0) {
        return;
    }
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, cap);
    }
}

/*
 * Parse a server response body into `out`. Every GetObjectItem result is
 * NULL/type-checked before use; missing fields keep their zero defaults, with
 * `rid` seeded to 0xFF so an absent RID is detectable as an anomaly.
 * Returns false only when the body is not valid JSON.
 */
bool wuhe_parse_response_json(const char *body, wuhe_response_t *out)
{
    if (body == NULL || out == NULL) {
        return false;
    }

    /* Start from a clean baseline; strings become "" and numbers 0. */
    memset(out, 0, sizeof(*out));
    out->rid = 0xFF;   /* sentinel: cleared only if the server sends a real RID */

    cJSON *r = cJSON_Parse(body);
    if (r == NULL) {
        ESP_LOGE(TAG, "response JSON parse failed");
        return false;
    }

    uint32_t v = 0;
    if (get_number(r, "SID", &v))          { out->sid = (uint16_t)v; }
    if (get_number(r, "RID", &v))          { out->rid = (uint8_t)v; }
    if (get_number(r, "WID", &v))          { out->wid = (uint16_t)v; }
    if (get_number(r, "ReStart", &v))      { out->restart = (uint8_t)v; }
    if (get_number(r, "NeedOutput", &v))   { out->need_output = (uint8_t)v; }
    if (get_number(r, "PackInterval", &v)) { out->pack_interval = (uint8_t)v; }
    if (get_number(r, "PackMax", &v))      { out->pack_max = (uint8_t)v; }
    if (get_number(r, "BeepLoop", &v))     { out->beep_loop = (uint8_t)v; }
    if (get_number(r, "BeepTime", &v))     { out->beep_time = (uint8_t)v; }

    /* Spec sample key "BeepInterval " carries a trailing space — try the
     * canonical spelling first, then the malformed variant as a fallback. */
    if (get_number(r, "BeepInterval", &v) || get_number(r, "BeepInterval ", &v)) {
        out->beep_interval = (uint16_t)v;
    }

    get_string(r, "RCode",       out->rcode,       sizeof(out->rcode));
    get_string(r, "Note01",      out->note01,      sizeof(out->note01));
    get_string(r, "Note02",      out->note02,      sizeof(out->note02));
    get_string(r, "Note03",      out->note03,      sizeof(out->note03));
    get_string(r, "Note04",      out->note04,      sizeof(out->note04));
    get_string(r, "VoiceOutput", out->voice_output, sizeof(out->voice_output));

    cJSON_Delete(r);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Task 7 — HTTPS POST client                                         */
/* ------------------------------------------------------------------ */

/*
 * POST `json_body` to `url` over TLS, capturing the response body into
 * `resp_buf` (NUL-terminated, at most resp_cap-1 bytes). Returns true iff the
 * server replies with a 2xx status. The http handle is cleaned up on every
 * path (success and failure).
 */
bool wuhe_http_post(const char *url, const char *json_body,
                    char *resp_buf, size_t resp_cap)
{
    if (url == NULL || json_body == NULL) {
        return false;
    }

    esp_http_client_config_t config = {
        .url             = url,
        .transport_type  = HTTP_TRANSPORT_OVER_TCP,
        .method          = HTTP_METHOD_POST,
        .timeout_ms      = WUHE_HTTP_TIMEOUT_MS,
#ifdef CONFIG_WUHE_SKIP_CERT_VALIDATE
        /* Debug-only: loosen CN validation. Production must keep the CA bundle. */
        .skip_cert_common_name_check = true,
#else
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
#ifdef CONFIG_WUHE_SKIP_CERT_VALIDATE
    ESP_LOGW(TAG, "TLS cert validation skipped (debug build)");
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "http_client_init failed for %s", url);
        return false;
    }

    esp_err_t err;
    int written = 0;
    int status  = 0;
    bool ok     = false;

    err = esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_header failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    size_t body_len = strlen(json_body);
    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    written = esp_http_client_write(client, json_body, (int)body_len);
    if (written < 0 || (size_t)written != body_len) {
        ESP_LOGE(TAG, "write incomplete: wrote %d of %zu bytes", written, body_len);
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "fetch_headers failed");
        goto cleanup;
    }

    /* Drain the response body into the caller's buffer (capped, NUL-terminated). */
    if (resp_buf != NULL && resp_cap > 0) {
        size_t total = 0;
        int n;
        while (total < resp_cap - 1 &&
               (n = esp_http_client_read(client, resp_buf + total,
                                         (int)(resp_cap - 1 - total))) > 0) {
            total += (size_t)n;
        }
        resp_buf[total] = '\0';
    }

    status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d from %s", status, url);
        goto cleanup;
    }

    ok = true;

cleanup:
    esp_http_client_cleanup(client);   /* ALWAYS paired with init */
    return ok;
}

/* ================================================================== */
/*  Task 9 — cloud orchestrator: queue / dedup / batching / heartbeat  */
/*             offline-backup / retry-backoff / response-dispatch      */
/*  Everything below is NEW; the T5/6/7 functions above are untouched. */
/* ================================================================== */

/* Max items a single batch buffer can hold (spec PackMax upper bound 20). */
#define WUHE_PACK_ARRAY_MAX   20
/* Response-body scratch buffer for POST replies (spec sample ~300 B). */
#define WUHE_RESP_BUF_SIZE   1024

/* ---- A) queue item + dedup cache (file-scope) --------------------- */

typedef struct {
    char     code[WUHE_BARCODE_MAX_BYTES];   /* barcode + NUL (UTF-8, up to 40 CJK chars) */
    uint16_t sid;        /* SID captured at scan time */
} scan_item_t;

static QueueHandle_t s_queue;     /* cap 32, populated by wuhe_cloud_start */

/* RAM-only dedup cache; never persisted (plan: no flash write per scan). */
static char    dedup_code[CONFIG_WUHE_DEDUP_CACHE_SIZE][WUHE_BARCODE_MAX_BYTES];
static uint64_t dedup_ts[CONFIG_WUHE_DEDUP_CACHE_SIZE];   /* esp_timer ms */

/* Pack parameters adopted from the most recent server response. */
static uint32_t s_pack_interval_ms = WUHE_PACK_INTERVAL_DEFAULT_MS;
static uint8_t  s_pack_max         = WUHE_PACK_MAX_DEFAULT;

/* Drain backoff: after drain_backup stalls on transport failure, wait this
 * long before the main loop tries again. Prevents hammering a dead server
 * every cycle when WiFi is up but the HTTP endpoint is unreachable. */
#define WUHE_DRAIN_BACKOFF_MS   60000U
static uint64_t s_last_drain_fail_ms = 0;   /* 0 = no prior failure */

/* ---- forward decls ------------------------------------------------ */

static bool send_pack(scan_item_t *items, uint8_t n);
static void drain_backup(void);
static void send_heartbeat(void);
static void wuhe_cloud_task(void *arg);

/* ---- F) beep hook (hardware not implemented) ---------------------- */

static void wuhe_beep_notify(const wuhe_response_t *r)
{
    (void)r;
    /* Buzzer driver deferred; Beep* fields already logged by the caller. */
}

/* ---- F) response dispatch ----------------------------------------- */

void wuhe_cloud_apply_response(const wuhe_response_t *r)
{
    if (r == NULL) {
        return;
    }

    /* The four *_safe setters each acquire lvgl_api_lock internally, so we
     * must NOT hold the lock here (double-acquire of _lock_t is undefined). */
    wuhe_ui_set_warehouse_safe(r->note01);
    wuhe_ui_set_post_safe(r->note02);
    wuhe_ui_set_workid_safe(r->note03);
    wuhe_ui_set_count_safe(r->note04);

    /* Adopt dynamic batching parameters when the server supplies sane ones. */
    if (r->pack_interval > 0) {
        s_pack_interval_ms = (uint32_t)r->pack_interval * 100U;   /* 100 ms units */
    }
    if (r->pack_max > 0) {
        s_pack_max = (r->pack_max > WUHE_PACK_ARRAY_MAX) ? WUHE_PACK_ARRAY_MAX
                                                         : r->pack_max;
    }

    if (r->beep_loop > 0) {
        ESP_LOGI(TAG, "beep: loop=%u time=%u interval=%u",
                 r->beep_loop, r->beep_time, r->beep_interval);
    }
    wuhe_beep_notify(r);

    if (r->restart == 1) {
        ESP_LOGI(TAG, "server requested restart (ReStart=1), rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

/* ---- C) send one batch with per-pack exponential-backoff retry ---- *
 * Returns true when the pack is resolved (success RID==0, or dead-letter
 * RID!=0 — either way it is gone).  Returns false only when every retry
 * failed at the transport level, in which case the items have been pushed
 * back to the littlefs backup so a later drain can retry them.  The bool
 * lets drain_backup stop instead of spinning on a dead link.            */
static bool send_pack(scan_item_t *items, uint8_t n)
{
    if (n == 0 || items == NULL) {
        return true;
    }

    char mac[13];
    wuhe_get_mac_hex(mac);
    char mno[21];
    wuhe_storage_mno_get(mno, sizeof(mno));

    /* scancodes points into the caller's items[] (read-only within this call). */
    const char *codes[WUHE_PACK_ARRAY_MAX];
    for (uint8_t i = 0; i < n; i++) {
        codes[i] = items[i].code;
    }

    wuhe_request_t req;
    memset(&req, 0, sizeof(req));
    req.sid            = items[0].sid;   /* ONE SID per pack (spec) */
    req.wid            = WUHE_WORK_MODE_SCAN;
    req.scancodes      = codes;
    req.scancode_count = n;
    req.port           = WUHE_DEFAULT_PORT;
    strlcpy(req.mac, mac, sizeof(req.mac));
    strlcpy(req.mno, mno, sizeof(req.mno));

    char *body = wuhe_request_to_string(&req);   /* heap; cJSON_Print inside */
    if (body == NULL) {
        ESP_LOGE(TAG, "send_pack: request body alloc failed, backing up %u", n);
        for (uint8_t i = 0; i < n; i++) {
            wuhe_backup_push(items[i].code, items[i].sid);
        }
        return false;
    }

    char resp_buf[WUHE_RESP_BUF_SIZE];
    bool resolved = false;   /* set true on success (RID==0) or dead-letter (RID!=0) */

    for (uint8_t k = 0; k < WUHE_RETRY_MAX; k++) {
        if (wuhe_http_post(WUHE_ENDPOINT_BEEP, body, resp_buf, sizeof(resp_buf))) {
            wuhe_response_t resp;
            if (wuhe_parse_response_json(resp_buf, &resp)) {
                if (resp.rid == 0) {
                    wuhe_cloud_apply_response(&resp);
                    ESP_LOGI(TAG, "pack ok: n=%u sid=%u", n, items[0].sid);
                    resolved = true;
                    break;
                }
                /* RID!=0: server permanently rejects this pack — dead-letter. */
                ESP_LOGE(TAG, "dead-letter RID=%u RCode=%s sid=%u (no retry, no backup)",
                         resp.rid, resp.rcode, items[0].sid);
                resolved = true;   /* consumed: do NOT push to backup */
                break;
            }
            /* 2xx but un-parseable body: treat as transient, fall to backoff. */
            ESP_LOGW(TAG, "2xx but bad JSON, will retry (k=%u)", k);
        }

        /* transport error or parse error → exponential backoff, same SID */
        uint32_t backoff = MIN((uint32_t)WUHE_RETRY_BACKOFF_CAP_MS,
                               (uint32_t)WUHE_RETRY_BACKOFF_BASE_MS << k);
        ESP_LOGW(TAG, "pack retry %u/%u backoff=%lums", k + 1, WUHE_RETRY_MAX,
                 (unsigned long)backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }

    free(body);   /* always pair with wuhe_request_to_string */

    if (!resolved) {
        /* Exhausted all retries at transport level → park in backup for a
         * later drain; do NOT infinite-loop in the caller. */
        ESP_LOGW(TAG, "pack failed after %u retries, %u items backed up",
                 WUHE_RETRY_MAX, n);
        for (uint8_t i = 0; i < n; i++) {
            wuhe_backup_push(items[i].code, items[i].sid);
        }
    }

    return resolved;   /* false ⇒ transport_fail, caller (drain) should stop */
}

/* ---- D) drain littlefs backup FIFO on reconnect -------------------- */

static void drain_backup(void)
{
    ESP_LOGI(TAG, "drain start, backup count=%u", (unsigned)wuhe_backup_count());

    while (wuhe_backup_count() > 0) {
        scan_item_t batch[WUHE_PACK_ARRAY_MAX];
        uint8_t n = 0;

        while (n < WUHE_PACK_ARRAY_MAX && wuhe_backup_count() > 0) {
            uint16_t sid = 0;
            if (!wuhe_backup_pop(batch[n].code, sizeof(batch[n].code), &sid)) {
                break;
            }
            batch[n].sid = sid;
            n++;
        }
        if (n == 0) {
            break;
        }

        ESP_LOGI(TAG, "drain pack: n=%u", n);
        bool resolved = send_pack(batch, n);
        if (!resolved) {
            /* Link still dead and items were re-pushed: stop to avoid spin.
             * Record the failure timestamp so the main loop's periodic drain
             * check backs off instead of re-entering every cycle. The next
             * WiFi rising-edge bypasses this backoff (intentional). */
            s_last_drain_fail_ms = esp_timer_get_time() / 1000U;
            ESP_LOGW(TAG, "drain stalled (transport fail), retry in %us",
                     (unsigned)(WUHE_DRAIN_BACKOFF_MS / 1000U));
            break;
        }
        /* resolved (success or dead-letter): keep draining remaining backlog. */
    }

    ESP_LOGI(TAG, "drain done, backup count=%u", (unsigned)wuhe_backup_count());
}

/* ---- heartbeat (WID=2, empty ScanCode) ----------------------------- */

static void send_heartbeat(void)
{
    char mac[13];
    wuhe_get_mac_hex(mac);
    char mno[21];
    wuhe_storage_mno_get(mno, sizeof(mno));

    wuhe_request_t req;
    memset(&req, 0, sizeof(req));
    req.sid            = wuhe_storage_sid_next();
    req.wid            = WUHE_WORK_MODE_HEARTBEAT;
    req.scancodes      = NULL;
    req.scancode_count = 0;
    req.port           = WUHE_DEFAULT_PORT;
    strlcpy(req.mac, mac, sizeof(req.mac));
    strlcpy(req.mno, mno, sizeof(req.mno));

    char *body = wuhe_request_to_string(&req);
    if (body == NULL) {
        ESP_LOGE(TAG, "heartbeat body alloc failed");
        return;
    }

    char resp_buf[WUHE_RESP_BUF_SIZE];
    if (wuhe_http_post(WUHE_ENDPOINT_BEEP, body, resp_buf, sizeof(resp_buf))) {
        wuhe_response_t resp;
        if (wuhe_parse_response_json(resp_buf, &resp)) {
            if (resp.rid == 0) {
                wuhe_cloud_apply_response(&resp);
            } else {
                ESP_LOGW(TAG, "heartbeat RID=%u RCode=%s", resp.rid, resp.rcode);
            }
        }
    }
    free(body);
}

/* ---- B) non-blocking scan submit (called from HID callback) -------- */

bool wuhe_cloud_submit_scan(const char *code)
{
    if (code == NULL || code[0] == '\0') {
        return false;
    }

    /* --- dedup (RAM-only, independent of upload success) --- */
    uint64_t now = esp_timer_get_time() / 1000U;   /* ms since boot */
    int slot = 0;
    uint64_t oldest = UINT64_MAX;
    bool is_dup = false;

    for (int i = 0; i < CONFIG_WUHE_DEDUP_CACHE_SIZE; i++) {
        if (dedup_code[i][0] != '\0' && strcmp(dedup_code[i], code) == 0) {
            if ((now - dedup_ts[i]) < (uint64_t)CONFIG_WUHE_DEDUP_WINDOW_MS) {
                is_dup = true;
            }
            slot = i;   /* refresh existing matching slot whether dup or expired */
            break;
        }
        if (dedup_ts[i] < oldest) {
            oldest = dedup_ts[i];
            slot = i;   /* pick oldest (empty slots have ts 0 → chosen first) */
        }
    }

    if (is_dup) {
        return true;   /* silently dropped as duplicate */
    }
    strlcpy(dedup_code[slot], code, sizeof(dedup_code[slot]));
    dedup_ts[slot] = now;

    /* --- SID assigned at scan time so backup retries reuse it --- */
    uint16_t sid = wuhe_storage_sid_next();

    if (wifi_is_connected() && s_queue != NULL) {
        scan_item_t item;
        strlcpy(item.code, code, sizeof(item.code));
        item.sid = sid;
        if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
            /* Queue full: relocate oldest to flash backup so drain_backup can
             * replay it later — never silently drop scanned data. */
            scan_item_t dropped;
            xQueueReceive(s_queue, &dropped, 0);
            ESP_LOGW(TAG, "queue full, relocating SID=%u to backup", dropped.sid);
            wuhe_backup_push(dropped.code, dropped.sid);
            xQueueSend(s_queue, &item, 0);
        }
    } else {
        /* Offline: persist to littlefs; drain_backup replays on reconnect. */
        wuhe_backup_push(code, sid);
        ESP_LOGI(TAG, "[wuhe] backup +1 (offline)");
    }

    return true;
}

/* ---- E) cloud task state machine ----------------------------------- */

static void wuhe_cloud_task(void *arg)
{
    (void)arg;

    /* wuhe_backup_init() runs synchronously in app_main before this task
     * starts, to avoid an FD-table registration race with lwip. */

    /* Wait for first Wi-Fi connection before doing anything network-bound. */
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* ---- power-on handshake: fired EXACTLY once per boot ---- */
    {
        char mac[13];
        wuhe_get_mac_hex(mac);
        char mno[21];
        wuhe_storage_mno_get(mno, sizeof(mno));

        wuhe_request_t req;
        memset(&req, 0, sizeof(req));
        req.sid            = wuhe_storage_sid_next();
        req.wid            = WUHE_WORK_MODE_POWERON;
        req.scancodes      = NULL;
        req.scancode_count = 0;
        req.port           = WUHE_DEFAULT_PORT;
        strlcpy(req.mac, mac, sizeof(req.mac));
        strlcpy(req.mno, mno, sizeof(req.mno));

        char *body = wuhe_request_to_string(&req);
        if (body != NULL) {
            char resp_buf[WUHE_RESP_BUF_SIZE];
            if (wuhe_http_post(WUHE_ENDPOINT_BEEP, body, resp_buf, sizeof(resp_buf))) {
                wuhe_response_t resp;
                if (wuhe_parse_response_json(resp_buf, &resp)) {
                    if (resp.rid == 0) {
                        wuhe_cloud_apply_response(&resp);
                        ESP_LOGI(TAG, "[wuhe] poweron POST ok RID=0");
                    } else {
                        ESP_LOGE(TAG, "[wuhe] poweron RID=%u RCode=%s",
                                 resp.rid, resp.rcode);
                    }
                } else {
                    ESP_LOGE(TAG, "[wuhe] poweron response un-parseable");
                }
            } else {
                ESP_LOGE(TAG, "[wuhe] poweron POST failed (transport)");
            }
            free(body);
        }
    }

    /* We enter the loop already connected (guaranteed by the wait above). */
    bool was_connected = true;

    for (;;) {
        bool now_connected = wifi_is_connected();

        /* Rising edge disconnected→connected: replay backlog, no poweron. */
        if (!was_connected && now_connected) {
            ESP_LOGI(TAG, "reconnect detected, draining backup");
            drain_backup();
        }
        was_connected = now_connected;

        if (!now_connected) {
            /* Offline: scans land in backup via submit; just idle. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Periodic drain: covers the case where WiFi stayed associated but
         * the HTTP server was temporarily unreachable (send_pack exhausted
         * its retries and pushed items to backup). Without this, those items
         * would sit indefinitely — no WiFi rising-edge ever fires when only
         * the HTTP layer went down. Backoff window prevents hammering a
         * still-dead server every loop cycle. s_last_drain_fail_ms == 0
         * means "never failed", allowing immediate first-attempt drain. */
        if (wuhe_backup_count() > 0 &&
            (s_last_drain_fail_ms == 0 ||
             (esp_timer_get_time() / 1000U) >=
                 s_last_drain_fail_ms + WUHE_DRAIN_BACKOFF_MS)) {
            ESP_LOGI(TAG, "periodic drain: backup count=%u",
                     (unsigned)wuhe_backup_count());
            drain_backup();
        }

        /* ---- online: assemble one pack ---- */
        scan_item_t pack[WUHE_PACK_ARRAY_MAX];
        uint8_t n = 0;

        /* Block up to one heartbeat interval for the first item. */
        if (xQueueReceive(s_queue, &pack[0],
                          pdMS_TO_TICKS(WUHE_HEARTBEAT_SEC * 1000)) != pdTRUE) {
            /* Timeout with no scans → heartbeat keep-alive. */
            send_heartbeat();
            continue;
        }
        n = 1;

        /* Squeeze the queue within the pack-interval window (non-blocking). */
        uint8_t eff_max = s_pack_max;
        if (eff_max > WUHE_PACK_ARRAY_MAX) {
            eff_max = WUHE_PACK_ARRAY_MAX;
        }
        uint64_t deadline = (esp_timer_get_time() / 1000U) + s_pack_interval_ms;
        while (n < eff_max) {
            if ((esp_timer_get_time() / 1000U) >= deadline) {
                break;
            }
            if (xQueueReceive(s_queue, &pack[n], 0) != pdTRUE) {
                break;   /* queue empty → send what we have, low latency */
            }
            n++;
        }

        send_pack(pack, n);
    }
}

/* ---- G) public entry point ----------------------------------------- */

void wuhe_cloud_start(void)
{
    s_queue = xQueueCreate(32, sizeof(scan_item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate(32) failed — cloud task not started");
        return;
    }
    BaseType_t ok = xTaskCreate(wuhe_cloud_task, "wuhe_cloud", 10240, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(wuhe_cloud) failed");
    }
}
