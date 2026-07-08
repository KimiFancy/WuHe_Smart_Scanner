#include "wuhe_backup.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>      /* fsync */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wuhe_cloud.h"  /* WUHE_BACKUP_* constants (task 2). */
#include "scanner_config.h"   /* WUHE_BARCODE_MAX_BYTES */

static const char *TAG = "wuhe.bak";

/* ------------------------------------------------------------------ */
/* On-disk layout                                                     */
/* ------------------------------------------------------------------ */

/* Index file: binary {head, tail, count}. 12 bytes. */
typedef struct {
    uint32_t head;   /* slot index of the oldest entry (next to pop) */
    uint32_t tail;   /* slot index of the next free slot for push    */
    uint32_t count;  /* number of valid entries currently stored     */
} wuhe_backup_idx_t;

/* Entry file: binary {sid, code[N]}. N = WUHE_BARCODE_MAX_BYTES (121).
 * WARNING: Bumping WUHE_BACKUP_CODE_FIELD_LEN invalidates existing on-disk
 * entries written by older firmware (different struct size). On first boot
 * after upgrade, wuhe_backup_init's read-back may fail and trigger the
 * format+remount fallback in wuhe_backup.c, erasing the offline backlog.
 * This is acceptable for dev devices; for production deploy a version/magic
 * field migration (see AGENTS.md). */
#define WUHE_BACKUP_CODE_FIELD_LEN WUHE_BARCODE_MAX_BYTES
typedef struct {
    uint16_t sid;
    char     code[WUHE_BACKUP_CODE_FIELD_LEN];
} wuhe_backup_entry_t;

/* ------------------------------------------------------------------ */
/* Module state (accessed from two tasks: HID task push / cloud task pop — guarded by s_lock) */
/* ------------------------------------------------------------------ */

static bool             s_mounted = false;
static wuhe_backup_idx_t s_idx    = {0, 0, 0};
static uint32_t         s_dropped = 0;   /* cumulative overflow drops */
static SemaphoreHandle_t s_lock   = NULL;

#define IDX_PATH  WUHE_BACKUP_MOUNT "/idx"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void entry_path(uint32_t slot, char *buf, size_t cap)
{
    /* 6-digit zero-padded decimal covers WUHE_BACKUP_FILE_MAX up to 999999. */
    (void)snprintf(buf, cap, "%s/e_%06" PRIu32, WUHE_BACKUP_MOUNT, slot);
}

/* Atomically write the index. LittleFS rename gives all-or-nothing on disk. */
static bool idx_write(void)
{
    char tmp[64];
    (void)snprintf(tmp, sizeof(tmp), "%s/.idx.tmp", WUHE_BACKUP_MOUNT);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "idx_write: open(%s) failed", tmp);
        return false;
    }
    if (fwrite(&s_idx, sizeof(s_idx), 1, f) != 1) {
        ESP_LOGE(TAG, "idx_write: write failed");
        fclose(f);
        return false;
    }
    (void)fflush(f);
    (void)fsync(fileno(f));   /* best-effort; close() also commits */
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "idx_write: close failed");
        return false;
    }
    if (rename(tmp, IDX_PATH) != 0) {
        ESP_LOGE(TAG, "idx_write: rename %s -> %s failed", tmp, IDX_PATH);
        return false;
    }
    return true;
}

/* Load idx from disk into s_idx. If missing/invalid, init to empty. */
static void idx_load(void)
{
    s_idx.head = s_idx.tail = s_idx.count = 0;
    FILE *f = fopen(IDX_PATH, "rb");
    if (f == NULL) {
        /* First boot or wiped partition: persist a zeroed idx so count() works. */
        (void)idx_write();
        return;
    }
    wuhe_backup_idx_t tmp;
    size_t n = fread(&tmp, sizeof(tmp), 1, f);
    (void)fclose(f);
    if (n != 1) {
        ESP_LOGW(TAG, "idx_load: short read, resetting FIFO");
        (void)idx_write();
        return;
    }
    /* Sanity-check bounds; corrupt idx -> reset rather than crash. */
    if (tmp.head >= WUHE_BACKUP_FILE_MAX || tmp.tail >= WUHE_BACKUP_FILE_MAX ||
        tmp.count > WUHE_BACKUP_FILE_MAX) {
        ESP_LOGE(TAG, "idx_load: out-of-range head=%" PRIu32 " tail=%" PRIu32
                     " count=%" PRIu32 ", resetting FIFO",
                 tmp.head, tmp.tail, tmp.count);
        (void)idx_write();
        return;
    }
    s_idx = tmp;
}

/* Write one entry file at `slot`. Returns false on I/O failure. */
static bool entry_write(uint32_t slot, const char *code, uint16_t sid)
{
    char path[64];
    entry_path(slot, path, sizeof(path));

    wuhe_backup_entry_t e;
    e.sid = sid;
    memset(e.code, 0, sizeof(e.code));
    /* strlcpy guarantees NUL within sizeof(e.code) and never overflows. */
    (void)strlcpy(e.code, code ? code : "", sizeof(e.code));

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "entry_write: open(%s) failed", path);
        return false;
    }
    if (fwrite(&e, sizeof(e), 1, f) != 1) {
        ESP_LOGE(TAG, "entry_write: write(%s) failed", path);
        (void)fclose(f);
        (void)remove(path);
        return false;
    }
    (void)fflush(f);
    (void)fsync(fileno(f));
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "entry_write: close(%s) failed", path);
        return false;
    }
    return true;
}

/* Read one entry at `slot`. Returns false on I/O failure. */
static bool entry_read(uint32_t slot, char *code_out, size_t cap, uint16_t *sid_out)
{
    char path[64];
    entry_path(slot, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "entry_read: open(%s) failed", path);
        return false;
    }
    wuhe_backup_entry_t e;
    size_t n = fread(&e, sizeof(e), 1, f);
    (void)fclose(f);
    if (n != 1) {
        ESP_LOGE(TAG, "entry_read: short read on %s", path);
        return false;
    }
    /* Defensive: force NUL termination even if file was tampered. */
    e.code[WUHE_BACKUP_CODE_FIELD_LEN - 1] = '\0';
    if (sid_out) {
        *sid_out = e.sid;
    }
    if (cap > 0) {
        (void)strlcpy(code_out, e.code, cap);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool wuhe_backup_init(void)
{
    if (s_mounted) {
        return true;
    }

    esp_vfs_littlefs_conf_t conf = {
        .format_if_mount_failed = true,
        .base_path              = WUHE_BACKUP_MOUNT,
        .partition_label        = WUHE_BACKUP_PARTITION_LABEL,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register failed: %s — trying format+remount",
                 esp_err_to_name(err));
        /* Hard retry: wipe partition then register again. */
        err = esp_littlefs_format(WUHE_BACKUP_PARTITION_LABEL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "format failed: %s — backup UNAVAILABLE (read-only degrade)",
                     esp_err_to_name(err));
            return false;
        }
        err = esp_vfs_littlefs_register(&conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "remount after format failed: %s — backup UNAVAILABLE",
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGW(TAG, "partition reformatted; any prior backed-up scans lost");
    }

    /* Report real usage so operators can spot a near-full partition. */
    size_t total = 0, used = 0;
    if (esp_littlefs_info(WUHE_BACKUP_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s at %s — %zu/%zu bytes used",
                 WUHE_BACKUP_PARTITION_LABEL, WUHE_BACKUP_MOUNT, used, total);
    } else {
        ESP_LOGI(TAG, "mounted %s at %s", WUHE_BACKUP_PARTITION_LABEL, WUHE_BACKUP_MOUNT);
    }

    idx_load();   /* sets s_idx (writes a zeroed idx on first boot) */
    s_mounted = true;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex create failed — running WITHOUT locking (degraded)");
    }

    ESP_LOGI(TAG, "FIFO ready: count=%" PRIu32, s_idx.count);
    return true;
}

bool wuhe_backup_push(const char *code, uint16_t sid)
{
    if (!s_mounted) {
        return false;
    }
    if (code == NULL || code[0] == '\0') {
        ESP_LOGW(TAG, "push: rejected empty code");
        return false;
    }

    /* Non-blocking take: HID callback must never block. */
    if (s_lock && xSemaphoreTake(s_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "push: lock busy, dropping scan (non-blocking)");
        return false;
    }

    /* If FIFO is full, drop the oldest entry first (overwrite-at-head policy). */
    if (s_idx.count >= WUHE_BACKUP_FILE_MAX) {
        char path[64];
        entry_path(s_idx.head, path, sizeof(path));
        (void)remove(path);                       /* best-effort delete */
        s_idx.head = (s_idx.head + 1) % WUHE_BACKUP_FILE_MAX;
        s_idx.count--;                            /* now there's room */
        s_dropped++;
        ESP_LOGW(TAG, "FIFO full — dropped oldest entry (total dropped=%" PRIu32 ")",
                 s_dropped);
    }

    /* Write the data file BEFORE updating the index: a crash here leaves an
     * orphan entry file but never an index pointing at missing data. */
    if (!entry_write(s_idx.tail, code, sid)) {
        if (s_lock) xSemaphoreGive(s_lock);
        return false;
    }

    s_idx.tail = (s_idx.tail + 1) % WUHE_BACKUP_FILE_MAX;
    s_idx.count++;

    /* Persist new index. If this fails the entry is orphaned (harmless) but
     * the device must keep scanning, so we don't unwind the RAM state. */
    if (!idx_write()) {
        ESP_LOGE(TAG, "push: idx persist failed (entry orphaned, FIFO RAM view kept)");
        if (s_lock) xSemaphoreGive(s_lock);
        return false;
    }

    if (s_lock) xSemaphoreGive(s_lock);
    return true;
}

bool wuhe_backup_pop(char *code_out, size_t cap, uint16_t *sid_out)
{
    if (!s_mounted) {
        return false;
    }
    if (code_out == NULL || cap == 0) {
        ESP_LOGW(TAG, "pop: rejected null/empty out buffer");
        return false;
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_idx.count == 0) {
        if (s_lock) xSemaphoreGive(s_lock);
        return false;
    }

    if (!entry_read(s_idx.head, code_out, cap, sid_out)) {
        /* The entry file is missing/corrupt — almost certainly the result of a
         * crash between a prior pop's remove() and idx_write(). Skip the dead
         * slot so the FIFO self-heals instead of stalling the drain loop. */
        ESP_LOGE(TAG, "pop: lost entry at slot %" PRIu32 ", skipping", s_idx.head);
        s_idx.head = (s_idx.head + 1) % WUHE_BACKUP_FILE_MAX;
        s_idx.count--;
        (void)idx_write();
        if (s_lock) xSemaphoreGive(s_lock);
        return false;
    }

    char path[64];
    entry_path(s_idx.head, path, sizeof(path));
    (void)remove(path);   /* best-effort; orphan file is harmless if it survives */

    s_idx.head = (s_idx.head + 1) % WUHE_BACKUP_FILE_MAX;
    s_idx.count--;

    if (!idx_write()) {
        ESP_LOGE(TAG, "pop: idx persist failed (RAM view advanced; will self-heal on reboot)");
        /* Don't return false: caller already has the entry in code_out/sid_out. */
    }

    if (s_lock) xSemaphoreGive(s_lock);
    return true;
}

size_t wuhe_backup_count(void)
{
    if (!s_mounted) {
        return 0;
    }

    size_t cnt;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    cnt = (size_t)s_idx.count;
    if (s_lock) xSemaphoreGive(s_lock);
    return cnt;
}
