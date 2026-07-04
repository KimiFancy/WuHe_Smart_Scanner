#include "wuhe_storage.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#define NVS_NS          "wuhe"
#define NVS_KEY_SID     "sid"
#define NVS_KEY_MNO     "mno"
#define SID_MAX         65535
#define MNO_PLACEHOLDER "XGWHY0000000"

static const char *TAG = "wuhe.nvs";

/* ---- SID ---------------------------------------------------------------- */

uint16_t wuhe_storage_sid_next(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        goto fallback;
    }

    uint16_t cur = 0;
    err = nvs_get_u16(h, NVS_KEY_SID, &cur);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_get_u16(sid) failed: %s", esp_err_to_name(err));
        nvs_close(h);
        goto fallback;
    }

    /* Wrap: 65535 → 1 so we never return 0 */
    uint16_t next = (cur == SID_MAX) ? 1 : cur + 1;

    err = nvs_set_u16(h, NVS_KEY_SID, next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u16(sid) failed: %s", esp_err_to_name(err));
        nvs_close(h);
        goto fallback;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(sid) failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return next;

fallback:
    /* Static RAM counter — device still works without NVS */
    static uint16_t ram_sid = 0;
    ram_sid = (ram_sid == SID_MAX) ? 1 : ram_sid + 1;
    ESP_LOGE(TAG, "NVS unavailable, using RAM fallback SID=%u", ram_sid);
    return ram_sid;
}

/* ---- MNo ---------------------------------------------------------------- */

void wuhe_storage_mno_get(char *out, size_t len)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        strlcpy(out, MNO_PLACEHOLDER, len);
        return;
    }

    size_t needed = len;
    err = nvs_get_str(h, NVS_KEY_MNO, out, &needed);
    nvs_close(h);

    if (err != ESP_OK) {
        strlcpy(out, MNO_PLACEHOLDER, len);
    }
}

bool wuhe_storage_mno_set(const char *mno)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(mno) failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(h, NVS_KEY_MNO, mno);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(mno) failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(mno) failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return (err == ESP_OK);
}
