# AGENTS.md

Compact guidance for OpenCode sessions working in this repo. Every line is something
that is hard to infer from filenames alone.

## Project

ESP-IDF **USB CDC-ACM Host** scanner demo for the "五合云" (WuHeYun) device.
Originally forked from the official Espressif `usb/host_hid` example; the HID
keyboard path was **fully replaced** with a CDC-ACM virtual-serial path on the
`cdc-acm-utf8-barcode` branch (this is now the default code path).

- **Target**: ESP32-S3 (Xtensa). USB-OTG required. Also buildable for S2/P4/H4 per README.
- **ESP-IDF**: v6.0.1 at `/home/kimi/.espressif/v6.0.1/esp-idf`.
- **Scanner**: CX70 (was Newland NLS-FM430-EX), USB CDC-ACM mode,
  VID=0x0218 PID=0x0212 (see `main/scanner_config.h`). Emits **GBK** byte
  streams terminated by ENTER (`\r` or `\n` or `\r\n`); firmware transcodes
  GBK→UTF-8 at dispatch time (see `gbk_utf8.c` / `gbk_table.c`).
- **`main` component subsystems**:
  - `hid_host_example.c` — USB Host Library lifecycle (`usb_lib_task`) +
    `app_main` init sequencing. HID code removed; name kept for git-blame
    continuity.
  - `wuhe_cdc_scan.c` — CDC-ACM scanner task: opens by VID/PID, accumulates
    raw GBK bytes in a 121-byte buffer, dispatches barcodes on ENTER to UI +
    cloud after GBK→UTF-8 transcoding. This is the **only** scanner input path.
  - `gbk_utf8.c` / `gbk_table.c` — GBK→UTF-8 transcoder + ~47 KiB generated
    lookup table (21791 entries). Regenerate with `python3 tools/gen_gbk_table.py`.
  - `screen_display.c` — ILI9341 LCD + LVGL v9 UI.
  - `wuhe_cloud.c` / `wuhe_backup.c` / `wuhe_storage.c` — cloud upload,
    LittleFS offline backup, NVS SID/MNo storage.
  - `scanner_config.h` — **single source of truth** for VID/PID and barcode
    buffer width (`WUHE_BARCODE_MAX_BYTES = 121`, sized for 40 CJK chars in
    UTF-8 + NUL). Bump here to widen every layer at once.

## Build / Flash

```bash
idf.py set-target esp32s3   # only once, or after deleting build/
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # JTAG flash configured
```

- `CMakeLists.txt` enables **`MINIMAL_BUILD ON`** — only `main` and its direct deps are built. Adding a new IDF component dependency requires also using it from `main`.
- Managed components are fetched by the **IDF Component Manager** into `managed_components/` (gitignored) on configure. Versions pinned in `dependencies.lock`.
- `sdkconfig` is gitignored and regenerated from `sdkconfig.defaults` + Kconfig. The only non-default option set there is `CONFIG_USB_HOST_HUBS_SUPPORTED=y`.
- `.clangd` strips `-f*`/`-m*` flags so clangd can index; it reads `build/compile_commands.json`. **A successful build must exist** before IntelliSense/clangd work.

## Critical gotchas

### CDC scanner path (replaces HID keyboard)

The scanner is a **GBK byte pipe with firmware-side GBK→UTF-8 transcoding**.
The HID keyboard path (keycode2ascii, Alt+numpad escape, single-byte ASCII
output) was **fully removed** because USB HID Keyboard Usage Page (0x07) has
no codepoints for CJK characters — Chinese is fundamentally impossible over
HID-keyboard mode.

`wuhe_cdc_scan.c` does NOT:
- Filter bytes by `>= 0x20` (would drop GBK trail bytes 0x40-0xFE).
- Pass raw bytes to LVGL/cloud — `dispatch_barcode()` runs `gbk_to_utf8()`
  first; downstream only ever sees UTF-8.
- Use `cdc_acm_host_data_rx_blocking` (the callback `data_cb` is correct;
  it returns `bool`, takes `const uint8_t *data`).

`data_cb` runs in the CDC driver's own background task (single-threaded,
no re-entrancy). Dispatch to `wuhe_ui_set_barcode_safe` (acquires LVGL
lock internally) and `wuhe_cloud_submit_scan` (FreeRTOS queue) is
thread-safe by design — direct call from `data_cb` is OK.

### `usb_lib_task` NO_CLIENTS break (fragile, do not "fix" without thought)

`usb_lib_task` in `hid_host_example.c` breaks out of its event loop on
`USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS`. This was designed for the HID path
where pressing BOOT uninstalls the HID driver (the only client). With CDC,
the CDC-ACM driver is the client — as long as `cdc_acm_host_install()`
runs before any disconnect event, the loop stays alive. Race window is
tiny (between `usb_host_install()` and `cdc_acm_host_install()`), but if
you see mysterious USB shutdowns on boot, this is the suspect.

### Display is wired in (was orphaned before this branch)

`display_start_up_pagevoid(NULL)` is called from `app_main()` BEFORE
`wuhe_cdc_start()`. The LCD shows the WuHeYun UI at boot — no longer dark.

### Backup migration on width bump

`WUHE_BACKUP_CODE_FIELD_LEN` in `wuhe_backup.c` is now `WUHE_BARCODE_MAX_BYTES`
(121). On-disk `wuhe_backup_entry_t` grew from 43 → 123 bytes. **Existing
LittleFS backup files from older firmware will be misread** on first boot
after upgrade; `wuhe_backup_init`'s read-back failure triggers its
format+remount fallback, erasing the offline backlog. Acceptable for dev;
for production deploy a magic/version field migration.

### `MyFont.c` lives in `main/` (was in managed_components)

`LV_FONT_DECLARE(MyFont)` is referenced throughout `screen_display.c`.
`MyFont.c` was moved out of `managed_components/lvgl__lvgl/src/font/` into
`main/MyFont.c` and registered in `main/CMakeLists.txt` SRCS. **It is now
safe** from `idf.py reconfigure`, `fullclean`, and LVGL version bumps.

If you ever move it back (don't), the old gotcha applies: any re-fetch of
`lvgl/lvgl` would silently destroy local modifications to the managed copy.

### Cross-task LVGL access
LVGL runs in its own FreeRTOS task (`example_lvgl_port_task`) protected by `_lock_t lvgl_api_lock`
in `screen_display.c`. Any LVGL API call from another task/context **must** `_lock_acquire(&lvgl_api_lock)`
/ `_lock_release(&lvgl_api_lock)` around it. The existing display init already does this.

### Cloud upload reliability (SID / queue / backup drain)

The cloud upload path (`wuhe_cloud.c`) is designed for zero silent data loss.
Three invariants that are easy to break by accident:

1. **SID is allocated and persisted at scan time** (`wuhe_storage_sid_next`,
   NVS key `"sid"` in namespace `"wuhe"`), before the upload attempt, and is
   **never rolled back** on failure. Retries reuse the same SID so the cloud
   sees a stable dedup key across replays. Only `idf.py erase-flash` resets it;
   `idf.py flash` alone preserves NVS (see `partitions.csv` line 14-15).

2. **`drain_backup()` has TWO trigger conditions** (OR, not AND):
   - WiFi rising edge (`!was_connected && now_connected`) in `wuhe_cloud_task`
   - Periodic check: online + `wuhe_backup_count() > 0` + `WUHE_DRAIN_BACKOFF_MS`
     (60 s) elapsed since last drain stall (`s_last_drain_fail_ms`)

   The periodic check is **the only** path that drains backlog when WiFi stays
   associated but the HTTP server is unreachable (common field condition). Do
   not remove it — the WiFi-only trigger was the original design and silently
   lost data when only the HTTP layer went down.

3. **Three-tier overflow chain, no silent drops:**
   ```
   scan → s_queue (RAM, 32) → send_pack → cloud
              │ full              │ transport fail ×WUHE_RETRY_MAX
              ▼                   ▼
         LittleFS backup (2000) ←──┘ → drain_backup → cloud
   ```
   The `submit_scan` queue-full branch MUST `wuhe_backup_push(dropped)` — the
   original code dropped silently and lost scans. LittleFS overflow (after
   ~2032 offline scans) is the only remaining loss path.

`send_pack` blocks the cloud task for the full retry window on every failure
(`WUHE_RETRY_MAX` × timeout + exponential backoff). Keep `WUHE_RETRY_MAX`
modest in debug builds. Params live in `wuhe_cloud.h`, some Kconfig-overridable.

## Hardware / pin map (screen_display.c)

ILI9341 on **SPI2_HOST**, 320×240, color format RGB565, BGR element order:

| Signal | GPIO | Signal | GPIO |
|--------|------|--------|------|
| SCLK | 36 | DC | 39 |
| MOSI | 35 | RST | 40 |
| MISO | 37 | CS | 38 |
| BK light | 45 | Touch CS | 15 (unused, touch disabled) |

- **GPIO0 = APP_QUIT_PIN** (the BOOT button). Falling-edge ISR is registered but currently a **no-op stub** — the CDC/cloud/LVGL tasks run indefinitely. Wired for future graceful-shutdown use.
- LCD pixel clock 20 MHz, DMA-backed dual draw buffers (20 lines each).
- Several alternative pinouts are left commented-out in `screen_display.c` (the previous wiring: SCLK18/MOSI19/MISO21/DC5/RST3/CS4/BK2). Don't "clean them up" — they document prior hardware revisions.

## Adding code

- All app source is in `main/`. Register new `.c` files in `main/CMakeLists.txt` `SRCS`.
- `main/CMakeLists.txt` declares `PRIV_REQUIRES esp_timer esp_driver_gpio`. LCD/SPI/USB-HID symbols come transitively via the managed components in `main/idf_component.yml` — add new external components there, not by hand.
- The public UI update functions (`wuhe_ui_set_warehouse`, `wuhe_ui_set_post`, `wuhe_ui_set_barcode`, `wifi_signal_set_rssi`) are the intended integration points for feeding data into the UI. `wuhe_ui_set_barcode_safe` is called from `wuhe_cdc_scan.c`'s `data_cb` on every scan; the others remain uncalled (server-response-driven).
- No tests, no linter, no formatter configured. Verification = `idf.py build` exits 0 + flash + serial monitor.

## Dev environment

- VS Code ESP-IDF extension; config in `.vscode/` assumes ESP-IDF v6.0.1 at the path above and JTAG on `/dev/ttyACM0`.
- Devcontainer (`.devcontainer/`) uses `espressif/idf` image with `--privileged` (needed for USB passthrough); not used for local builds.
