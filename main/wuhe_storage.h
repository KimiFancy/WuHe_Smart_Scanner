#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Allocate next SID (1..65535, wraps 65535→1).
 * Persisted in NVS key "sid" under namespace "wuhe".
 * Falls back to static RAM counter on NVS failure.
 */
uint16_t wuhe_storage_sid_next(void);

/**
 * Read MNo into `out` (null-terminated, respects `len`).
 * Falls back to placeholder "XGWHY0000000" on missing/error.
 */
void wuhe_storage_mno_get(char *out, size_t len);

/**
 * Persist MNo string into NVS. Returns true on success.
 */
bool wuhe_storage_mno_set(const char *mno);
