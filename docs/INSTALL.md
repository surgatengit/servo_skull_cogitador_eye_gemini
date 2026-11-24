# VOX M-01 — Instalación, restauración y seguridad

Estado: **v1.0 (2026-11-24)**. Verificado sobre el código del espacio de trabajo
(`esp-mosaico-utils` en `f58f4f9`, Vibe Mode empaquetado 0.4.1, ABI 2, layout 5).
**Nada de esto se ha ejecutado todavía en el dispositivo.**

Regla general: solo `python mosaico.py` toca el dispositivo. Nunca `idf.py flash`,
`esptool` en escritura, borrado completo de la flash ni cambios de particiones o eFuses.

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
