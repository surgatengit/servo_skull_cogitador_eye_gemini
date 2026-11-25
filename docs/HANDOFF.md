# VOX M-01 GEMINI DIRECT — traspaso para retomar el desarrollo

Última actualización: **2026-11-25**, al cierre de la sesión de 1.2.0 y 1.3.0. `main` = **1.3.0**
instalada y validada.

Otros documentos:
- Google Search (opcional y apagado): [GOOGLE_SEARCH.md](GOOGLE_SEARCH.md);
- plan de evolución por fases: [RESEARCH.md](RESEARCH.md);
- ideas futuras: [ROADMAP.md](ROADMAP.md).
Empieza por aquí en cualquier sesión nueva. El detalle está en [DESIGN.md](DESIGN.md)
(requisitos y arquitectura) y en [INSTALL.md](INSTALL.md) (instalación, restauración, seguridad y
el historial completo de las pruebas físicas).

## 1. Estado en una frase

VOX M-01 **1.3.0** funciona en el ESP-Mosaico. Es un ojo mecánico GSP que:
- conversa con **Gemini Live directamente por Wi-Fi y WebSocket seguro**, sin Windows ni servicios
  intermedios;
- se despierta con **«Hola ESP»** en local;
- se interrumpe con un toque y se cierra con otro;
- no vuelve al reposo mientras Gemini prepara la respuesta y vuelve a los 15 s de inactividad;
- desde 1.3.0 tiene margen de sobra en la memoria DMA de la pantalla.

Todo está validado en el hardware (pruebas 1–11 de INSTALL.md). Google Search está implementado pero
**apagado** por la cuota del plan gratuito.

| Elemento | Valor |
|---|---|
| Repositorio | `github.com/surgatengit/cogitador_gemini` (**privado**), rama `main` = versión instalada (**1.3.0**, etiqueta `v1.3.0`); `v1.2.0` y `v1.1.0` = estables anteriores |
| 1.3.0 | Memoria DMA de la pantalla: copia del BSP con `trans_queue_depth = 2` (`third_party/`), bloque DMA libre de 63 488 B estable (antes 8 192 B), aviso de reservas fallidas |
| 1.2.0 | Google Search opcional y **desactivado** (`VOX_GOOGLE_SEARCH=n`, cuota del plan gratuito), espera mientras el modelo piensa, motivo de cierre en los logs |
| Ruta local | `esp-mosaico-vibe/projects/servocraneo_gemini_direct/` (repo Git propio, dentro del espacio de trabajo) |
| Último commit validado | `35159d8` (main) |
| Firmware en el dispositivo | 1.3.0, SHA `0bd69a50b737…`, instalado con `iris system-update` (2026-11-25), prueba 11 superada |
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
| `app_camera` | Foto bajo demanda (1.4.0): abre la cámara, captura un fotograma, PPA a 240×320 y JPEG por hardware ≤ 24 KB, y la cierra | La cámara parchea ESP-IDF (sección 5); componentes del BSP en `third_party/` |
| `app_photos` | Anillo de las 10 últimas fotos en la partición `photos`, para la galería (1.5.0) | Solo desde la tarea de la sesión (pila interna); cabecera escrita al final y CRC |
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

Particiones (1.5.0): prefijo inmutable de Vibe Mode, `nvs` 0x200000, `main_app` 0x210000 (11 712 K),
`photos` 0xD80000 (512 K, fotos de la galería),
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
  **Desde 1.3.0 (rama `feat/display-dma`):** VOX usa una copia del BSP en `third_party/esp-mosaico-bsp`
  con `trans_queue_depth = 2` en la pantalla, el mismo arreglo que openai-live-esp32 (RESEARCH.md §2).
  Comprueba la copia con `python tools/check_bsp_fork.py` y rehazla si se actualiza el submódulo.
  El `diag` avisa además de cualquier reserva de memoria fallida (`allocation failures: …`).
- **La cámara parchea ESP-IDF (decisión del propietario, 2026-11-25, fase 2).** Al configurar CMake,
  `mosaico_module_camera` aplica `patches/esp-idf-dvp-frame-capture-stability.patch` (157 líneas del
  driver DVP `esp_driver_cam`) **sobre `C:\esp\esp-idf-master`**. Además aplica un parche del sensor
  SC101IOT en `managed_components`, dentro del proyecto.
  - Se aceptó la opción 2 (dejar que parchee) porque en este PC solo se compila VOX.
  - El commit de ESP-IDF sigue siendo `7b9cc1ac79f8`, pero su árbol queda modificado.
  - Compruébalo con `git -C C:\esp\esp-idf-master status`. Para deshacerlo:
    `git -C C:\esp\esp-idf-master checkout components/esp_driver_cam`.
  - Otro PC necesita el mismo parche (lo aplica la propia compilación) para tener capturas estables.
  - A 2026-11-25, ESP-IDF `master` aún no incluye el arreglo. Si se actualiza ESP-IDF y ya lo trae, el
    componente lo detecta («already applied») o avisa sin romper la compilación.
- **Componente de cámara con el arreglo oficial de PWDN `b60f5d8` (prueba 13).** Con el de nuestro
  BSP `5f88b91`, el driver genérico manejaba CAM_PWDN con una polaridad equivocada para el módulo, y
  el sensor respondía pero no entregaba fotogramas. Detalle en `third_party/esp-mosaico-bsp/VOX_FORK.md`.
- **Formato del SC101IOT: solo 1280×720 a 25 fps (cámara, 1.5.0).** Con el de 15 fps la cámara
  tampoco entrega fotogramas. `sdkconfig.defaults` lo fija como formato por defecto y no compila el de
  15 fps (prueba 13).
- **USB Serial/JTAG debe seguir desactivado (cámara, 1.4.0).** `# CONFIG_USJ_ENABLE_USB_SERIAL_JTAG is not set`
  en `sdkconfig.defaults`. GPIO33/34 son D2 y el flash de la cámara: con el USJ activo, el SC101IOT
  transmite pero ningún fotograma se completa (`VIDIOC_DQBUF` agota el tiempo; prueba 12). VOX no usa el
  USJ: la consola va por UART y ESP-Iris por el USB-OTG HS.
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

El plan principal está en [RESEARCH.md](RESEARCH.md) §9. Estado:
- **Fase 0 (verificaciones) hecha:** `iris` usa el USB-OTG con TinyUSB, no el USB Serial/JTAG que
  comparte pines con la cámara, y la cámara es el módulo V1.4 (SC101IOT, enfoque manual).
- **Fase 1 (memoria DMA) hecha:** 1.3.0.

Siguientes, en el orden recomendado:

1. **Fase 2, cámara bajo demanda** («VOX, ¿qué ves?»): captura única, JPEG por hardware y
   `realtimeInput.video`.
2. **Fase 3, funciones propias:** emoción del ojo, sensores y mensaje «CUOTA AGOTADA» sin reintentos.
   Es independiente de la fase 2.
3. **Fase 5, interrupción por voz:** primero «Hola ESP» durante la respuesta, que ahora es viable con
   el margen de DMA; después, AEC con referencia por software.
4. **Fase 4, alimentación:** compilación de diagnóstico del BQ27220 y después la base magnética
   (ROADMAP.md §3).
5. **Fase 6, seguimiento de movimiento** para orientar el iris (rejilla 32×24, idea de jamiryo, MIT).

Otros pendientes:

- **Google Search:** reactivar solo con cuota. Comprobarlo antes con `tools/test_live_setup.py`
  (GOOGLE_SEARCH.md).
- **BSP:** proponer aguas arriba (`esp-mosaico-bsp`) que se pueda configurar `trans_queue_depth`, para
  poder retirar la copia `third_party/esp-mosaico-bsp`. Si se actualiza el submódulo, rehacer la copia
  y pasar `tools/check_bsp_fork.py`.
- Ajuste fino: latencia, umbral de voz, volumen y personalidad.
- Pruebas de larga duración: sesiones de más de 15 min con compresión, reanudación y `goAway`.
- Seguridad opcional (irreversible, **requiere autorización expresa**): cifrado NVS con HMAC en eFuse.
- Opcional: empaquetar como Idea (`mosaico.py project package`).
- Si `GEMINI_API_KEY` sigue como variable de entorno persistente en Windows, el propietario puede
  eliminarla: `[Environment]::SetEnvironmentVariable("GEMINI_API_KEY", $null, "User")`.
- Los registros del dispositivo solo conservan unos 20 s tras el arranque. Para las pruebas de voz, abre
  una captura en directo: `python mosaico.py iris logs --project projects/servocraneo_gemini_direct
  --timeout 240`.

## 7. Cómo verificar rápidamente que todo sigue bien

1. `iris device-status --json` → `firmware_mode=normal`, `project_name=servocraneo_gemini_direct`,
   `app_version=1.3.0`, `crash_count=0`.
2. `iris logs --snapshot` → `app_wake: listening for "Hola ESP"`, `search off`,
   `diag st=0 … largest=` de unos 63 488 (con 1.2.0 era 8 192–12 288; si baja de 8 192 el ojo puede
   congelarse) y ningún `Display render failed` ni `allocation failures`.
3. Prueba de voz: «Hola ESP» → ámbar → rojo; pregunta → verde con voz; un toque interrumpe; otro cierra.
4. Para pruebas de voz con medidas, usa una captura en directo (sección 6).
