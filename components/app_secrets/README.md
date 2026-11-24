# app_secrets

Stores the device-exclusive Gemini API key and an optional model override in the
application's own `nvs` partition (namespace `vox_secret`), separate from Vibe
Mode's shared information. Depends on ESP-Iris (RPC), NVS and Mbed TLS zeroization.

## Provisioning RPC (service `0x5658`)

| Method | Payload | Response |
|---|---|---|
| 1 set_api_key | key, 30..128 bytes of `A-Z a-z 0-9 . _ -` | `ok` / `error` |
| 2 clear_api_key | — | `ok` / `error` |
| 3 status | — | `{"api_key":true|false,"model":"…"}` |
| 4 set_model | `gemini-3.8-live`, `gemini-3.1-flash-live-preview`, or empty | `ok` / `error` |

There is deliberately **no** method that returns the key. Use
`tools/provision_key.py` over USB.

## Contract

- `app_secrets_init(cb, ctx)`: once at startup after `nvs_flash_init()` and
  ESP-Iris startup. `cb` runs on the ESP-Iris service task after a change and
  must not block.
- `app_secrets_load_api_key(buf, cap)`: `cap >= APP_SECRETS_API_KEY_CAPACITY`.
  Returns `ESP_ERR_NVS_NOT_FOUND` when unprovisioned, `ESP_ERR_INVALID_STATE` for
  a stored value that fails validation. The buffer is cleared on every error;
  callers zeroize it after use and never log it.
- NVS access is synchronous and thread-safe; call from task context.
- Never erases NVS on read failures and never writes defaults.

## Security notes

The key is stored unencrypted in flash: NVS encryption needs eFuse keys (HMAC)
or flash encryption, both irreversible and not enabled without the owner's
explicit authorization. Mitigate with a restricted, quota-limited, revocable key.
See `docs/DESIGN.md` §4.
