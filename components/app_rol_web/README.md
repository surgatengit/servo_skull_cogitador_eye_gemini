# app_rol_web

Role-mode phone page (1.6.0, Kconfig `VOX_ROLE_PHONE_PAGE`). Each player sees **only their own
secrets and character sheet** on their own phone, without anyone having to look away from the
screen. The same page offers the saved game for download and upload.

## How it works

- The server exists only while role mode is on. The session starts it when Gemini calls
  `modo_rol(activar=true)` (or `cargar_partida`) and stops it when role mode ends or the
  conversation closes.
- It runs on the **home Wi-Fi** (the station interface VOX already uses for Gemini), port 80, at the
  address shown on the status line (`MÓVIL: 192.168.1.36`).
  - No access point of its own. An AP would have to share the station's channel, uses more RAM, and
    phones joined to it lose Internet.
- Endpoints:
  - `GET /`: the page (`rol_page.html`, embedded).
  - `POST /join`: body = the player's name. Returns `{"ok":true,"token":…,"nombre":…}`, or
    `{"ok":false,"error":…}` (bad name, or 6 players already).
  - `GET /me?t=TOKEN`: `{"ok":true,"nombre","secreto"|null,"seq","leido","ficha"|null,"ficha_seq"}`;
    `ok:false` for an unknown token (a new game, or the name moved to another phone).
  - `POST /seen?t=TOKEN&s=SEQ`: the player revealed secret number SEQ.
  - `GET /partida`: the saved game as `partida_vox.json` (404 if none).
  - `POST /partida`: upload a saved game (JSON, < 16 KiB, `"vox_partida":1`); it replaces the
    one on VOX.
- Players:
  - A name gets an entry when a phone joins, or earlier when the master gives it a secret or a sheet
    (or a game is loaded). Such a **reserved** entry is claimed by the first phone that joins with
    the name, so late players get what was waiting for them.
  - Joining with a name already on a phone **moves** the player to the new phone (lost or changed
    phone); the old one gets `ok:false`. The master is told («… ha vuelto a entrar desde otro
    móvil»), so swapping names on purpose is visible.
  - Name matching ignores case and Spanish accents (`rol_players_normalize`).
- Notices: every join adds a sentence (`app_rol_web_take_notices`). The session sends them to
  the master as a text turn when the model is quiet, so it can greet each player in the lobby.
- The page keeps the token in `localStorage` and polls `/me` every 2 s. A new secret or sheet
  change makes the phone vibrate. The secret stays blurred until the player **holds** the button;
  the first reveal is reported to `/seen`, which `estado_jugadores` turns into
  `secreto_sin_leer`.

## Saved game

- `rol_game.c` (pure C + cJSON, host-tested) builds and reads one document:
  `{"vox_partida":1,"titulo","juego","resumen","dados":{…},"jugadores":[{"nombre","ficha","secreto"}]}`.
- `app_rol_web_store_game` keeps it in the application NVS (`vox_game`/`partida`, one game, up to
  16 KiB). If the old and new copies do not fit together, the old one is erased first.
- `app_rol_web_restore_game` puts the saved players into the table as reserved entries: sheets
  always, secrets only to continue the same story. Players already on a phone keep it.

## Contract

- Every public function is called from the VOX session task only. HTTP handlers run on the server
  task. The player table and the notices are shared under a mutex.
- The player table (6 players × name, token, a secret of up to 640 bytes and a sheet of up to
  1 KiB) lives in PSRAM while the server runs. It is zeroed and freed on stop. The server task
  stack is 6 KiB, internal; an upload is read into a 16 KiB PSRAM buffer.
- Tokens are 128-bit, from the hardware RNG (`esp_fill_random`).
- Plain HTTP on the local network: secrets and sheets are game text, not credentials. Anyone on the
  home Wi-Fi can open the page, download the saved game or upload another one. Nothing leaves the
  LAN.
- `rol_players.c` and `rol_game.c` are tested on the PC (`tools/host_tests/run.ps1`).
