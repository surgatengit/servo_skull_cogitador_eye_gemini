# VOX M-01 GEMINI DIRECT — traspaso para retomar el desarrollo

Última actualización: **2026-11-25**: rama `feat/google-search` (1.2.0) compilada, probada en PC
y en GitHub, **pendiente de instalar** (prueba 10 de INSTALL.md). Google Search nativo:
[GOOGLE_SEARCH.md](GOOGLE_SEARCH.md). Ampliaciones futuras: [ROADMAP.md](ROADMAP.md).
Empieza por aquí en cualquier sesión nueva. El detalle está en [DESIGN.md](DESIGN.md)
(requisitos y arquitectura) y en [INSTALL.md](INSTALL.md) (instalación, restauración, seguridad y
el historial completo de las pruebas físicas).

## 1. Estado en una frase

VOX M-01 **1.1.0** funciona en el ESP-Mosaico: un ojo mecánico GSP que conversa con **Gemini Live
directamente por Wi-Fi y WebSocket seguro** (sin Windows ni servicios intermedios), se despierta con
**«Hola ESP»** en local, se interrumpe con un toque, se cierra con otro y vuelve al reposo a los 15 s.
Todo validado en el hardware (pruebas 1–9 de INSTALL.md).

| Elemento | Valor |
|---|---|
| Repositorio | `github.com/surgatengit/cogitador_gemini` (**privado**), rama `main` = versión instalada (**1.2.0**, etiqueta `v1.2.0`); `v1.1.0` = estable anterior |
| 1.2.0 | Google Search opcional y **desactivado** (`VOX_GOOGLE_SEARCH=n`, cuota del plan gratuito), espera mientras el modelo piensa, motivo de cierre en los logs |
| Ruta local | `esp-mosaico-vibe/projects/servocraneo_gemini_direct/` (repo Git propio, dentro del espacio de trabajo) |
| Último commit validado | `35159d8` (main) |
| Firmware en el dispositivo | 1.2.0, SHA `42ff0083c79c…`, instalado con `iris system-update` (2026-11-25), validado por voz |
| Dispositivo | ESP-Mosaico, Device ID `4553502d4952495301001c2904d11390`, USB (COM7 control / COM6 datos) |
| Modelo Gemini | `gemini-3.1-flash-live-preview` (principal) / `gemini-3.8-live` (alternativo) |
| Clave de API | Provisionada en la NVS propia del dispositivo (`vox_secret`); no está en el repo |
| App anterior | ESP-Claw = Mosaico-Claw 0.8.0, Idea **100000** (restaurable desde Vibe Mode → Mis Ideas) |

## 2. Entorno

- ESP-IDF en `C:\esp\esp-idf-master`, commit exacto `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`
  (6.2.0), herramientas en `%USERPROFILE%\.espressif` (instaladas solo para `esp32s31`).
- Python 3.13, GitHub CLI autenticado como `surgatengit`.
- **La ruta del espacio de trabajo tiene espacios** (`1 proyectos`): compila siempre con
  `IDF_CONFIGDEP_ENABLE=0` (el runner y `mosaico.py` ya lo hacen en Windows).
- El simulador usa MSVC (Visual Studio) vía `cmake` del entorno de ESP-IDF: activa antes `export.ps1`.

Comandos habituales (desde la raíz `esp-mosaico-vibe`):

```powershell
$env:IDF_CONFIGDEP_ENABLE = "0"
python submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py --project projects/servocraneo_gemini_direct --idf-path C:\esp\esp-idf-master build
```
```powershell
Set-Location C:\esp\esp-idf-master; . .\export.ps1; Set-Location -
python mosaico.py project sim --project projects/servocraneo_gemini_direct --interactive
```
```powershell
python mosaico.py iris device-status --project projects/servocraneo_gemini_direct --json
python mosaico.py iris logs --snapshot --project projects/servocraneo_gemini_direct
python mosaico.py iris system-update --project projects/servocraneo_gemini_direct
```

## 3. Reglas de trabajo acordadas con el propietario

1. **Ninguna escritura en el dispositivo sin autorización expresa en el chat**: instalar, reiniciar,
   cambiar el modelo, cargar o borrar la clave. Las lecturas (`device-status`, `logs --snapshot`,
   `screenshot`) son seguras.
2. Solo `python mosaico.py` toca el dispositivo. Nunca `idf.py flash`, `esptool` en escritura,
   `recover` (salvo último recurso autorizado), borrado de flash, particiones fuera de la región de
   aplicación ni eFuses.
3. **Instalar con `iris system-update`.** `iris app-update` falla con `recovery_required` porque este PC
   no tiene registro de `recover` para el dispositivo. **No ejecutes `recover` para arreglarlo**:
   reescribiría Vibe Mode con 0.4.1 sin saber qué versión tiene el dispositivo.
4. Rama por tarea (`feat/...`), commits descriptivos con trailer `Co-Authored-By`, push al repo privado
   al final de cada etapa funcional; fusión a `main` solo cuando el propietario lo pide.
   Antes de cada push: escanear secretos (`AIza…`, tokens, `.env`, binarios, `sdkconfig`, `.gateway/`).
5. Se trabaja **solo** dentro de este proyecto: no modificar submódulos (BSP, utils), otros proyectos
   ni el repo padre.
6. Comunicación en español de España.

## 4. Mapa del código

| Componente | Qué hace | Notas clave |
|---|---|---|
| `main/main.c` | Raíz de composición | NVS ilegible → ojo violeta «ERROR DE NVS», nunca borra |
| `app_vox_eye` | Ojo GSP (escena `ui/main.json`), animación a 30 fps, toque | Brillo cuantizado (≤10 Hz) por la presión DMA de la pantalla |
| `app_vox_session` | Máquina de estados, vigilantes, diagnóstico | Logs `diag st=…` cada 5 s en conversación y 10 s en reposo |
| `app_gemini_live` | Cliente WSS + protocolo Live | TLS en PSRAM, búfer WS de 4 KB, 409 → sesión nueva |
| `app_audio` | ES8311 a 48 kHz; 16 kHz de subida, 24 kHz de bajada | Ganancia 24 dB, volumen 70 |
| `app_wake` | WakeNet10 `wn10_es_holaesp` (esp-sr 2.6.2) | Detector solo en reposo; se devuelve si el bloque DMA < 8 KB |
| `app_wifi` | Estación con credenciales de Vibe Mode | `WIFI_STORAGE_RAM`, sin ahorro de energía |
| `app_secrets` | Clave y modelo en NVS + RPC `0x5658` | Sin RPC de lectura de la clave |
| `tools/provision_key.py` | Provisión de la clave por USB | CLI en el mismo proceso; nunca en argv ni en logs |

Kconfig (`components/app_vox_session/Kconfig`): `VOX_GEMINI_MODEL`, `VOX_GEMINI_FALLBACK_MODEL`,
`VOX_WAKE_WORD`, `VOX_IDLE_TIMEOUT_S`=15, `VOX_PREROLL_MS`=2000, `VOX_SILENT_MODEL_VOICED_FRAMES`=250,
`VOX_VOICED_PEAK_THRESHOLD`=1036 (≈ −30 dBFS), `VOX_MIC_GAIN_DB`=24, `VOX_SPEAKER_VOLUME`=70.
Desde 1.2.0: `VOX_GOOGLE_SEARCH`=**n** (el servidor cierra con 1011 los `setup` con
`googleSearch`; ver INSTALL.md, prueba 10), `VOX_SHOW_SEARCH_SOURCES`=n (búsqueda discreta, sin citar
fuentes, por decisión del propietario) y `VOX_RESPONSE_TIMEOUT_S`=30 (espera máxima sin salida del modelo
mientras hay una respuesta pendiente; durante ese tiempo no corre el reposo).

Pruebas en PC (MSVC) del análisis de `groundingMetadata` y del formato de la línea de estado:
`powershell -File tools/host_tests/run.ps1`.

Particiones (1.1.0): prefijo inmutable de Vibe Mode, `nvs` 0x200000, `main_app` 0x210000 (12 224 K),
`model` 0xE00000 (1 M, `srmodels.bin`), `ui_apps` 0xF00000 (1 M). El paquete de System Update tiene
4 componentes: tabla, app, `ui_apps` y `model`.

## 5. Trampas conocidas (lee esto antes de tocar nada)

- **Memoria DMA de la pantalla (la más importante).** El BSP fija `psram_dma_direct = false` y
  transacciones SPI de 7 680 B (8 líneas) con cola de 10, así que **cada trozo de fotograma reserva un
  búfer intermedio interno DMA contiguo**. Si el bloque libre más grande baja de ~7,7 KB, el ojo se
  congela. Mitigaciones activas: TLS y Wi-Fi/LWIP en PSRAM, `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`,
  `SPIRAM_MALLOC_RESERVE_INTERNAL=65536`, búferes propios en PSRAM, brillo cuantizado y la salvaguarda
  del detector. Mínimo observado: **8 192 B** (margen ajustado). Vigila `largest=` en los logs `diag`
  al añadir cualquier función. La solución de fondo sería que el BSP active `psram_dma_direct`.
- **Cambiar valores por defecto de Kconfig o de `sdkconfig*.defaults` no afecta a un `build/sdkconfig`
  existente.** Borra `build/sdkconfig` (es generado) y recompila; compara después el diff de `CONFIG_`.
- **Pilas de tareas en PSRAM: no.** La sesión lee la NVS; con la caché desactivada durante una
  operación de flash, una pila en PSRAM provoca una aserción.
- `gemini-3.8-live` estuvo mudo por WebSocket directo (pruebas 2–3) y luego respondió (prueba 7):
  es intermitente. El vigilante de modelo mudo cambia una vez por arranque tras 10 s de voz sin respuesta.
- Errores `409 Conflict` en Google AI Studio = reconexiones rápidas con *handle* de reanudación;
  ya gestionados (se descarta el *handle*).
- «Hola ESP» está apagado a propósito mientras VOX habla (la interrupción es por toque).
- La aplicación ocupa 2,51 MB: no cabría en `vibe_mode` (1,75 MB); ESP-IDF solo avisa y es correcto.
- Los avisos `CMake Warning … object file directory … 250 characters` y los de `component_validation`
  vienen de la ruta larga y de ESP-IDF; no son errores.

## 6. Pendiente y siguientes pasos

0. **Estado 2026-11-25:** 1.2.0 con búsqueda desactivada (`VOX_GOOGLE_SEARCH=n`) instalada y
   validada por voz (INSTALL.md, prueba 10). Falta fusionar en `main` y etiquetar `v1.2.0` cuando
   lo pida el propietario. Lo que sigue en este punto 0 es el plan original para la búsqueda, que
   solo aplica si se reactiva con cuota disponible.
   **1.2.0 / Google Search:** con autorización, instalar la rama `feat/google-search` y hacer la
   prueba 10 (confirmar que el servidor acepta `tools:[{"googleSearch":{}}]`, las fuentes en pantalla
   y `largest=` ≥ 8192). Si sale bien, fusionar en `main` (solo cuando lo pida el propietario) y
   etiquetar `v1.2.0`. Cumplimiento de la presentación de fuentes en un dispositivo de voz: riesgo
   abierto (GOOGLE_SEARCH.md §4).

1. **Proponer al BSP** (`esp-mosaico-bsp`, fuera de este repo) evaluar `psram_dma_direct = true` para el
   CO5300: eliminaría la presión DMA de raíz.
2. **AEC por software / full-duplex** (esp-sr AFE, formato «MR» con referencia de reproducción) para
   poder interrumpir hablando en lugar de con un toque. Ojo con la RAM interna (sección 5).
3. Ajuste fino: latencia, umbral de voz, volumen, prompt de la personalidad.
4. Pruebas de larga duración (sesiones >15 min con compresión y reanudación, `goAway`).
5. Seguridad opcional (irreversible, **requiere autorización expresa**): cifrado NVS con HMAC en eFuse.
6. Opcional: borrar las ramas ya fusionadas si reaparecen; empaquetar como Idea (`mosaico.py project package`).
7. Si `GEMINI_API_KEY` sigue como variable de entorno persistente en Windows, el propietario puede
   eliminarla: `[Environment]::SetEnvironmentVariable("GEMINI_API_KEY", $null, "User")`.

## 7. Cómo verificar rápidamente que todo sigue bien

1. `iris device-status --json` → `firmware_mode=normal`, `project_name=servocraneo_gemini_direct`,
   `app_version=1.1.0`, `crash_count=0`.
2. `iris logs --snapshot` → `app_wake: listening for "Hola ESP"`, `diag st=0 … largest=` ≥ 8192,
   ningún `Display render failed`.
3. Prueba de voz: «Hola ESP» → ámbar → rojo; pregunta → verde con voz; un toque interrumpe; otro cierra.
4. (1.2.0) Pregunta de actualidad → ámbar mientras busca → verde con «GOOGLE: …» / «FUENTE …» abajo.
