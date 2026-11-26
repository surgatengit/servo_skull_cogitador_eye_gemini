# Cogitador M-01 GEMINI DIRECT — diseño del cliente Gemini Live nativo

Estado: **v1.1 (2026-11-24)**. Decisiones técnicas aprobadas por el propietario; versión 1.1.0
validada en el hardware. Para retomar el desarrollo, empieza por [HANDOFF.md](HANDOFF.md).
Ámbito: solo este proyecto (`projects/servocraneo_gemini_direct/` dentro de un
espacio de trabajo `esp-mosaico-vibe`). Sin Windows, sin puentes ni servicios
intermedios: el ESP-Mosaico abre directamente un WebSocket seguro con Gemini Live por Wi-Fi.

## 1. Requisitos aprobados

### 1.1 Conversación

| Requisito | Decisión |
|---|---|
| Modelo principal | `gemini-3.1-flash-live-preview` (cambio aprobado el 2026-11-24 tras las pruebas físicas: `gemini-3.8-live` completa el `setup` pero no transcribe ni responde por WebSocket directo) |
| Modelo alternativo | `gemini-3.8-live`, configurable sin recompilar (RPC `set_model`) |
| Turnos | Micrófono continuo; Gemini detecta las pausas (VAD automático del servidor) |
| Eco (v1) | Micrófono silenciado mientras el altavoz reproduce; interrupción táctil |
| Duración | Sin límite artificial (no hay grabación de 8 s); sesión continua con reanudación |
| Idioma | Español de España (`es-ES`) |
| Voz | `Charon` |
| Personalidad | Cogitador M-01, servocráneo del Adeptus Mechanicus: solemne, ceremonial, con referencias ocasionales al Omnissiah. Útil y directo con cualquier pregunta legítima, incluidas las matemáticas sencillas |

### 1.2 Clave de API

- Clave exclusiva de este dispositivo, configurada **en privado después de instalar** la aplicación.
- Nunca en el código fuente, los binarios distribuidos, GitHub, sdkconfig ni los registros.
- **No** se usa la información compartida de Vibe Mode (`sysmeta/shared`) como almacén,
  porque la pueden leer todas las Ideas.
- Detalle y protecciones disponibles: sección 4.

### 1.3 Ojo mecánico (obligatorio)

- Ocupa toda la pantalla de 480×480: un iris animado que explora distintas direcciones con
  movimiento suave (aceleración y frenado), microsacadas y parpadeos ocasionales.
- Colores por estado:

  | Estado | Color | Significado |
  |---|---|---|
  | Reposo | Cian `#00E5FF` | Conversación cerrada o en espera |
  | Escuchando | Rojo `#FF1A1A` | Micrófono abierto y enviando audio |
  | Procesando | Ámbar `#FFB000` | Turno enviado; esperando la respuesta del modelo |
  | Hablando | Verde `#20FF60` | Reproduciendo la voz de Cogitador |
  | Error | Violeta `#B040FF` | Sin Wi-Fi, sin clave, error de red o de sesión |

- **Un toque en la pantalla abre o cierra la conversación.** Mientras Cogitador habla, un toque la
  interrumpe y vuelve a escuchar (sección 3.4).
- Una línea de estado discreta en la parte inferior solo para errores accionables
  (por ejemplo «SIN WI-FI» o «SIN CLAVE»).

## 2. Restricciones de partida (verificadas)

| Elemento | Valor | Origen |
|---|---|---|
| SoC | ESP32-S31, 2 núcleos a 320 MHz, Wi-Fi 6 nativo, PSRAM | `soc_caps`, `build/sdkconfig` |
| ESP-IDF | `7b9cc1ac79f8` (6.2.0) | `AGENTS.md` del espacio de trabajo |
| Códec | ES8311, un solo bus I2S full-duplex a 48 kHz / 16 bit / estéreo | BSP `audio.h`, `onboard/audio.c` |
| Referencia AEC por hardware | No disponible (`no_dac_ref = true`) | BSP `onboard/audio.c` |
| Particiones | `nvs` 64 KiB (de la app), `main_app` 13 MiB; prefijo retenido inmutable | `partitions.csv` |
| Comprobación de tamaño de IDF | Contra la app más pequeña (`vibe_mode`, 0x1C0000) | Log de compilación |
| Red | La app es dueña del Wi-Fi; ESP-Iris TCP espera a la netif (`esp_netif_init` idempotente) | `esp_iris_transport_tcp.c` |
| Credenciales Wi-Fi | `app_config_load_wifi()` → `WIFI_STORAGE_RAM` | `app_config/README.md` |
| UI | GSP 1.5.1: `x/y` dinámicos acotados, color por *bind* y temporizadores en la tarea de render | Documentación de esp-gsp |

Protocolo Gemini Live (documentación oficial; la API está en *preview*):

- `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=…`
- Primer mensaje `setup`; no se envía nada más hasta recibir `setupComplete`.
- Entrada: `realtimeInput.audio`, `mimeType: "audio/pcm;rate=16000"`, PCM 16 bit LE mono en base64.
- Salida: `serverContent.modelTurn.parts[].inlineData`, PCM 16 bit LE a 24 kHz.
- `interrupted`, `turnComplete`, `generationComplete`, `goAway.timeLeft`, `sessionResumptionUpdate`.
- Límites: sesión de audio de 15 min sin compresión y conexión de ~10 min (se resuelven con
  `contextWindowCompression` y `sessionResumption`).

## 3. Arquitectura

```
main (composición; app_main solo inicializa y conecta)
 ├─ app_config        (plantilla) Wi-Fi compartido de Vibe Mode, solo lectura
 ├─ app_secrets       clave y modelo en NVS propio de la app + RPC privada de provisión
 ├─ app_wifi          estación Wi-Fi, reintentos con backoff, eventos de estado
 ├─ app_audio         I2S/ES8311: captura 48k→16k mono, reproducción 24k→48k estéreo
 ├─ app_gemini_live   WSS + protocolo Live: setup, envío, reensamblado, parseo y reanudación
 ├─ app_cogitador_session   máquina de estados: une audio ↔ Gemini ↔ ojo y gestiona los toques
 └─ app_cogitador_eye       UI GSP del ojo mecánico (portable al simulador; sustituye a app_hello)
```

Las dependencias van en una sola dirección. `app_cogitador_eye` no conoce la red: recibe estados
y emite toques. Cada componente guarda su estado en un contexto privado y expone `include/` mínimos.

### 3.1 `app_wifi`

- Lee las credenciales con `app_config_load_wifi()`, las aplica con `WIFI_STORAGE_RAM` y
  borra la copia temporal con `mbedtls_platform_zeroize`.
- Reintentos con backoff exponencial (1 s → 30 s). Estados `UNCONFIGURED`, `CONNECTING`,
  `CONNECTED`, `FAILED`, notificados por callback.

### 3.2 `app_gemini_live`

- `espressif/esp_websocket_client` 1.8.x sobre esp-tls/mbedTLS con el bundle de CA
  (`esp_crt_bundle_attach`); se verifica siempre el certificado del servidor.
- La URI con la clave se compone en un búfer de la instancia y se borra tras conectar; nunca se registra.
- Recepción: reensamblado de fragmentos en un búfer de PSRAM de la instancia (256 KiB),
  parseo con `espressif/cjson` (asignador en PSRAM) y entrega de PCM de 24 kHz por callback.
- Envío: tramas de 40 ms (1280 B de PCM a 16 kHz → ~1,8 KB de JSON); base64 en un búfer reutilizable.
- `setup`: modelo, `responseModalities: ["AUDIO"]`, voz Charon, `languageCode: es-ES`,
  `systemInstruction` con la personalidad, transcripciones de entrada y salida,
  `sessionResumption` y `contextWindowCompression.slidingWindow`.
- `goAway` o cierre → reconexión con el último *handle* de reanudación y backoff.
- El modelo alternativo se usa automáticamente si el principal falla durante el `setup`
  y se puede fijar por RPC.
- **Vigilante de modelo mudo** (en `app_cogitador_session`): si en una conexión salen al menos
  `COGITADOR_SILENT_MODEL_VOICED_FRAMES` tramas con voz (100 = 4 s; pico ≥ `COGITADOR_VOICED_PEAK_THRESHOLD`,
  ≈ −30 dBFS) y no llega ninguna transcripción ni audio, la sesión reconecta **una vez por arranque**
  con el otro modelo (`app_gemini_live_swap_model`).

### 3.3 `app_audio`

```
MIC  I2S RX 48k estéreo → canal izquierdo → FIR paso bajo + diezmado ÷3 → 16k mono → callback
SPK  ring 24k (PSRAM, ~4 s) → interpolación ×2 con FIR → 48k → estéreo → I2S TX
```

- Conversores FIR propios con razones enteras (÷3, ×2); el coste es despreciable a 320 MHz.
- Tareas de captura y reproducción en el núcleo 1; la red en el núcleo 0.
- `flush_playback()` vacía el ring al instante (interrupción); el nivel de salida mueve la pupila.

### 3.4 `app_cogitador_session` — estados y toques

```
CERRADA ──toque──▶ CONECTANDO ──setupComplete──▶ ESCUCHANDO ──(audio del modelo)──▶ HABLANDO
   ▲                    │                           │  ▲                              │
   │                 error ──▶ ERROR ◀──────────────┘  └──turnComplete + vaciado──────┘
   └──────────toque (en ESCUCHANDO, ERROR o CONECTANDO)─────────────────────────────────┘
HABLANDO ──toque──▶ interrumpe la reproducción y vuelve a ESCUCHANDO (sin cerrar la sesión)
ESCUCHANDO ──actividad detectada/turno enviado──▶ PROCESANDO (ámbar) ──audio──▶ HABLANDO
```

- Color del ojo: CERRADA → cian; ESCUCHANDO → rojo; PROCESANDO → ámbar; HABLANDO → verde; ERROR → violeta.
- PROCESANDO empieza con la primera transcripción de entrada (el usuario está hablando)
  y termina con el primer audio del modelo; tiene un temporizador de seguridad.
- Mientras HABLANDO, el micrófono no envía nada; al terminar la reproducción (más una cola de 300 ms)
  vuelve a ESCUCHANDO.
- Para cerrar la sesión hay que tocar en ESCUCHANDO, PROCESANDO o ERROR. En HABLANDO,
  el toque solo interrumpe; un segundo toque cierra.

### 3.5 `app_cogitador_eye`

- Escena GSP 480×480: carcasa metálica, anillo luminoso de estado, cuenca oscura, grupo del iris
  (anillo de color, anillo interior, pupila y brillo) con `x/y` dinámicos acotados, párpados
  superior e inferior con `y` dinámico y un botón transparente a pantalla completa.
- Animación con un temporizador GSP de 33 ms: objetivo aleatorio cada 0,7–2,8 s, interpolación
  con amortiguación crítica, microsacadas de ±3 px y parpadeo cada 2,5–7 s (≈180 ms, a veces doble).
- El estado solicitado se publica con una variable atómica; solo el temporizador toca la escena.
- El simulador ejecuta el mismo controlador C; su backend de PC recorre los estados para la demostración.

## 4. Clave de API: almacenamiento y protecciones

**Almacén:** partición `nvs` propia de la aplicación (0x200000), espacio de nombres `cog_secret` (hasta 1.6, `vox_secret`; se migra solo al arrancar),
claves `api_key` (≤ 128 B) y `model` (opcional). Es distinta de `sysmeta/shared` de Vibe Mode.

**Provisión privada tras la instalación:** RPC binaria de ESP-Iris registrada por `app_secrets`
(servicio `0x5658` «VX»):

| Método | Uso | Respuesta |
|---|---|---|
| 1 `set_api_key` | Carga útil = clave (ASCII, 30–128 B) | `ok` / error; nunca devuelve la clave |
| 2 `clear_api_key` | Borra la clave | `ok` |
| 3 `status` | — | `{"api_key":true|false,"model":"…"}` |
| 4 `set_model` | `gemini-3.8-live` o `gemini-3.1-flash-live-preview` (vacío = predeterminado) | `ok` |

`tools/provision_key.py` lee la clave de la variable `GEMINI_API_KEY`, de un `.env` local
(ignorado por Git) o de un `getpass`, y llama a `mosaico.py iris rpc` por USB.
No existe ningún método para leer la clave desde fuera.

**Protecciones evaluadas (no se activa nada irreversible):**

| Mecanismo | ¿Reversible? | Estado |
|---|---|---|
| NVS propio de la app, fuera de la información compartida | Sí | **Activo** |
| Sin registros ni RPC de lectura; búferes borrados con `mbedtls_platform_zeroize` | Sí | **Activo** |
| Clave restringida en Google Cloud (solo Generative Language API, cuota y alertas) | Sí | **Recomendado al propietario** |
| Cifrado NVS basado en HMAC (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`) | **No**: graba una clave HMAC en eFuse | Requiere autorización expresa |
| Cifrado de flash (`CONFIG_SECURE_FLASH_ENC_ENABLED`) + cifrado NVS | **No**: quema eFuses y afecta a Vibe Mode y al prefijo retenido | Fuera del alcance |
| Secure Boot v2 | **No** | Fuera del alcance |

Riesgo residual: sin cifrado de flash, quien tenga acceso físico y lea la flash puede extraer la clave.
Mitigación: clave exclusiva, restringida y revocable desde Google Cloud.

## 5. Riesgos

Instalación, datos conservados, restauración y auditoría de la clave: [INSTALL.md](INSTALL.md).

- **Tamaño frente a `vibe_mode` (0x1C0000):** la app ya ocupa 2,04 MB y ESP-IDF solo avisa
  (va en `main_app`, de 13 MiB). System Update comprueba el tamaño contra `main_app`; no se tocan particiones.
- **Bootloader:** excluido de `ALL` en `CMakeLists.txt` (patrón oficial de los juegos de mosaico-tools).
  Las apps normales usan el bootloader retenido.
- **La ruta del espacio de trabajo tiene espacios:** se compila con `IDF_CONFIGDEP_ENABLE=0`.
- **API Live en preview:** los nombres de modelo y la forma de `setup` pueden cambiar.
  Hay informes de turnos sin audio con `gemini-3.8-live` por WebSocket crudo; por eso existe el modelo alternativo.
- **Eco:** semidúplex en v1; el AEC por software (esp-sr AFE, formato «MR») queda para la fase 2.

## 6. Plan incremental

1. ✅ Entorno, compilación y simulador.
2. ✅ Wi-Fi, secretos y RPC de provisión, cliente Gemini Live, audio y ojo (compilado y en el simulador).
3. ✅ Primera prueba física (2026-11-24): Cogitador conversa con Gemini Live directamente por Wi-Fi.
   Se corrigieron la RAM interna de la pantalla y el modelo; detalles en [INSTALL.md](INSTALL.md).
4. Ajustes: ganancia del micrófono de 30 a 24 dB (saturaba), vigilante de modelo mudo,
   y después latencia, VAD, pruebas de larga duración y reanudación.
5. ✅ **Activación por voz «Hola ESP»** (sección 7): versión 1.1.0, validada en el hardware
   (INSTALL.md, pruebas 6–9), fusionada en `main`.
6. AEC por software y conversación full-duplex (siguiente fase; ver HANDOFF.md §6).
7. **Google Search nativo** (1.2.0, rama `feat/google-search`): herramienta `googleSearch` en el
   `setup`, `groundingMetadata` y partes sin audio, respuesta pendiente que suspende el reposo,
   sugerencias y fuentes en la línea de estado. Diseño, cumplimiento, cuotas y costes en
   [GOOGLE_SEARCH.md](GOOGLE_SEARCH.md). Pendiente de validar en el dispositivo.
8. Ampliaciones futuras sin implementar (cámara bajo demanda, Gemini 3.8 Extended Thinking, base
   magnética de 5 V): [ROADMAP.md](ROADMAP.md).
   Plan por fases basado en proyectos de referencia: [RESEARCH.md](RESEARCH.md).
9. ✅ **Memoria DMA de la pantalla** (1.3.0): copia del BSP con `trans_queue_depth = 2`.
   El bloque libre más grande pasó de 8 192 B a 63 488 B estables (INSTALL.md, prueba 11).

## 7. Activación por voz «Hola ESP» (implementada y validada en 1.1.0)

Estado: **implementada** (componente `app_wake`, sesión, partición `model`, paquete de System Update).
Compila con esp-sr 2.6.2 (app 2,51 MB, `srmodels.bin` 427 911 B) y el preparador oficial acepta el
paquete con la partición `model`. Queda validar en el dispositivo: que Vibe Mode acepte la tabla
nueva, la tasa de aciertos y falsos disparos, la memoria y el pre-roll.

Implementación respecto al plan de 7.4: el detector se crea al entrar en reposo y se destruye al
abrir la conversación (no compite por RAM interna con el TLS); el plazo de inactividad (15 s, decidido por el propietario el 2026-11-24) se aplica a todas las
sesiones, se abran por voz o por toque; y ya no se envía `audioStreamEnd` al empezar a hablar el modelo,
porque chocaba con el audio entrante.

### 7.1 Requisitos

| Requisito | Decisión |
|---|---|
| Palabra de activación | «Hola ESP», modelo preentrenado de Espressif `wn10_es_holaesp` (ESP-SR, WakeNet10). Sin palabra personalizada |
| Procesamiento | 100 % local en el ESP-Mosaico. En reposo **no hay WebSocket abierto** y no sale ningún audio a Gemini ni a la red |
| Tras la activación | Se abre la sesión Gemini Live; Gemini gestiona las pausas y los turnos (VAD del servidor), como con el toque |
| Vuelta al reposo | Tras **15 s de inactividad** (antes 30 s; reducido a petición del propietario) se cierra la sesión y se vuelve a la escucha local |
| Alternativa | Se conserva el despertar por toque; el toque sigue abriendo, cerrando e interrumpiendo como hoy |
| Ojo | En reposo, cian (con escucha local activa); al detectar «Hola ESP», parpadeo de confirmación y ámbar mientras conecta |

«Inactividad» significa que no llega ninguna transcripción del usuario ni audio del modelo,
no hay ningún toque y no hay reproducción en curso. Cada uno de esos eventos, y también la voz detectada localmente, reinicia el plazo de 15 s.
El plazo no corre mientras Cogitador habla.

### 7.2 Compatibilidad verificada (2026-11-24)

| Aspecto | Resultado | Evidencia |
|---|---|---|
| Existencia del modelo | Sí: `model/wakenet_model/wn10_es_holaesp`, Kconfig `CONFIG_SR_WN_WN10_ES_HOLAESP` | Paquete oficial `espressif/esp-sr` 2.6.2 (registro de componentes) |
| Soporte ESP32-S31 | Biblioteca precompilada `lib/esp32s31` (`libwakenet.a`, AFE). ESP-SR lo declara «soporte preliminar» desde 2026-05 | README y árbol de esp-sr 2.6.2 |
| ESP-IDF 6.2 (`7b9cc1ac79f8`) | **Compila**: copia temporal del ejemplo `voice_wake_cmd` del BSP con esp-sr 2.6.2 y `wn10_es_holaesp`, objetivo esp32s31, `BUILD_EXIT=0` | Compilación local de prueba (directorio temporal ya eliminado) |
| Versión mínima | Usar esp-sr **≥ 2.6.2** (2.6.1 corrige núcleos de WakeNet10; 2.5.0 corrige Kconfig con IDF ≥ 6.1) | CHANGELOG de esp-sr |
| Tamaño del modelo | Dos variantes cuantizadas a 8 bits (p1 375 KB, p2 418 KB); el empaquetador incluye **417,76 KB** | Salida de `movemodel` en la compilación de prueba |
| RAM y CPU en S31 | WakeNet10 cuantizado, 3 canales: **16 KB de RAM interna, 523 KB de PSRAM, 6,4 ms por trama de 32 ms, 19,9 % de un núcleo**. Con 1 canal se espera menos; no hay cifra publicada | Benchmark oficial de ESP-SR para esp32s31 |
| Entrada | PCM 16 kHz, mono, 16 bit (trama de 32 ms = 512 muestras). Ya la producimos: `app_audio` entrega 16 kHz mono en tramas de 640 | Documentación de WakeNet |
| Tamaño de la app | El ejemplo completo (WakeNet + MultiNet + TTS) ocupa 1,25 MB; el aumento estimado para Cogitador solo con WakeNet es de +0,3–0,6 MB (por medir). `main_app` tiene margen de sobra | Compilación de prueba |

### 7.3 Almacenamiento: cambio de particiones necesario

esp-sr carga los modelos por mapeo de memoria de una partición llamada **`model`**
(`esp_srmodel_init("model")`). Su CMake genera `srmodels.bin` y solo lo graba con `idf.py flash`,
que este proyecto no usa. Plan:

1. Añadir `model` (data, spiffs) de **1 MiB** dentro de la región de aplicación, sin tocar el
   prefijo inmutable ni `nvs`:

   | Partición | Actual | Propuesta |
   |---|---|---|
   | main_app `0x210000` | 13 248 K | 12 224 K (`0xBF0000`) |
   | model | — | `0xE00000`, 1 M |
   | ui_apps `0xF00000` | 1 M | 1 M (sin cambio) |

2. Registrar `srmodels.bin` como imagen de datos del paquete de System Update
   (`MOSAICO_SYSTEM_UPDATE_DATA_LABELS` con `IMAGE`/`TARGET`, igual que `ui_apps`).
   `prepare_system_update.py` admite particiones de datos en la región de aplicación
   que no sean NVS.
3. Como la tabla cambia, esa instalación **será otro `iris system-update`** (nunca `app-update`).
   Descartado: cargar el modelo desde la NAND o una tarjeta SD (`MODEL_IN_SDCARD` /
   `ESP_SR_EXTERNAL_MODEL_PATH`). Exige montar el sistema de archivos de Vibe Mode
   desde la aplicación y no aporta nada para 418 KB.

### 7.4 Arquitectura prevista

- Nuevo componente `app_wake` (WakeNet10 sin AFE: un solo micrófono, sin referencia AEC).
  Reagrupa tramas de 640 a 512 muestras en un ring propio y detecta en su tarea, en el núcleo 1
  con prioridad inferior a la de audio. La tarea de captura nunca se bloquea.
- `app_cogitador_session`: en CERRADA, el micrófono va **solo** a `app_wake`. Con una detección se llama
  a `open_conversation()` (las mismas comprobaciones de Wi-Fi y clave). WakeNet queda en pausa mientras
  la conversación está abierta, para evitar disparos con el altavoz y ahorrar CPU.
- Pre-roll: se guardan los ~2 s posteriores a «Hola ESP» y se envían a Gemini al recibir
  `setupComplete`, para no perder la pregunta mientras conecta.
- Temporizador de inactividad de 15 s en la sesión (Kconfig `COGITADOR_IDLE_TIMEOUT_S`), con un evento
  de cierre limpio. Kconfig `COGITADOR_WAKE_WORD` para activarlo o desactivarlo.

### 7.5 Riesgos y validación pendiente

- Soporte preliminar de ESP32-S31 en ESP-SR: validar la tasa de aciertos y de falsos disparos en el
  hardware (por ejemplo, 20 activaciones a 1 y 3 m y 1 h de ruido doméstico).
- PSRAM: Cogitador ya reserva ≈ 832 KB (RX WebSocket 256 KB, PCM 192 KB, ring 384 KB) más la UI.
  Medir con `iris memory` en la primera prueba física antes de sumar unos 0,5 MB para WakeNet.
- CPU: unos 20 % de un núcleo en el peor caso publicado. Solo funciona en reposo,
  así que no compite con la conversación.
- Marcas: «Hola ESP» es un modelo de Espressif; revisar su aviso de uso antes de cualquier uso comercial.

## 8. Cómo compilar

El repositorio debe estar en `esp-mosaico-vibe/projects/servocraneo_gemini_direct/`
(usa `../../submodule/esp-mosaico-utils` y `../../submodule/esp-mosaico-bsp`).

```powershell
$env:IDF_CONFIGDEP_ENABLE = "0"
python ../../submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py --project . --idf-path C:\esp\esp-idf-master build
python ../../mosaico.py project sim --project . --interactive
```
