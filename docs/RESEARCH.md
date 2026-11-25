# VOX M-01 — Investigación de proyectos de referencia (2026-11-25)

Estudio de cuatro repositorios públicos y de la documentación oficial del kit ESP-Mosaico para
decidir cómo evolucionar VOX M-01. **Solo investigación:**
- no se ha copiado código;
- no se ha modificado la versión estable (`main` = `v1.2.0`, instalada en el dispositivo);
- no se ha tocado ningún submódulo.

Cada propuesta necesita su propia rama, la aprobación del propietario y una prueba en el hardware.

Hardware del propietario: ESP-Mosaico con el **módulo de cámara conectado en la ranura izquierda**,
la única que admite cámara.

## 1. Fuentes y licencias

| Proyecto | Licencia | Plataforma | Qué aporta | Uso permitido |
|---|---|---|---|---|
| [openai/openai-live-esp32](https://github.com/openai/openai-live-esp32), ejemplo `examples/mosaico/` (PR #32, 2026-11-10; último commit 2026-11-22) | **Apache-2.0** (más avisos MIT de libpeer y Mbed TLS) | **ESP-Mosaico / ESP32-S31, mismo ESP-IDF `7b9cc1ac79f8` y mismo BSP `5f88b91`** | Arreglo de la DMA de la pantalla, captura de fotos con liberación de la cámara, consola USB-OTG, pilas en PSRAM, diagnóstico de memoria | Ideas y, con aviso de licencia, código. Experimental, «no soportado oficialmente» |
| [ersan-oz/jamiryo-desktop-buddy](https://github.com/ersan-oz/jamiryo-desktop-buddy) (1.2.20-public1, 2026-11-02) | **MIT** | AtomS3R-CAM (ESP32-S3), IDF 6.0, **Gemini Live directo** | Seguimiento de movimiento local barato para orientar los ojos, foto bajo demanda por `realtimeInput.video`, herramientas, comportamiento del ojo | Ideas y código con aviso MIT. Los modelos ESP-DL tienen su propia licencia. La versión pública no se probó en hardware |
| [d4rkmen/M5Gemini](https://github.com/d4rkmen/M5Gemini) (2026-11-15) | **Ambigua**: no hay fichero LICENSE y el README dice «GNU GPL» sin versión | M5 Cardputer (ESP32-S3) **sin PSRAM**, IDF 5.5, **Gemini Live directo** | Técnicas de memoria (base64 en el mismo búfer), manejo de `goAway` y cierres | **Solo ideas. No copiar nada** |
| [vikramsinghvirdi/Hero-ESP32](https://github.com/vikramsinghvirdi/Hero-ESP32) (fork de xiaozhi-esp32 v2.4.1) | **MIT** | XIAO ESP32-S3; el proyecto base soporta **ESP32-S31 Korvo** | Configuración AFE/AEC de esp-sr, ajustes `sdkconfig` para S31, calibración ADC del S31, cámara DVP en S31 | Ideas y código con aviso MIT. **No usa Gemini** (protocolo Xiaozhi con servidor propio) |
| [Guía del módulo de cámara](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide_camera.html) y [guía de ESP-Mosaico](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html) | Documentación de Espressif | Nuestro kit | Patillaje, sensores, alimentación y batería | Referencia |

Nota: jamiryo usa `gemini-3.8-live` y M5Gemini usa `gemini-3.1-flash-live-preview`. Coinciden con nuestros
dos modelos y con nuestro formato de `setup` (PCM 16 kHz de subida y 24 kHz de bajada,
transcripciones y `sessionResumption`), lo que confirma que nuestra arquitectura de WebSocket directo es
la habitual.

## 2. Hallazgo principal: la DMA de la pantalla (afecta a todo lo demás)

> **Hecho en 1.3.0 (2026-11-25, prueba 11):**
> - Opción 2: copia del BSP en `third_party/esp-mosaico-bsp`, con `tools/check_bsp_fork.py`.
> - El bloque DMA libre más grande pasó de 8 192 B a **63 488 B estables** durante las conversaciones.
> - El ojo sigue fluido.

Nuestro punto débil conocido es este: el ojo se congela si el bloque interno DMA más grande baja de
unos 7,7 KB (HANDOFF §5). El mínimo medido es 8 192 B, tanto con 1.1.0 como con 1.2.0 tras una
conversación.

**openai-live-esp32 sufrió exactamente el mismo problema en ESP-Mosaico.** Con cámara y audio a la
vez, la pantalla «dejaba sin memoria DMA» al Wi-Fi y al AES por hardware.

- **Causa:** el BSP hace cola de **10 transferencias** SPI de la pantalla, y cada una copia su trozo
  desde PSRAM a un búfer intermedio interno DMA (`psram_dma_direct = false`).
- **Arreglo:** `tools/prepare_mosaico.py` parchea el BSP con una sola línea,
  `io_config.trans_queue_depth = 2`, justo después de `psram_dma_direct = false`, y comprueba el
  hash antes y después de aplicarlo.
- **Efecto:** el tope de búferes intermedios baja a unos 16 KiB y se mantiene el refresco sincronizado
  con TE, sin desgarros.
- **Comprobado aquí:** nuestro BSP está en el **mismo commit `5f88b91`**, y su
  `onboard/display.c:263` tiene la misma línea. La profundidad de cola sale del macro
  `CO5300_PANEL_IO_QSPI_CONFIG` y el BSP **no la expone como opción**.
- También indican que reducir la altura de franja no sirve, porque el modo TE fuerza el refresco a
  pantalla completa.

**Por qué importa:** liberaría del orden de 8 búferes de ~8 KB de RAM interna DMA. Es el requisito
previo para añadir la cámara (que también usa DMA), un AEC o más interfaz sin congelar el ojo.

**Cómo aplicarlo sin romper la regla de no tocar submódulos** (decide el propietario):
1. **Proponerlo aguas arriba** a `esp-mosaico-bsp`: exponer `trans_queue_depth` o una opción de
   Kconfig. Es lo más limpio, pero no depende de nosotros.
2. **Sustituir el componente en el proyecto:** ESP-IDF da prioridad a un componente del proyecto con el
   mismo nombre. Sería una copia de `esp-mosaico-bsp` (Apache-2.0) con esa línea, fijada al commit
   `5f88b91`. Habría que mantenerla a mano.
3. **Parche en tiempo de compilación** con comprobación de hash, como hace openai. Modifica el
   submódulo en disco y por eso choca con la regla.

Recomendación: la opción 2 en una rama de prueba y la 1 en paralelo. Medir `largest=` antes y después.

## 3. Captura de imágenes bajo demanda para Gemini Live

**Datos del kit (guía oficial):**

- **Módulo V1.2:** OV3640, 3,2 MP, por defecto 1024×768 UYVY a 30 fps, campo de visión de 68°.
- **Módulo V1.4:** SC101IOT, 1 MP, por defecto 1280×720 UYVY, enfoque manual, 52° o 79°.
- Ambos son DVP de 8 bits, solo en la **ranura izquierda**.
- Dos LED de flash en GPIO34 (activos a nivel bajo) y un LED rojo encendido mientras la cámara funciona.
- Pendiente: mirar la serigrafía del módulo para saber cuál de los dos tenemos.

**Cómo lo hacen los demás:**

- **openai (mismo hardware):** la foto se toma solo cuando el modelo llama a la herramienta, y la
  cámara no se queda abierta. Los pasos son:
  1. `mosaico_camera_open`, iniciar el flujo y tomar un fotograma de 1024×768;
  2. devolverlo, parar y cerrar la cámara;
  3. recortar a 4:3 y reducir a 320×240 por software, en PSRAM y por filas;
  4. codificar en **JPEG por hardware**, bajando la calidad (70 → 50 → 35 → 20 → 10) hasta caber en
     **10 KiB**;
  5. mostrar una vista previa de 2 s en pantalla.
- **jamiryo (Gemini Live):** con la herramienta `look_around`, toma un fotograma 320×240 y lo envía
  en JPEG de calidad 65 (máximo 48 KB) como **`realtimeInput.video`** (`image/jpeg`, base64).
  Limita la frecuencia y no envía mientras hay audio.

**Propuesta para VOX:**

- Frase de activación: «VOX, ¿qué ves?». Puede ser una función propia `capturar_imagen` declarada en el
  `setup` (sección 6) o una orden local.
- Pasos: abrir la cámara, un fotograma, cerrarla, reducir a 320×240, JPEG por hardware con un máximo
  de unos 10–16 KB y enviarlo por `realtimeInput.video` con el TX en PSRAM.
- En el ojo: destello o cambio de color mientras dispara. Sin vídeo continuo y sin guardar imágenes.
- Coste: la imagen de entrada cuesta 1 $ por 1 M de tokens, céntimos al mes con uso normal. En el plan
  gratuito, Google usa los datos.

**Riesgos:**

1. **USB: resuelto en la fase 0.** El driver del BSP desactiva el pad de USB Serial/JTAG
   (GPIO33 = D2 de la cámara) entre `mosaico_camera_new()` y `mosaico_camera_del()`, pero **VOX no usa
   ese USB**:
   - `iris` funciona por el USB-OTG con TinyUSB: `ESP_IRIS_TRANSPORT_USB` con 2 CDC; COM7 control y
     COM6 datos, `VID_303A&PID_4002`;
   - la consola va por UART;
   - el USB Serial/JTAG ni siquiera aparece en Windows.

   Coincide con la guía oficial: la consola del Type-C «no se ve afectada».
2. **DMA.** La captura y el JPEG usan DMA interna. Hay que hacer antes el arreglo de la sección 2, o
   como mínimo medir `largest=` con la cámara abierta.
3. GPIO14 (D4) también selecciona la dirección de la EEPROM izquierda: mientras la cámara está
   reservada no se puede leer esa EEPROM. Es inocuo.
4. El componente de cámara intenta aplicar un **parche a ESP-IDF** al configurar CMake. Hay que revisarlo
   frente a la regla de no modificar el ESP-IDF fijado.

## 4. Seguimiento local de movimiento para orientar el iris

**Referencia: jamiryo (MIT)**, con un algoritmo propio sin ESP-DL ni nube, documentado en
`docs/motion-tracking.md`:

1. Cada fotograma se reduce a una **rejilla de luminancia de 32×24** (768 B), a unos 3–5 Hz.
2. Diferencia con el anterior y resta de la mediana, para anular cambios de exposición globales.
3. Umbral adaptativo al ruido. Si cambia más del 45 % de las celdas, el fotograma se descarta.
4. Se agrupan las regiones (vecindad de 8) y se descartan las de menos de 6 celdas. Se calcula el
   centroide x en −1000…+1000. Si hay dos regiones parecidas y alejadas, el resultado es «ambiguo».
5. Se confirma solo tras **dos intervalos seguidos**. Se ignoran fotogramas mientras el robot se
   mueve (IMU) o habla.
6. **Los ojos se adelantan 180 ms a la cabeza**, con zona muerta central y suavizado.

Coste de CPU despreciable (miles de operaciones por fotograma); el autor no lo midió. jamiryo también
detecta y reconoce caras con ESP-DL. Es más caro, necesita modelos en flash y su autor lo considera
experimental.

**Encaje en VOX:** nuestro iris ya se mueve dentro de la escena GSP (posición x/y con límites). El
centroide (x, y) se puede traducir a la posición del iris con suavizado. No hay servo, solo ojo.

**Obstáculo importante: la cámara tendría que quedarse abierta.** Eso implica:
- USB Serial/JTAG apagado mientras dure el seguimiento (no afecta a `iris`, que usa USB-OTG: fase 0);
- DMA y PSRAM ocupadas de forma continua;
- LED rojo encendido;
- más consumo, con una batería de solo 65 mAh.

**Propuesta:** seguimiento solo en reposo, a baja resolución (por ejemplo 160×120 o la mínima del
sensor) y a 3–5 Hz. Se apagaría al abrir la conversación, se activaría con alimentación externa
(sección 7) y se podría desactivar con Kconfig. Solo después del arreglo de la sección 2 y con la
cuestión del USB resuelta.

## 5. Cancelación de eco e interrupción por voz

| Proyecto | AEC | Interrupción |
|---|---|---|
| openai (Mosaico) | **Ninguno**. ES8311 con `no_dac_ref = true`, sin esp-sr. Micrófono siempre abierto (dúplex completo) | Lo resuelve el servidor por WebRTC. No vacía la reproducción. Su README reconoce que las interrupciones «necesitan más ajuste» |
| jamiryo | Ninguno. Semidúplex con puerta de energía local | Ninguna (`NO_INTERRUPTION`) |
| M5Gemini | Ninguno. Semidúplex | Tecla local: para y vacía la reproducción. No avisa al servidor |
| Hero / xiaozhi | AEC con esp-sr AFE (`AFE_TYPE_VC`, `AEC_MODE_VOIP_HIGH_PERF`, VAD y memoria en PSRAM) **solo en placas con canal de referencia por hardware**. Su driver ES8311 fuerza `input_reference_=false`. Sin referencia, semidúplex | Con AEC, dúplex completo, y la palabra de activación puede interrumpir |

**Conclusiones:**

- Ninguno de los proyectos con Gemini o con nuestro hardware tiene AEC. Nuestro semidúplex con
  interrupción por toque es igual o mejor que el de todos ellos.
- Nuestro BSP también fija `no_dac_ref = true` en `onboard/audio.c`, así que **no hay referencia por
  hardware**.

**Opciones para interrumpir hablando:**

1. **Referencia por software** (recomendada para investigar): nosotros conocemos exactamente las
   muestras que enviamos al altavoz.
   - Se pasan a esp-sr AFE como canal de referencia (formato «MR») con un retardo fijo medido, en el
     mismo reloj I2S.
   - No toca el BSP. Riesgos:
     - la alineación (el AEC tolera poca deriva);
     - la RAM interna del AFE (pedir `AFE_MEMORY_ALLOC_MORE_PSRAM`);
     - el coste de CPU.
2. **Referencia por hardware:** inicializar el ES8311 desde `app_audio` con `no_dac_ref = false` en vez
   de usar el inicializador del BSP. Es un cambio de nuestra aplicación, pero hay que verificar el
   cableado (el canal derecho del ADC como referencia).
3. **Intermedia y barata:** dejar el semidúplex y añadir **«Hola ESP» como palabra de interrupción**
   mientras habla, como el modo «wake word in listening» de xiaozhi.
   - WakeNet es más robusto que un VAD frente al eco de su propia voz, pero hoy está desactivado al
     hablar por la memoria DMA.
   - Viable tras el arreglo de la sección 2.

## 6. Herramientas: búsqueda y funciones personalizadas

- **Google Search:** ya está implementado en VOX 1.2.0 (opcional y apagado) y bloqueado por la cuota
  del plan gratuito (GOOGLE_SEARCH.md e INSTALL.md, prueba 10).
  - Ninguno de los proyectos usa grounding en Gemini.
  - openai usa la búsqueda alojada por OpenAI.
- **Funciones propias** (`functionDeclarations` en `setup.tools`, `toolCall` y `toolResponse`). Ideas de
  jamiryo y openai para VOX:
  - `cambiar_emocion` (color e iris del ojo; NON_BLOCKING);
  - `capturar_imagen` (sección 3);
  - `leer_sensores` (batería, carga, temperatura, IMU, magnetómetros, como `read_device_sensors` de
    openai);
  - `hora_local`;
  - más adelante, recordar notas (jamiryo guarda notas en NVS y las inyecta en el prompt marcadas como
    datos no fiables).
- **Buenas prácticas observadas:**
  - Esquemas estrictos y validación de argumentos (openai: objeto plano, máximo 512 B, claves exactas,
    rangos).
  - Devolver el resultado real («queued=true»), no fingir éxito.
  - Ejecutar en una tarea de baja prioridad que se pueda cancelar (`toolCallCancellation`).
  - Diferencias por modelo: `gemini-3.1-flash-live-preview` solo admite llamadas **síncronas**;
    `gemini-3.8-live` es **asíncrono** por defecto (hay que marcar `behavior: BLOCKING`).
- **Lección de M5Gemini:** reintenta cualquier cierre 1011 cada 2 s, lo que con la cuota agotada sería
  un bucle. VOX ya se detiene tras 5 fallos y ahora registra el motivo. Se puede afinar: si el motivo
  menciona cuota, mostrar «CUOTA AGOTADA» sin reintentar.

## 7. Detección de alimentación externa y base magnética

**Datos del kit (guía oficial):**

- **Batería de solo 3,7 V / 65 mAh**, con cargador TP4057 a 65 mA. LED naranja mientras carga y blanco
  al terminar. Indicador BQ27220 en I2C 0x55.
- Pueden alimentar la placa el Type-C, el **pad trasero de 5 V** («también puede alimentar el
  dispositivo»), la batería y VIN (pin 17 de cada cabecera, que también carga la batería).
- Los pads traseros llevan la serigrafía GND, BOOT, RST, RX, TX, 5V, GND.
- **La guía no menciona contactos magnéticos ni pogo pins** y no describe ninguna lógica de detección de
  alimentación.

**Ningún proyecto estudiado detecta alimentación externa.** Lo más cercano:
- openai solo lee el porcentaje de batería;
- xiaozhi tiene un GPIO de carga y una calibración de ADC para el S31 (los valores brutos van invertidos
  y no hay calibración por hardware), útil si se mide una tensión.

**Propuesta para VOX, sin hardware nuevo:** usar el BQ27220 que ya tiene el BSP (`bsp_battery_read()`).
Con alimentación externa, `current_ma` / `average_current_ma` es positiva (carga) o casi nula con la
batería llena, y `status_flags` indica descarga o carga.

- Si `average_current_ma > umbral` o la batería no está descargando → «con alimentación».
- Usos en VOX: pulso tenue en reposo mientras carga, permitir el seguimiento de la sección 4 solo con
  alimentación, y avisar con batería baja.
- **Hay que validarlo en el hardware:** con la batería llena la corriente puede ser casi 0, así que
  conviene combinarlo con la tensión.

**Base magnética:** la base solo tiene que llevar 5 V y GND a los pads traseros. Hay que medir:
- si el pad de 5 V depende de GPIO60 (la guía no lo dice);
- que no realimente el USB del PC (diodo);
- la polaridad mecánica;
- que los imanes no saturen los magnetómetros 0x11/0x12, que el BSP usa para detectar contactos
  magnéticos entre placas.

Detalles en ROADMAP.md §3.

## 8. PSRAM y optimización de memoria

| Técnica | Dónde | Estado en VOX | Propuesta |
|---|---|---|---|
| `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` + `RESERVE_INTERNAL=65536` | openai (mismos valores); Hero S31 usa `RESERVE_INTERNAL=98304` | Ya activo (4096 / 65536) | Probar 98304 midiendo `largest=` |
| `MBEDTLS_EXTERNAL_MEM_ALLOC` | openai, jamiryo, Hero | TLS ya en PSRAM | — |
| **Pilas de tareas en PSRAM** (`xTaskCreatePinnedToCoreWithCaps` + `FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`) | openai: todas las tareas de audio y control en PSRAM | Prohibido en nuestra sesión porque lee NVS (aserción con la caché desactivada) | Solo para tareas que **nunca** toquen flash ni NVS (red y JSON). Medir |
| **Callback de reserva fallida** (`heap_caps_register_failed_alloc_callback`) + diagnóstico de memoria | openai | Tenemos `diag` periódico | Añadir el callback: registra la última reserva fallida con tamaño y capacidades |
| Base64 decodificado **en el mismo búfer** recibido; JSON con prefijo/sufijo fijos | M5Gemini (sin PSRAM) | Ya usamos prefijo/sufijo. Decodificamos a otro búfer en PSRAM | Solo ideas (licencia). No hace falta: tenemos PSRAM de sobra |
| Búferes Wi-Fi más pequeños (RX estático 3, dinámico 6, ventana BA 3) | Hero S31 | Wi-Fi y LWIP ya en PSRAM | Evaluar si libera DMA interna |
| `trans_queue_depth = 2` en la pantalla | openai | — | **Sección 2: lo más importante** |

## 9. Propuesta por fases

Cada fase va en su propia rama, con compilación, simulador si aplica, autorización e instalación con
`iris system-update`, prueba física y fusión solo cuando lo pida el propietario.

| Fase | Contenido | Requisitos | Riesgo |
|---|---|---|---|
| ~~**0. Verificaciones de solo lectura**~~ **Hecha (§9.1)** | Qué USB usa `iris` (Type-C con USB-OTG o USB Serial/JTAG). Versión del módulo de cámara por la serigrafía. Lectura del BQ27220 con y sin USB (por un log temporal) | Nada | Nulo |
| ~~**1. Memoria DMA**~~ **Hecha: 1.3.0, prueba 11** | `trans_queue_depth = 2` sustituyendo el componente del BSP en el proyecto (opción 2 de la sección 2), callback de reserva fallida y, quizá, `RESERVE_INTERNAL=98304` | Decisión del propietario sobre el BSP | Bajo si se mide; mantenimiento de la copia del BSP |
| ~~**2. Cámara bajo demanda**~~ **Hecha: 1.4.0, prueba 12** (la causa del fallo inicial fue el USB Serial/JTAG) |

Añadido a petición del propietario (1.5.0, `feat/gallery`): almacenamiento de las 10 últimas fotos en la partición `photos`, contador en el bisel y galería con X para volver al ojo. Diseño confirmado con una maqueta en el simulador. Función `capturar_imagen`, captura única con apertura y cierre, JPEG por hardware ≤ 16 KB, `realtimeInput.video`, destello en el ojo | Fase 1; resolver el USB (fase 0) | Medio (USB, DMA) |
| **3. Funciones propias** | `cambiar_emocion`, `leer_sensores`, «CUOTA AGOTADA» sin reintentos; Google Search se reactiva si hay cuota | — (independiente; puede ir antes que la 2) | Bajo |
| **4. Alimentación** | Detección por BQ27220, indicación en reposo y aviso de batería baja. Después, la base magnética (hardware) | Fase 0 | Bajo (software); medio (hardware) |
| **5. Interrupción por voz** | Primero «Hola ESP» durante la respuesta. Después, AEC con referencia por software en esp-sr AFE | Fase 1 | Alto (CPU, RAM interna, alineación) |
| **6. Seguimiento de movimiento** | Rejilla 32×24, iris orientado, solo en reposo y con alimentación externa | Fases 1, 2 y 4; USB resuelto | Alto (USB apagado, consumo, DMA) |

**Reutilización de código:**
- **openai** (Apache-2.0), **jamiryo** (MIT) y **xiaozhi/Hero** (MIT): se puede reutilizar código
  conservando el aviso de licencia en las cabeceras y en un `THIRD_PARTY_NOTICES`.
- **M5Gemini**: solo ideas.
- Los modelos ESP-DL y los componentes de Espressif tienen sus propias licencias.

## 9.1 Resultados de la fase 0 (2026-11-25, solo lectura)

| Verificación | Resultado |
|---|---|
| USB de `iris` | **USB-OTG con TinyUSB**, no USB Serial/JTAG. Windows ve `VID_303A&PID_4002` compuesto (serie `1C2904D11390`) con COM7 (MI_00, control) y COM6 (MI_02, datos). `sdkconfig`: `ESP_IRIS_TRANSPORT_USB=y`, `TINYUSB_CDC_COUNT=2`, consola en UART0, sin consola secundaria en USB Serial/JTAG. **La cámara no corta la instalación ni los logs**: el riesgo principal de las fases 2 y 6 desaparece (salvo comprobarlo con la cámara activa) |
| Versión del módulo de cámara | **V1.4 = SC101IOT** (1 MP, 1280×720 UYVY por defecto, enfoque manual). El módulo viene encapsulado, pero indica «rotate clockwise to focus far», y solo el V1.4 tiene enfoque manual. La primera compilación de la fase 2 lo confirmará por SCCB |
| Batería y alimentación (BQ27220) | **No se puede leer sin escribir en el dispositivo**: VOX no inicializa el indicador e `iris device-status` no informa de batería. Hace falta una compilación de diagnóstico que escriba `bsp_battery_read()` en la línea `diag`, con instalación autorizada. Se propone como primer paso de la fase 4 |

## 10. No verificado

- ~~Que `trans_queue_depth = 2` baste con nuestras reservas propias~~: verificado en la prueba 11
  (63 488 B estables con WakeNet, TLS y audio). Lo que sigue es la nota original: openai solo lo midió
  en sesiones cortas. Medir en el dispositivo.
- ~~Qué puerto USB usa `iris`~~: verificado en la fase 0 (USB-OTG con TinyUSB; la cámara no lo afecta).
- Detalles internos sin comprobar:
  - el comportamiento de interrupción de openai (no se leyó entero `live_esp32.cpp`, 69 KB);
  - el coste de CPU del seguimiento de jamiryo;
  - si el `esp_video` de xiaozhi funciona con el SC101IOT.
- La detección de alimentación por el BQ27220 con la batería llena, y si el pad trasero de 5 V depende
  de GPIO60.
- Las licencias son las del 2026-11-25. M5Gemini no tiene fichero de licencia.
