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
- **«Hola ESP» (1.1.0) queda validado en el hardware.**

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
