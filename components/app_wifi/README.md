# app_wifi

Wi-Fi station for VOX M-01. Reads the Vibe Mode shared credentials through
`app_config_load_wifi()`, applies them with `WIFI_STORAGE_RAM` (no second
persistent copy) and keeps the link up with exponential backoff (1 s → 30 s
with jitter). Power save is disabled for low-latency audio streaming.
Depends on `app_config`, esp_wifi/esp_netif/esp_event/esp_timer and Mbed TLS zeroization.

## Contract

- `app_wifi_start(cb, ctx)`: singleton, once, after `nvs_flash_init()`.
  Creates the default STA netif (shared with ESP-Iris TCP) and the default
  event loop if missing.
- `cb(state, ctx)` runs on the default event loop task and must not block.
- States: `UNCONFIGURED` (no shared credentials: ask the user to configure
  Vibe Mode), `CONNECTING`, `CONNECTED`, `FAILED` (driver/config error).
- Missing configuration is not an error: the UI and Recovery stay available.
- Credentials are zeroized after `esp_wifi_set_config()`; nothing is logged
  except the SSID-less status and the obtained IP.
