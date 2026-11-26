# Cogitador M-01 — Ampliaciones futuras (no implementadas)

Registro de ideas aprobadas para estudio. **Nada de este documento está implementado** en 1.2.0.
Cada punto necesita diseño propio, la confirmación del propietario y su propia rama.
Notas redactadas el 2026-11-25.

## 1. Cámara oficial con capturas bajo demanda

**Objetivo.** Que Cogitador pueda «mirar» cuando el usuario lo pida («Co, ¿qué ves?»): una única
fotografía enviada a Gemini Live. Sin vídeo continuo y sin capturas en segundo plano.

**Hardware y software disponibles** (BSP `esp-mosaico-bsp`, componente `mosaico_module_camera`):

- Submódulo de cámara DVP OV3640 o SC101IOT, solo en la **ranura izquierda**. Se detecta y se
  reserva automáticamente (EEPROM 0x50) y se expone como `/dev/video2`.
- API: `mosaico_camera_new/open/start_stream/get_frame/return_frame/stop_stream/close/del`.
  Los fotogramas son UYVY con dos búferes. Hay codificación y decodificación JPEG por hardware y
  rotación con el PPA.
- El componente aplica en CMake un **parche a ESP-IDF** (estabilidad de captura DVP). Hay que evaluarlo
  frente a la regla de no modificar el ESP-IDF fijado.

**Protocolo.** Una imagen JPEG por petición en `realtimeInput.video` (`image/jpeg`, base64). La imagen
de entrada cuesta 1,00 $ por 1 M de tokens, del orden de 0,0003 $ por foto (por medir). Encaja con la
búsqueda: «¿qué es esto?» y luego Google.

**Riesgos que hay que resolver antes:**

1. **USB.** En el ejemplo oficial `camera_photo_app`, **GPIO33 es compartido entre la cámara (D2) y
   USB Serial/JTAG**, que es el enlace que usa `mosaico.py iris` (instalación, logs, provisión de la
   clave). Hay que verificar si la cámara inutiliza la consola USB de Cogitador y cómo seguir instalando
   y diagnosticando (UART, o liberar el pin al cerrar la cámara).
2. **DMA interna.** La captura DVP y el JPEG usan DMA. Hay que medir el bloque interno más grande
   (`largest=` ≥ 8192, ver HANDOFF §5) con la cámara abierta durante una conversación. Abrir la
   cámara solo para la captura y cerrarla después.
3. **Privacidad.** Captura solo por orden explícita (toque largo u orden de voz confirmada) y aviso en
   el ojo (por ejemplo, destello blanco) antes de disparar. No guardar imágenes en flash y no enviar
   nada en reposo.
4. **Reinicio.** El BSP avisa de que el DMA de la cámara o de la pantalla puede quedar activo tras un
   reinicio por software. Hay que seguir su secuencia de parada.
5. **Rendimiento.** Base64 de un JPEG de 30–60 KB en un único mensaje de texto. Usar búfer TX en PSRAM y
   no bloquear el audio de subida.

## 2. Gemini 3.8 Extended Thinking

**Qué es.** `gemini-3.8-live-extended-thinking` es un **modelo Live distinto**, no un modo de
`gemini-3.8-live`. Razona en segundo plano mientras habla con frases de relleno («Déjame comprobarlo…»).
Más detalles en la [ficha oficial](https://aistudio.google.com/docs/models/gemini-3.8-live-extended-thinking):

- `thinkingLevel` admite `low`, `medium` y `high`; `minimal` se rechaza.
- `includeThoughts: true` devuelve resúmenes del razonamiento.
- Admite Google Search.
- Las funciones deben ser asíncronas (`NON_BLOCKING`); `SILENT`, `WHEN_IDLE` e `INTERRUPT` no se admiten.
- `turnComplete` llega con `interactionStatus: IN_PROGRESS` en cada relleno. El final real lo marca `IDLE`.
- Mismo precio por token que los otros modelos Live; el pensamiento cuenta como salida.

**Qué ya está preparado en 1.2.0.** `interactionStatus` (`IN_PROGRESS` no cierra el turno), partes
`thought` sin reproducir y el estado de respuesta pendiente con su límite de 30 s
(`VOX_RESPONSE_TIMEOUT_S`, que podría necesitar 60 s o más con razonamientos largos).

**Pendiente.**

- Decidir si es un tercer modelo, por ejemplo solo bajo petición («piensa a fondo…»), o sustituye al
  alternativo.
- Enviar `thinkingConfig` **solo** a los modelos que lo aceptan, porque `gemini-3.8-live` lo rechaza.
- Medir latencia, coste y comportamiento del toque de interrupción durante el razonamiento.
- Revisar el vigilante de modelo mudo: con rellenos hablados no debería saltar, pero hay que comprobarlo.

## 3. Base magnética de alimentación (contactos traseros de 5 V y GND)

**Objetivo.** Que el servocráneo se alimente o cargue al apoyarlo en una base magnética, sin cable
USB, usando los contactos traseros de **5 V y GND** del ESP-Mosaico.

**Antes de diseñar hay que verificar en el hardware** (el BSP no documenta estos contactos; sus
ejemplos magnéticos tratan de detección por magnetómetro y ESP-NOW, no de alimentación):

1. Esquema: a qué raíl va el contacto de 5 V (VBUS, entrada del cargador o salida), si tiene
   protección contra inversión y sobretensión y la corriente máxima admitida.
2. Que la base no realimente el USB del PC conectado a la vez. Hace falta un diodo o un *ideal diode*
   en la base.
3. Polaridad mecánica: imanes con orientación o guía que impidan conectar al revés.
4. Interferencias: los imanes de la base pueden saturar los **magnetómetros** (0x11 y 0x12) que usa la
   detección de contacto magnético del BSP. Además, el BSP excluye superficies ferromagnéticas, así
   que no conviene usar una base de chapa de hierro.
5. Ruido: comprobar que la fuente de la base no mete ruido en el micrófono ni en el altavoz (ES8311).
   Medir el nivel de ruido en los logs `diag` (`peak=` dBFS) en reposo.

**Software (opcional y posterior).** Leer el BQ27220 (indicador de batería) para saber si está
cargando y reflejarlo en el ojo en reposo (por ejemplo, un pulso tenue). No forma parte de esta
versión.
