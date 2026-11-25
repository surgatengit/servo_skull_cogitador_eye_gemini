# VOX M-01 — Instalación, restauración y seguridad

Estado: **v1.0 (2026-11-24)**. Verificado sobre el código del espacio de trabajo
(`esp-mosaico-utils` en `f58f4f9`, Vibe Mode empaquetado 0.4.1, ABI 2, layout 5).
**Nada de esto se ha ejecutado todavía en el dispositivo.**

Regla general: solo `python mosaico.py` toca el dispositivo. Nunca `idf.py flash`,
`esptool` en escritura, borrado completo de la flash ni cambios de particiones o eFuses.

> **Versión 1.1.0 («Hola ESP»):** cambia la tabla de la región de aplicación. `main_app` pasa a
> 12 224 K y se añade `model` (data/spiffs, 1 MiB en `0xE00000`) con `srmodels.bin` (~418 KB).
> El paquete tiene **cuatro** componentes: tabla, aplicación, `ui_apps` y `model`. Siguen intactos el
> prefijo inmutable, `nvs` (clave y override de modelo) y la NAND. Para volver a ESP-Claw
> (Mis Ideas → 100000), Vibe Mode reescribe su propia tabla, y `model` queda dentro del `main_app`
> de ESP-Claw. Requiere `system-update`; `app-update` no sirve porque cambia la tabla.

## 1. Qué escribe `iris system-update`

El paquete `.irisfw` lo genera `mosaico-tools/tools/prepare_system_update.py` y contiene
exactamente **tres componentes**:

| # | Componente | Destino | Contenido |
|---|---|---|---|
| 1 | `partition_table` | `0x8000` (4 KiB) | Tabla de VOX (abajo) |
| 2 | `application` | `main_app` `0x210000` | `servocraneo_gemini_direct.bin` (≈2,0 MB) |
| 3 | `data` `ui_apps` | `0xF00000` (1 MiB) | Paquete GSP del ojo (≈18 KB) |

El preparador **rechaza** por diseño cualquier imagen dirigida al prefijo inmutable o a
particiones NVS (`nvs`, `nvs_keys`). Vibe Mode vuelve a validar el manifiesto en el dispositivo
(ABI, layout, versión mínima 0.2.0, chip y tamaño de flash).

**No se escriben:** bootloader (`0x2000`), `otadata`, `phy_init`, `sysmeta`, `vibe_mode`,
`coredump`, `nvs` (`0x200000`) ni la memoria NAND.

### Tabla de particiones: base (`recover`) frente a VOX

| Partición | Base | VOX | ¿Cambia? |
|---|---|---|---|
| otadata, phy_init, sysmeta, vibe_mode, coredump | prefijo inmutable | idéntico | No (lo impone el contrato) |
| nvs `0x200000` | 64 K | 64 K | No |
| main_app `0x210000` | 14 272 K | 13 248 K | Se reduce 1 MiB |
| ui_apps `0xF00000` | — | 1 M | Nueva (último MiB del antiguo `main_app`) |

### ¿Sustituye a ESP-Claw?

- En el firmware, «ESP-Claw» solo aparece como dominio de los servicios de Ideas de Vibe Mode
  (`mosaico-vibe-rec-ver.esp-claw.com`: recomendaciones y OTA estable). Esos servicios forman
  parte de **Vibe Mode**, que `system-update` **no toca**.
- Si ESP-Claw es **la aplicación que arranca hoy** (en `main_app`), **sí la sustituye**:
  VOX ocupa `main_app` y la tabla pasa a la de VOX. La aplicación queda inactiva,
  pero su copia descargada en la NAND (`/nand/ideas/<código>/current/`) no se borra.
- La primera prueba física empieza con `iris device-status` (solo lectura) para confirmar
  qué firmware y qué versión de Vibe Mode hay realmente instalados.

## 2. Qué se conserva y qué se pierde

| Dato | Ubicación | Con `system-update` de VOX | Al volver a ESP-Claw desde Mis Ideas | Con `recover` |
|---|---|---|---|---|
| Vibe Mode y su versión | `vibe_mode` | Se conserva | Se conserva | Se reescribe con el paquete revisado 0.4.1 |
| Bootloader | `0x2000` | Se conserva | Se conserva | Se reescribe |
| Wi-Fi e información compartida de Vibe | `sysmeta` | Se conserva | Se conserva | No se escribe |
| Identidad del dispositivo y emparejamientos | `sysmeta` / NVS protegida | Se conserva | Se conserva | No se escribe |
| Ideas descargadas (Mis Ideas) | NAND | Se conserva | Se conserva | No se escribe |
| Datos de ESP-Claw en NVS de app | `nvs` `0x200000` | Se conservan (VOX usa su propio espacio de nombres `vox_secret`) | Siguen ahí | No se escribe |
| Clave de Gemini de VOX | `nvs` `vox_secret` | — | **Sigue ahí si no se borra antes** | No se escribe |
| Binario de ESP-Claw en NOR | `main_app` | **Se sobrescribe** | Se reinstala desde la NAND | Ver nota |
| Volcados de memoria previos | `coredump` | Se conservan; `mosaico.py` los guarda antes de escribir | — | — |

Nota: `recover` escribe `bootloader`, `partition-table`, `otadata` y `vibe_mode` desde
`firmware/recovery/`, y arranca en Vibe Mode. La aplicación se instala después.

### Estado observado antes de instalar (2026-11-24, solo lectura)

Lectura con `iris list` y `iris device-status` por USB, sin escrituras ni reinicios:

| Campo | Valor |
|---|---|
| Device ID | `4553502d4952495301001c2904d11390` (serie USB `1C2904D11390`) |
| Modo | `normal` (aplicación en `main_app`), ESP-Iris 0.2, contrato `esp-mosaico/v2`, ABI 2, layout `mosaico-retained-test-2m-v2` |
| Aplicación | **Mosaico-Claw 0.8.0** (Idea **100000**, «ESP-Claw»); SHA-256 `ce9f5d16a40ff4040306add4d78f3f83d6d8f99321389b15407e2cc570a4d323` |
| ESP-IDF de la app | `v6.1-dev-8142-g188e3e55bb2` (la misma base que Vibe Mode 0.4.1 empaquetado) |
| Salud | `crash_count 0`, sin bucle de bloqueos ni recuperación pendiente |
| Memoria | PSRAM total 26,8 MB (22,1 MB libres); RAM interna 346 KB (110 KB libres, mínimo 64 KB) |
| Datos de ESP-Claw | En la **NAND**: `/nand/router_rules`, `/nand/scheduler`, `/nand/memory`. `system-update` no los toca |
| Wi-Fi | Conectado con las credenciales de Vibe Mode (IP privada, región ES) |
| Versión de Vibe Mode | No la informa la aplicación. `system-update` exige al menos 0.2.0 y la comprueba antes de escribir; se ve en Vibe Mode → Acerca de |

Conclusión: ESP-Claw es la aplicación que arranca hoy y **será sustituida** en `main_app`.
Su código Ideas es 100000; sus datos en la NAND, el Wi-Fi y Vibe Mode se conservan.

### Instalación de VOX 1.0.0 (2026-11-24, autorizada)

| Evidencia | Valor |
|---|---|
| Operación | `iris system-update`, operation `17e90e9e8ac14518b4dcb5b58bf26d20`, `status: succeeded`, `validated: true`, `healthy: true` |
| Componentes escritos | Tabla `0x8000` (sha `3c26b245…`), aplicación `0x210000` (2 040 192 B, sha `b92d1c0a…`), `ui_apps` `0xF00000` (17 744 B) |
| Bootloader | Sin cambios (`46ebaa95…` antes y después) |
| Boot IDs | ESP-Claw `5011400773332120379` → Vibe Mode (escritor) `7763685529854875385` → VOX `14452938182979607246` |
| Estado en vivo | `normal`, `servocraneo_gemini_direct` 1.0.0, firmware sha `d82574ec…` (igual al ELF), `crash_count 0` |
| Arranque | `app_secrets` (clave ausente), GSP 480×480 en marcha, audio 48/16/24 kHz, sesión con `gemini-3.8-live`, Wi-Fi conectado en 8,4 s |
| Memoria | RAM interna libre 101 KB (mínimo 52 KB); PSRAM 29,8 MB libres de 31,6 MB |
| Pantalla | `iris screenshot`: ojo cian en reposo y parpadeos. Una captura muestra un desgarro horizontal del iris en movimiento; queda por confirmar en la pantalla física |

### Primera prueba de voz (2026-11-24)

- La clave se provisionó por RPC (`{"api_key":true}`). No aparece ninguna cadena `AIza…` en los logs
  del dispositivo, de `mosaico.py` ni del Gateway.
- **Gemini Direct funciona de extremo a extremo con `gemini-3.8-live`:** `setupComplete` en unos 300 ms,
  cuatro turnos completos ESCUCHANDO → PROCESANDO → HABLANDO → ESCUCHANDO, cierre y reapertura por toque.
- Un corte de transporte (código 0) se recuperó solo con `sessionResumption` en 3 s.
- **Fallo encontrado:** la RAM interna mínima bajó de 52 KB a **11,5 KB** con la sesión activa, y la
  pantalla perdió fotogramas (289 × `spi transmit (queue) color failed` / `TE ticket … ESP_ERR_NO_MEM`).
- **Corrección (pendiente de instalar con `iris app-update`):** TLS y búferes de Wi-Fi/LWIP en PSRAM
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`), búferes del WebSocket
  de 16 a 4 KB, y búferes de audio y de envío en PSRAM. La tabla de particiones no cambia.

### Pruebas 2–4 (2026-11-24): memoria, diagnóstico y modelo

| Prueba | Firmware / modelo | Resultado |
|---|---|---|
| 2 | Corrección de memoria / `gemini-3.8-live` | Pantalla arreglada: 0 errores, RAM interna mínima **40,6 KB** (antes 11,5 KB). Pero 0 turnos: Gemini no transcribe |
| 3 | + diagnóstico / `gemini-3.8-live` | Micrófono correcto (25 tramas/s, picos −3…−11 dBFS hablando, −42 dBFS en silencio); **734 tramas enviadas sin fallos**; el servidor solo devuelve 3 mensajes iniciales: **ni transcripción ni audio** |
| 4 | Igual / **`gemini-3.1-flash-live-preview`** (RPC `set_model` + `iris restart`) | **Funciona:** 6 turnos completos, 5 transcripciones, 326 fragmentos de audio, una reanudación automática tras un corte, 7 envíos fallidos de 1 880 |

Conclusiones:

- **`gemini-3.8-live` no procesa el audio por WebSocket directo** (coincide con los informes del foro de Google).
  El dispositivo usa ahora `gemini-3.1-flash-live-preview` mediante el override guardado en NVS.
  El paso automático al modelo alternativo no se activó porque 3.8 completa `setupComplete` y luego calla;
  solo se detectaban fallos de conexión.
- **El micrófono satura:** picos de 0 dBFS al hablar y por el eco del altavoz mientras VOX habla.
  Bajar la ganancia de 30 dB.
- Hay cortes de transporte esporádicos (código 0) que se recuperan con `sessionResumption`.

### Prueba 5 (2026-11-24): versión con 3.1 por defecto, 24 dB y vigilante

- Instalación por `system-update`: validada, sana, firmware `d5c4c7b3…`; la clave y el override se conservan.
- **4 turnos completos** con `gemini-3.1-flash-live-preview`, 0 errores de pantalla, sin bloqueos.
- Micrófono a 24 dB: voz con picos de unos **−12 dBFS** (antes −3/0 dBFS). Los 0 dBFS restantes son
  eco del altavoz mientras VOX habla, con el micrófono silenciado (no se envía).
- 2 × `websocket_client: Could not lock ws-client within 20 timeout`: el `audioStreamEnd` coincide con la
  llegada del audio del modelo y se descarta. Inofensivo; pendiente de mejora menor.

### Prueba 6 (2026-11-24): versión 1.1.0 con «Hola ESP»

- System Update con **cuatro componentes**, aceptado por Vibe Mode
  (`data image and readback verified: label=model`). El modelo se carga (`Successfully load srmodels`)
  y escucha con 512 muestras por trama a 16 kHz.
- **2 activaciones por voz** correctas; sesión lista en ~0,6 s; pre-roll de 560–600 ms; 6 turnos.
  El regreso al reposo a los 30 s funcionó; el cierre por toque quedó registrado
  (`tap in state 2`) y la escucha local se rearmó.
- **Fallo:** 29 fotogramas fallidos (`spi transmit (queue) color failed`, `ESP_ERR_NO_MEM`) solo
  entre los 149 y 161 s, con el detector recreado justo tras cerrar la primera sesión.
  Causa: el BSP fija `psram_dma_direct = false` y transacciones de 7 680 B (8 líneas) con cola de 10,
  así que cada trozo pide un búfer intermedio de RAM interna DMA (`spicommon_dma_setup_priv_buffer`).
  Con la RAM troceada tras la sesión no hay bloques contiguos.
- **Mitigación en la app (pendiente de instalar):** brillo cuantizado (8 niveles, ≤10 Hz), espera
  de 1,5 s antes de crear el detector y diagnóstico del bloque interno DMA contiguo más grande.
  **Recomendación aguas arriba:** que los responsables del BSP evalúen `psram_dma_direct = true`
  para el CO5300 (elimina los búferes intermedios a cambio de algo de velocidad).

### Prueba 7 (2026-11-24): 15 s, brillo cuantizado y diagnóstico de memoria

| Momento | RAM DMA interna libre | Bloque contiguo mayor | Pantalla |
|---|---|---|---|
| Arranque, reposo con detector | 39 KB | 8 192 B | bien (al límite) |
| Conversación (detector destruido) | 27,5 KB | 8–12 KB | bien |
| Reposo tras una sesión, detector recreado | 40 KB | **5 888 B** | **congelada** (49 fallos) |

- Cada transacción del panel (7 680 B) necesita un bloque interno DMA contiguo; el detector
  recreado tras la sesión lo impide. El usuario lo vio así: «al parar con dos toques se queda la
  animación bloqueada».
- La vuelta a reposo a los 15 s funcionó. **El vigilante de modelo mudo saltó en falso** (100 tramas,
  4 s de voz antes de la primera transcripción) y cambió a `gemini-3.8-live`, que **sí respondió**
  esta vez (también se ve en el panel de Google AI Studio).
- Errores de la API en Google AI Studio: todos `409 Conflict`, coincidiendo con reconexiones rápidas
  con *handle* de reanudación mientras el servidor aún mantenía la sesión anterior.

Correcciones (pendientes de instalar): `SPIRAM_MALLOC_ALWAYSINTERNAL` 16 → 4 KB y
`SPIRAM_MALLOC_RESERVE_INTERNAL` 32 → 64 KB; el detector se devuelve y reintenta cada 3 s si deja el
bloque DMA mayor por debajo de 8 KB; vigilante a 250 tramas (10 s); ante un 409 se descarta el *handle*
y el siguiente intento abre una sesión nueva.

### Prueba 8 (2026-11-24): reserva DMA de 64 KB y malloc > 4 KB en PSRAM

- **0 errores de pantalla** en 4 activaciones por «Hola ESP», cierres con toques y vueltas a reposo.
- Bloque DMA interno contiguo mayor: **12 800 B** en reposo con detector (antes 8 192 → 5 888 B tras una
  sesión) y 11–12 KB en conversación. La salvaguarda del detector no llegó a activarse.
- Vuelta a reposo a los 15 s correcta; ningún falso cambio de modelo con el umbral de 250 tramas.
- Carrera detectada en los registros de toques: tras interrumpir, el estado volvía a HABLANDO en ~10 ms
  (un fragmento de audio que estaba esperando hueco en el búfer de reproducción) y hacía falta un
  segundo toque. Corregido después: si el turno se interrumpió mientras ese fragmento esperaba, se vacía
  el búfer y no se anuncia un turno nuevo.

### Prueba 9 (2026-11-24): interrupción con un solo toque

- Dos interrupciones con **un toque** (`tap in state 4` → `4 -> 2`, sin volver a HABLANDO) y un
  cierre con el segundo toque. Activación por «Hola ESP» correcta.
- 0 errores de pantalla. El bloque DMA interno contiguo mínimo observado fue de 8 192 B (por encima
  de los 7 680 B por transacción; margen ajustado, vigilado por el diagnóstico).
- **«Hola ESP» (1.1.0) queda validado en el hardware.** Etiqueta Git `v1.1.0` (versión estable).

### Prueba 10: 1.2.0 con Google Search

Rama `feat/google-search`; diseño en [GOOGLE_SEARCH.md](GOOGLE_SEARCH.md). Instalación con
`iris system-update`, como siempre. No cambian ni las particiones ni la clave.

**Instalación (2026-11-25, autorizada por el propietario):**

- Antes: `app_version=1.1.0`, firmware `386d32ec304e…`, `crash_count=0`.
- `iris system-update` (commit `5495f7e`) correcto en 37 s. El paquete escribió la tabla de
  particiones, la app (2 512 688 B), `ui_apps` y `model`.
- Después:
  - mismo Device ID, `firmware_mode=normal`, `app_version=1.2.0`;
  - Boot ID nuevo, `15367548457134334378`;
  - firmware `84d60693d603…`, `crash_count=0`.
- Logs de arranque:
  - `VOX M-01 ready (model gemini-3.1-flash-live-preview, fallback gemini-3.8-live, search on)`;
  - `app_wake: listening for "Hola ESP"`;
  - en reposo, `diag st=0 … largest=12288`, igual que en 1.1.0.

**Primera prueba de voz:** el propietario notó VOX **muy lento**. Los logs de esa conversación se
perdieron (`log_dropped_bytes`: el búfer del dispositivo no los retuvo) y una captura en directo
posterior no registró ninguna conversación de 1.2.0, así que no hay medida de latencia.
Causa probable: la frase añadida a la personalidad («busca ante hechos actuales y cita la fuente»)
empujaba a buscar y a alargar las respuestas.

**Búsqueda discreta (2026-11-25, autorizada; commit `10a6eaf`):**

- Cambios: VOX responde de inmediato, busca solo si se le pide o ante un dato reciente que desconoce,
  y no cita fuentes ni muestra nada en pantalla (`VOX_SHOW_SEARCH_SOURCES=n`).
- `iris system-update` correcto en 42 s.
- Después:
  - mismo Device ID, `firmware_mode=normal`, `app_version=1.2.0`;
  - Boot ID `6174699672841644746`, firmware `66658c0065a5…`, `crash_count=0`.
- Logs de arranque: `search on`, «Hola ESP» escuchando y `largest=12288` en reposo.

**Prueba de voz (2026-11-25): FALLA. VOX no llega a conectar.** Captura en directo:

- Cada `setup` con `tools:[{"googleSearch":{}}]` recibe `closed (close code 1011)` unos 150 ms
  después.
- Pasa con `gemini-3.1-flash-live-preview` (2 intentos) y con `gemini-3.8-live` (3 intentos).
  Después aparece `Gemini Live unavailable (code 1011)` y el ojo violeta «ERROR DE CONEXIÓN».
- Sin `tools`, el mismo `setup` funcionaba en 1.1.0.
- El cliente no registraba el motivo de cierre.
- La primera prueba «lenta» fue con toda seguridad este mismo fallo: nunca hubo una sesión 1.2.0
  con búsqueda.

**Causa (2026-11-25, `tools/test_live_setup.py` ejecutado por el propietario):** el servidor
responde que se ha **excedido la cuota** con los dos modelos. El propietario usa el **plan
gratuito**.

**Decisión del propietario:** la búsqueda queda **opcional y desactivada por defecto**
(`VOX_GOOGLE_SEARCH=n`). Para activarla hay que recompilar con `VOX_GOOGLE_SEARCH=y` y tener cuota
disponible.

**Corrección:**

- `VOX_GOOGLE_SEARCH` desactivado por defecto: VOX conecta como en 1.1.0 y conserva el resto de 1.2.0.
- El cliente registra ahora `server close reason: …` en las tramas de cierre.
- `tools/test_live_setup.py`: el propietario lo ejecuta en el PC con su clave para ver qué
  variante del `setup` acepta Google y con qué motivo rechaza las demás.

**Instalación sin búsqueda (2026-11-25, autorizada; commit `dae739e`):**

- `iris system-update` correcto en 49 s.
- Después:
  - mismo Device ID, `app_version=1.2.0`;
  - Boot ID `5146913244462897174`, firmware `42ff0083c79c…`, `crash_count=0`.
- Logs de arranque: `search off`, «Hola ESP» escuchando y `largest=12288` en reposo.
- Prueba de voz (2026-11-25): **VOX habla y funciona perfectamente**, según el propietario.
  **1.2.0 con la búsqueda desactivada queda validada en el hardware.**

Pendiente de la búsqueda discreta, si se activa con cuota disponible:

- una pregunta sencilla, que debe responder al instante;
- «¿Cuál es la última película de Nolan?»;
- «Busca…»;
- el toque para interrumpir y el reposo a los 15 s.

1. Arranque: `device-status` con `app_version=1.2.0` y `crash_count=0`. En los logs aparece
   `VOX M-01 ready (… search on)` y `diag st=0 … largest=` ≥ 8192.
2. «Hola ESP» → `connected; setup sent (model gemini-3.1-flash-live-preview, google search)` →
   `session ready`. **Si no llega `session ready`, el servidor rechaza el campo `tools`**: se anota y
   se recompila con `VOX_GOOGLE_SEARCH=n`.
3. Pregunta sin búsqueda («¿Cuánto es siete por ocho?»). Debe comportarse como en 1.1.0, sin línea
   de fuentes.
4. Pregunta con búsqueda («¿Qué tiempo hará mañana en Madrid?»):
   - ámbar mientras busca y verde con voz, que menciona la fuente;
   - en la línea de estado, «GOOGLE: …» y después «FUENTE 1/N: …», rotando cada 3 s;
   - logs: `grounded answer: …`, `search{q=… grounded=…}` y `turn usage: … tool=…`.
5. Pregunta larga con búsqueda: no debe volver al reposo mientras busca (`pending=1` en `diag`).
   Tras terminar de hablar, a los 15 s vuelve al cian.
6. Toque mientras habla una respuesta con búsqueda: se interrumpe y vuelve a rojo, como en la
   prueba 9. Otro toque cierra.
7. Memoria: `largest=` ≥ 8192 durante las respuestas con búsqueda y ningún `Display render failed`.
8. Modelo alternativo (opcional): repetir el punto 4 con `gemini-3.8-live`.

### Prueba 11 (pendiente de autorización): 1.3.0, memoria DMA de la pantalla

Rama `feat/display-dma` (RESEARCH.md, fase 1). Cambios:
- copia del BSP con `trans_queue_depth = 2`;
- aviso de reservas fallidas en el `diag`.

Ni la funcionalidad ni las particiones cambian.

1. Arranque: `app_version=1.3.0`, `crash_count=0`, ojo cian animado y sin `Display render failed`.
2. `largest=` en reposo y tras dos o tres conversaciones (con «Hola ESP», interrupciones y cierre).
   Comparar con 1.2.0: 12 288 B en reposo y 8 192 B tras conversar. **Se espera un valor mayor.**
3. Animación del ojo fluida, sin tirones ni desgarros (antes con cola de 10 y ahora con 2).
4. Ningún `allocation failures` en los logs.

**Resultado (2026-11-25, instalación autorizada):**

- `iris system-update` correcto en 44 s.
- Estado: `app_version=1.3.0`, Boot ID `4574309094169058797`, firmware `0bd69a50b737…`,
  `crash_count=0`.
- Captura de logs en directo de dos conversaciones: «Hola ESP», preguntas, interrupción con un toque,
  cierre con un toque y vuelta al reposo a los 15 s.

| Medida | 1.2.0 | **1.3.0** |
|---|---|---|
| `largest=` en reposo | 12 288 B | **63 488 B** |
| `largest=` durante y después de conversar | 8 192 B (mínimo) | **63 488 B (constante)** |
| `int_dma_free` en reposo | 43 007 B | **108 867 B** |
| `int_dma_free` mínimo hablando | — | **91 631 B** |
| `free_internal` (device-status) | 74 975 B | **140 839 B** |

Sin `allocation failures`, sin errores de pantalla y `tx fail=0`. «Hola ESP», toque, interrupción y
reposo de 15 s funcionan igual. **El bloque DMA libre es 5 veces mayor y estable**: ya hay margen para
la cámara, el AEC o «Hola ESP» durante la respuesta.

### Prueba 12 (pendiente de autorización): 1.4.0, foto bajo demanda

Rama `feat/camera` (RESEARCH.md, fase 2). Requisito previo opcional: ejecutar
`python tools/test_live_setup.py` y comprobar que «functionDeclarations capturar_imagen» sale
**ACEPTADO** con los dos modelos.

1. Arranque: `app_version=1.4.0`, `crash_count=0` y `largest=` en reposo de unos 63 488 B.
2. «Hola ESP» → `connected; setup sent` → `session ready`. El servidor acepta la declaración de la
   función.
3. «VOX, ¿qué ves?» o «Mira lo que tengo en la mano»:
   - el ojo parpadea, muestra «OBSERVANDO» y se enciende el LED rojo de la cámara;
   - VOX describe la escena.
   - Logs: `toolCall capturar_imagen` → `app_camera: still 240x320 JPEG … B (q…) in … ms` →
     `image … B … sent` → `toolResponse capturar_imagen sent`.
4. **Orientación y color:** pregunta por un objeto de color conocido y si la imagen está derecha. Si sale
   girada o en espejo, cambia `VOX_CAMERA_ROTATION` / `MIRROR_*`. Si los colores salen cambiados
   (rojo↔azul), anotarlo.
5. Memoria: `largest=` durante y después de la foto, y ningún `allocation failures`.
6. Después de la foto, comprobar que `iris logs` y `iris device-status` siguen funcionando por USB.
7. Repetir la foto 3 veces seguidas y comprobar que no hay fugas: `int_dma_free` debe volver al mismo
   valor en reposo.

**Primer intento (2026-11-25, instalación autorizada, firmware `30b15ccc1351…`):**

- Funciona:
  - Google acepta la función: `toolCall capturar_imagen`, 3 veces.
  - El módulo se identifica como **SC101IOT** (`PID=0xda4a`, leído del propio sensor por SCCB).
  - La cámara se abre a 1280×720, se libera bien y Gemini recibe el error.
  - Sin fugas (`int_dma_free` 83 379 estable) y `largest=` de 63 488 mientras habla.
- **Falla:** `VIDIOC_DQBUF … timed out`, ningún fotograma en 1 s.
- El propietario confirma que la cámara funciona perfectamente en Vibe Mode, así que el hardware está
  bien.
- El LED rojo está encendido en reposo y se apaga al liberar la cámara, que queda en power-down.

**Hipótesis:** contención del ancho de banda de la PSRAM. VOX ejecutaba el código desde PSRAM
(`SPIRAM_XIP_FROM_PSRAM=y`), mientras que los ejemplos del BSP con cámara y Wi-Fi (`wifi_http_cam`,
`camera_lcd_preview`) lo desactivan.

**Compilación de diagnóstico (commit siguiente):**

- Código desde flash.
- Tiempo de espera del fotograma de 2 s.
- Una foto de prueba 8 s después del arranque, sin conversación (`VOX_CAMERA_BOOT_TEST`): solo se
  registra y no se envía.
- Coste: unos 19 KB más de RAM interna, porque los búferes de ESP-Iris dejan la PSRAM.

**Diagnóstico 1 (firmware `bc4aab0b28d9…`):** con el código desde flash, la foto de prueba del
arranque, sin conversación, **también falla** (`VIDIOC_DQBUF` tras 2 s). No es contención de PSRAM, así
que se revierte. Tampoco hay conflicto de pines con el BSP ni con VOX.

**Diagnóstico 2 (commit siguiente):** el resolvedor eligió `esp_video` 2.6.0 / `esp_cam_sensor` 2.7.0,
mientras que openai-live-esp32 usa `esp_video` 2.5.0 con este mismo BSP.

- Se fija `esp_video==2.5.0` (trae `esp_cam_sensor` 2.6.0).
- El formato por defecto del SC101IOT pasa a 1280×720 a 25 fps, el que activa el BSP.
- La prueba del arranque intenta A (1280×720 explícito, que resulta en 15 fps) y, si falla, B (formato
  por defecto, 25 fps).

**Diagnóstico 2 (firmware `7e7d15594342…`):** con `esp_video` 2.5.0 fallan **los dos formatos**
(A, 15 fps, y B, 25 fps), también en reposo. No es la versión ni el formato.

**Diagnóstico 3 (commit siguiente):**

- Tras STREAMON se cuentan los cambios de nivel en PCLK, VSYNC, DE y D0: ¿envía el sensor?
- Prueba C: igual que A, pero con el micrófono y WakeNet parados.

**Diagnóstico 3 (firmware de `29ba3ca`):**

- La prueba C, con el audio parado, también falla.
- La sonda se ejecutó, pero el canal de registros perdió sus líneas: aparecen líneas en blanco justo
  tras «Camera stream started».
- Comprobado: ESP-Iris usa el USB-OTG HS (`TINYUSB_PORT_HIGH_SPEED_0`), así que no comparte
  GPIO33/34 con la cámara.

**Diagnóstico 4 (commit siguiente):**

- `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG` desactivado. Lo hacen los ejemplos con cámara y Wi-Fi y
  openai-live-esp32, porque GPIO33/34 son D2 y el flash de la cámara; VOX no usa el USJ.
- Las medidas de la sonda se escriben después de cerrar la cámara, con pausas.

**Diagnóstico 4 (firmware de `9263419`): RESUELTO.**

- Con el **USB Serial/JTAG desactivado**, la foto del arranque sale bien: `still 240x320 JPEG 10107 B
  (q80) in 1911 ms`.
- La sonda mide unos 241 000 flancos en PCLK, unos 35 en VSYNC (≈ 25 fps) y actividad en DE y D0: el
  sensor siempre transmitió.
- **Causa:** el periférico USJ, activo aunque VOX no lo usaba, tiene los pads GPIO33/34, que son D2 y
  el flash de la cámara. Con él activo ningún fotograma llegaba a completarse.

**Prueba de voz (2026-11-25):** 3 fotos en una conversación.

- Cada una: `toolCall capturar_imagen` → `still 240x320 JPEG 13–14 KB (q80) in ~1370 ms` →
  `image … sent` → `toolResponse … sent`.
- VOX describe la escena.
- Según el propietario, **la orientación y los colores son correctos** (270° con espejo vertical,
  RGB565).
- `largest=` entre 47 104 y 63 488 B, sin `allocation failures` ni errores de pantalla.

**Prueba 12 superada.** Se retira el código de diagnóstico; `VOX_CAMERA_BOOT_TEST` queda disponible pero
apagado.

## 3. Volver al firmware actual (restauración)

**Antes de instalar VOX** (en Vibe Mode, en la pantalla del dispositivo):

1. Comprueba en **Mis Ideas** que ESP-Claw (o la aplicación actual) aparece descargada y apunta
   su código de seis dígitos. Si no aparece, **detente**: entonces la única vía de vuelta sería
   obtener su paquete.

**Para volver** (cuando VOX esté instalado):

1. Con VOX en marcha, borra la clave del dispositivo:
   `python tools/provision_key.py --clear`
2. Entra en Vibe Mode con `python mosaico.py iris test enter-recovery --project projects/servocraneo_gemini_direct`,
   o manteniendo pulsado el botón **AI** (GPIO7) al encender.
3. En Vibe Mode: **Mis Ideas → ESP-Claw → seleccionar como aplicación de arranque**. Vibe Mode
   revalida el paquete de la NAND, reescribe tabla y aplicación, comprueba la lectura y fija el
   arranque. Si no está en la NAND: **Descargar Ideas** con su código (requiere Wi-Fi).
4. Verifica con `python mosaico.py iris device-status --project projects/servocraneo_gemini_direct --json`.
5. Si olvidaste el paso 1, revoca la clave en Google AI Studio / Cloud Console: sin cifrado de
   flash, la clave seguiría en la NVS.

**Último recurso**, solo si ni la aplicación ni Vibe Mode responden: `python mosaico.py recover`.
Puede pedir el modo ROM a mano: apagar, mantener **Boot** (GPIO61, a la izquierda del USB-C),
encender y avisar al agente. Después hay que reinstalar la aplicación desde Mis Ideas.

## 4. Auditoría de la clave de Gemini

| Vector | Estado | Detalle |
|---|---|---|
| Código fuente, sdkconfig, binarios, GitHub | Limpio | La clave no existe en compilación; escaneo del historial Git sin coincidencias |
| `.env` | Riesgo local | Texto plano en el PC, ignorado por Git; el script recomienda vaciarlo tras provisionar. Alternativa: entrada oculta |
| Argumentos de proceso | Limpio | `provision_key.py` llama a la CLI **en el mismo proceso**; la clave no aparece en ninguna línea de órdenes |
| Log de `mosaico.py` (`.codex-runs/mosaico/*/raw.log`) | Limpio | Registra la orden hija (`rpc-raw … --payload-base64-stdin`) y la respuesta (`ok`), nunca la entrada estándar |
| Gateway ESP-Iris (registro de operaciones) | Hash | Guarda `request_bytes` y `request_sha256` del payload, no el texto. Inviable de invertir para una clave aleatoria de 39 caracteres |
| Salida en consola | Limpio | Se captura y se sustituye cualquier aparición de la clave por `<redacted>` |
| Logs del dispositivo | Limpio | Solo «API key provisioned»; la URI con la clave nunca se imprime (logs del transporte WebSocket a nivel WARN) |
| Lectura remota | Imposible | No existe RPC de lectura; `status` solo devuelve `true`/`false` |
| Transporte | USB | Usa USB. El TCP emparejado no se ha verificado como cifrado |
| Flash del dispositivo | Riesgo físico | Sin cifrado NVS ni de flash (necesitarían eFuses irreversibles). Mitigación: clave exclusiva, restringida y revocable |

## 5. Recuperación ante errores (resumen verificado en el código)

| Fallo | Comportamiento |
|---|---|
| Sin Wi-Fi o sin configurar | Ojo violeta «SIN WI-FI» / «CONFIGURA WI-FI EN VIBE»; vuelve a cian cuando conecta |
| Sin clave | «SIN CLAVE»; vuelve a cian al provisionarla |
| Clave rechazada (HTTP 401/403) | «CLAVE RECHAZADA» tras los reintentos; se limpia al provisionar otra |
| TLS, DNS o servidor caído | Reintento cada 2 s; modelo alternativo tras 2 fallos; error tras 5 o 45 s en conexión |
| Corte durante la conversación, `goAway` o 10 min | Reconexión automática con `sessionResumption`; ojo ámbar hasta `setupComplete` |
| El modelo no responde | 15 s en ámbar y vuelve a escuchar |
| Audio de salida atascado | 8 s sin audio en verde y vuelve a escuchar |
| Interrupción táctil | Vacía la reproducción; descarta el resto del turno (máximo 10 s) |
| Búfer de reproducción lleno o sin datos | Contrapresión con espera de 1,5 s; silencio en vez de basura |
| Error de lectura/escritura I2S | Reintento a los 10 ms sin bloquear el resto |
| NVS de app ilegible | Ojo violeta «ERROR DE NVS»; Recovery operativo; nunca se borra |
| Fallo de audio o de memoria al arrancar | «ERROR INTERNO»; Recovery operativo |
| Setter GSP rechazado | Se reintenta en el siguiente fotograma; se registra en el diagnóstico |
| Pantalla o paquete UI no disponibles | La sesión arranca sin UI; Recovery operativo |

## 6. Primera prueba física (requiere autorización)

Desde la raíz `esp-mosaico-vibe`, con `$env:IDF_CONFIGDEP_ENABLE = "0"` y el dispositivo por USB:

```powershell
python mosaico.py iris status --all --json
python mosaico.py iris list --details --project projects/servocraneo_gemini_direct --json
python mosaico.py iris device-status --project projects/servocraneo_gemini_direct --json
python mosaico.py iris logs --snapshot --project projects/servocraneo_gemini_direct
```

Revisar la identidad, el modo, la aplicación actual y la versión de Vibe Mode, y confirmar
**Mis Ideas** (sección 3) antes de continuar. Después:

```powershell
python mosaico.py iris system-update --project projects/servocraneo_gemini_direct
python mosaico.py iris device-status --project projects/servocraneo_gemini_direct --json
python mosaico.py iris logs --snapshot --project projects/servocraneo_gemini_direct --grep vox
python mosaico.py iris screenshot vox-idle.png --project projects/servocraneo_gemini_direct --json
```

Aceptación de la instalación: el mismo Device ID pasa por normal → Vibe Mode → normal con
Boot ID nuevo, la aplicación queda sana y se ve el ojo cian. Después, la clave:

```powershell
python projects/servocraneo_gemini_direct/tools/provision_key.py --status
python projects/servocraneo_gemini_direct/tools/provision_key.py
python projects/servocraneo_gemini_direct/tools/provision_key.py --status
```

Prueba funcional: tocar la pantalla (cian → ámbar → rojo), preguntar «¿Cuánto es siete por
ocho?» (rojo → ámbar → verde con voz Charon en español), tocar mientras habla (vuelve a rojo)
y tocar para cerrar (cian). Las evidencias son `iris logs --grep "vox_session\|gemini_live"`
y `iris screenshot` en cada estado.
