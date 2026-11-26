# app_rol_web

Role-mode phone page (1.6.0, Kconfig `VOX_ROLE_PHONE_PAGE`). It lets each player read **only their
own secrets** on their own phone, without anyone having to look away from the screen.

## How it works

- The server exists only while role mode is on. The session starts it when Gemini calls
  `modo_rol(activar=true)` and stops it when role mode ends or the conversation closes.
- It runs on the **home Wi-Fi** (the station interface VOX already uses for Gemini), port 80, at the
  address shown on the status line (`MÓVIL: 192.168.1.36`).
  - No access point of its own. An AP would have to share the station's channel, uses more RAM, and
    phones joined to it lose Internet.
- Endpoints:
  - `GET /`: the page (`rol_page.html`, embedded).
  - `POST /join`: body = the player's name. Returns `{"ok":true,"token":…,"nombre":…}`, or
    `{"ok":false,"error":…}` when the name is already taken or there are 6 players already.
  - `GET /secret?t=TOKEN`: `{"ok":true,"nombre":…,"secreto":…|null,"seq":n}`; `ok:false` for an
    unknown token, which happens after a new game.
- The page keeps the token in `localStorage`. It polls every 2 s. A new secret makes the phone
  vibrate, and the text stays blurred until the player **holds** the button.
- `app_rol_web_deliver(player, text)`: the name match ignores case and Spanish accents
  (`rol_players_normalize`). It returns false if nobody joined with that name; the session then
  falls back to the on-screen card.

## Contract

- Every public function is called from the VOX session task only. HTTP handlers run on the server
  task. The player table is shared under a mutex.
- The player table (6 players × name, token and a secret of up to 640 bytes) lives in PSRAM while
  the server runs. It is zeroed and freed on stop. The server task stack is 6 KiB, internal.
- Tokens are 128-bit, from the hardware RNG (`esp_fill_random`).
- Plain HTTP on the local network: the secrets are game text, not credentials. Nothing is stored in
  flash and nothing leaves the LAN.
- `rol_players.c` is pure C and is tested on the PC (`tools/host_tests/run.ps1`).
