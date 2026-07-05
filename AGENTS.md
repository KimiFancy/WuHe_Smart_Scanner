# AGENTS.md

Compact guidance for OpenCode sessions working in this repo. Every line is something
that is hard to infer from filenames alone.

## Project

ESP-IDF **USB HID Host** demo (forked from the official Espressif `usb/host_hid` example)
extended with an **LVGL + ILI9341 LCD** UI for a "五合云" (WuHeYun) device.

- **Target**: ESP32-S3 (Xtensa). USB-OTG required. Also buildable for S2/P4/H4 per README.
- **ESP-IDF**: v6.0.1 at `/home/kimi/.espressif/v6.0.1/esp-idf`.
- **Two independent subsystems share one `main` component**:
  - `hid_host_example.c` — USB HID host (keyboard/mouse), prints reports to serial. This is the **only** subsystem wired into `app_main()`.
  - `screen_display.c` — ILI9341 LCD + LVGL v9 UI. See "Display is orphaned" below.

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

### Display code is orphaned (not a bug to "fix" blindly)
`display_start_up_pagevoid()` in `screen_display.c` is **never called** from `app_main()`.
Out of the box, **only the HID serial output runs; the LCD stays dark.**
If asked to "make the screen work", call `display_start_up_pagevoid(NULL)` from `app_main()`
(after the HID driver install, or in its own task) — do not rewrite the function, it is complete.
Confirm intent before wiring it, since the HID task blocks on its event queue.

### `MyFont` is a hand-edited file inside a managed component
`LV_FONT_DECLARE(MyFont)` is referenced throughout `screen_display.c`, but `MyFont.c` lives at
`managed_components/lvgl__lvgl/src/font/MyFont.c` — a **local modification to a fetched dependency**.
It will be **silently destroyed** by any of:
- `idf.py reconfigure` after deleting `managed_components/`
- a version bump of `lvgl/lvgl` in `main/idf_component.yml`
- `idf.py fullclean` followed by configure

If touching fonts or upgrading LVGL, **back up `MyFont.c` first** and restore it after re-fetch,
or (better) move it into `main/` and register it in `main/CMakeLists.txt`.

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

- **GPIO0 = APP_QUIT_PIN** (the BOOT button). Falling edge uninstalls the HID driver — pressing BOOT during operation tears down USB.
- LCD pixel clock 20 MHz, DMA-backed dual draw buffers (20 lines each).
- Several alternative pinouts are left commented-out in `screen_display.c` (the previous wiring: SCLK18/MOSI19/MISO21/DC5/RST3/CS4/BK2). Don't "clean them up" — they document prior hardware revisions.

## Adding code

- All app source is in `main/`. Register new `.c` files in `main/CMakeLists.txt` `SRCS`.
- `main/CMakeLists.txt` declares `PRIV_REQUIRES esp_timer esp_driver_gpio`. LCD/SPI/USB-HID symbols come transitively via the managed components in `main/idf_component.yml` — add new external components there, not by hand.
- The public UI update functions (`wuhe_ui_set_warehouse`, `wuhe_ui_set_post`, `wuhe_ui_set_barcode`, `wifi_signal_set_rssi`) are the intended integration points for feeding data into the UI from the HID side. They are currently uncalled.
- No tests, no linter, no formatter configured. Verification = `idf.py build` exits 0 + flash + serial monitor.

## Dev environment

- VS Code ESP-IDF extension; config in `.vscode/` assumes ESP-IDF v6.0.1 at the path above and JTAG on `/dev/ttyACM0`.
- Devcontainer (`.devcontainer/`) uses `espressif/idf` image with `--privileged` (needed for USB passthrough); not used for local builds.
