# VOX M-01 GEMINI DIRECT — diseño del cliente Gemini Live nativo

Estado: **v0.2 (2026-11-24)**. Decisiones técnicas aprobadas por el propietario.
Ámbito: solo este proyecto (`projects/servocraneo_gemini_direct/` dentro de un
espacio de trabajo `esp-mosaico-vibe`). Sin Windows, sin puentes ni servicios
intermedios: el ESP-Mosaico abre directamente un WebSocket seguro con Gemini Live por Wi-Fi.

## 1. Requisitos aprobados

### 1.1 Conversación

| Requisito | Decisión |
|---|---|
| Modelo principal | `gemini-3.8-live` |
| Modelo alternativo | `gemini-3.1-flash-live-preview`, configurable sin recompilar |
| Turnos | Micrófono continuo; Gemini detecta las pausas (VAD automático del servidor) |
| Eco (v1) | Micrófono silenciado mientras el altavoz reproduce; interrupción táctil |
| Duración | Sin límite artificial (no hay grabación de 8 s); sesión continua con reanudación |
| Idioma | Español de España (`es-ES`) |
| Voz | `Charon` |
| Personalidad | VOX M-01, servocráneo del Adeptus Mechanicus: solemne, ceremonial, con referencias ocasionales al Omnissiah. Útil y directo con cualquier pregunta legítima, incluidas las matemáticas sencillas |

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
  | Hablando | Verde `#20FF60` | Reproduciendo la voz de VOX |
  | Error | Violeta `#B040FF` | Sin Wi-Fi, sin clave, error de red o de sesión |

- **Un toque en la pantalla abre o cierra la conversación.** Mientras VOX habla, un toque la
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
 ├─ app_vox_session   máquina de estados: une audio ↔ Gemini ↔ ojo y gestiona los toques
 └─ app_vox_eye       UI GSP del ojo mecánico (portable al simulador; sustituye a app_hello)
```

Las dependencias van en una sola dirección. `app_vox_eye` no conoce la red: recibe estados
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

### 3.3 `app_audio`

```
MIC  I2S RX 48k estéreo → canal izquierdo → FIR paso bajo + diezmado ÷3 → 16k mono → callback
SPK  ring 24k (PSRAM, ~4 s) → interpolación ×2 con FIR → 48k → estéreo → I2S TX
```

- Conversores FIR propios con razones enteras (÷3, ×2); el coste es despreciable a 320 MHz.
- Tareas de captura y reproducción en el núcleo 1; la red en el núcleo 0.
- `flush_playback()` vacía el ring al instante (interrupción); el nivel de salida mueve la pupila.

### 3.4 `app_vox_session` — estados y toques

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

### 3.5 `app_vox_eye`

- Escena GSP 480×480: carcasa metálica, anillo luminoso de estado, cuenca oscura, grupo del iris
  (anillo de color, anillo interior, pupila y brillo) con `x/y` dinámicos acotados, párpados
  superior e inferior con `y` dinámico y un botón transparente a pantalla completa.
- Animación con un temporizador GSP de 33 ms: objetivo aleatorio cada 0,7–2,8 s, interpolación
  con amortiguación crítica, microsacadas de ±3 px y parpadeo cada 2,5–7 s (≈180 ms, a veces doble).
- El estado solicitado se publica con una variable atómica; solo el temporizador toca la escena.
- El simulador ejecuta el mismo controlador C; su backend de PC recorre los estados para la demostración.

## 4. Clave de API: almacenamiento y protecciones

**Almacén:** partición `nvs` propia de la aplicación (0x200000), espacio de nombres `vox_secret`,
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

- **Tamaño frente a `vibe_mode` (0x1C0000):** se mide en cada etapa. Si se acerca al límite, se
  consultará el mecanismo de mosaico-tools; no se tocan particiones.
- **Bootloader:** excluido de `ALL` en `CMakeLists.txt` (patrón oficial de los juegos de mosaico-tools).
  Las apps normales usan el bootloader retenido.
- **La ruta del espacio de trabajo tiene espacios:** se compila con `IDF_CONFIGDEP_ENABLE=0`.
- **API Live en preview:** los nombres de modelo y la forma de `setup` pueden cambiar.
  Hay informes de turnos sin audio con `gemini-3.8-live` por WebSocket crudo; por eso existe el modelo alternativo.
- **Eco:** semidúplex en v1; el AEC por software (esp-sr AFE, formato «MR») queda para la fase 2.

## 6. Plan incremental

1. ✅ Entorno, compilación y simulador.
2. Wi-Fi, secretos y RPC de provisión, cliente Gemini Live, audio y ojo (compilado y en el simulador).
3. Primera prueba física (requiere autorización para instalar): Wi-Fi, clave, `setupComplete`,
   voz bidireccional y captura del ojo.
4. Ajustes de ganancia, latencia y VAD; pruebas de larga duración y reanudación.
5. Fase 2: AEC por software y conversación full-duplex.

## 7. Cómo compilar

El repositorio debe estar en `esp-mosaico-vibe/projects/servocraneo_gemini_direct/`
(usa `../../submodule/esp-mosaico-utils` y `../../submodule/esp-mosaico-bsp`).

```powershell
$env:IDF_CONFIGDEP_ENABLE = "0"
python ../../submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py --project . --idf-path C:\esp\esp-idf-master build
python ../../mosaico.py project sim --project . --interactive
```
