# Cogitador M-01 1.2.0 — Google Search nativo en Gemini Live

Estado: **implementado y compilado en la rama `feat/google-search` (2026-11-25); pendiente de
validar en el dispositivo** (prueba 10 de [INSTALL.md](INSTALL.md)). La versión estable sigue siendo
la **1.1.0** (etiqueta Git `v1.1.0`, instalada en el dispositivo).

Fuentes consultadas el 2026-11-25. Las páginas de `ai.google.dev/gemini-api/docs/*` redirigen ahora a
`aistudio.google.com/docs/*`:
[Live API: herramientas](https://aistudio.google.com/docs/live-api/tools),
[referencia Live](https://aistudio.google.com/api-reference/live),
[Google Search](https://aistudio.google.com/docs/google-search),
[términos adicionales](https://aistudio.google.com/docs/terms),
[precios](https://aistudio.google.com/docs/pricing),
[límites](https://aistudio.google.com/docs/rate-limits),
[gestión de sesiones](https://aistudio.google.com/docs/live-api/session-management),
[pensamiento en Live](https://aistudio.google.com/docs/live-api/thinking.md) y las fichas de
[`gemini-3.1-flash-live-preview`](https://aistudio.google.com/docs/models/gemini-3.1-flash-live-preview),
[`gemini-3.8-live`](https://aistudio.google.com/docs/models/gemini-3.8-live) y
[`gemini-3.8-live-extended-thinking`](https://aistudio.google.com/docs/models/gemini-3.8-live-extended-thinking).

> **ESTADO (2026-11-25): búsqueda DESACTIVADA por defecto.**
>
> - En la prueba 10, el servidor cerró con código 1011 todos los `setup` que declaraban
>   `googleSearch`, con los dos modelos, y Cogitador no podía conectar.
> - Causa: según `tools/test_live_setup.py`, la **cuota del plan gratuito está excedida** en los
>   dos modelos.
> - Decisión del propietario: búsqueda **opcional, desactivada por defecto** (`VOX_GOOGLE_SEARCH=n`).
>   Para usarla hay que recompilar con `=y` y tener cuota disponible (detalles en INSTALL.md,
>   prueba 10).
>
> **Decisión del propietario tras la prueba 10 (2026-11-25): búsqueda discreta.** Gemini responde
> de inmediato con lo que sabe y solo busca cuando el operador se lo pide («busca…») o cuando
> desconoce un dato reciente (por ejemplo, la última película de Nolan). No cita fuentes ni de
> viva voz ni en pantalla. La línea de estado con sugerencias y fuentes queda desactivada
> (Kconfig `VOX_SHOW_SEARCH_SOURCES=n`). El código se conserva por si se quiere activar. Las
> secciones 1 y 4 describen esa opción; el riesgo de cumplimiento de la sección 4 aumenta al no
> mostrarse nada.

## 1. Qué cambia

- Gemini decide por sí mismo cuándo buscar en Google. La búsqueda se ejecuta **en los servidores de
  Google**: el dispositivo no llama a ningún buscador, no usa servidores intermedios y no responde a
  ninguna llamada de herramienta.
- Los modelos no cambian: `gemini-3.1-flash-live-preview` es el principal y `gemini-3.8-live` el
  alternativo. La documentación marca Google Search como **compatible en los dos**.
- La personalidad añade la regla de búsqueda discreta: responder de inmediato y buscar solo si se
  le pide o si desconoce un dato reciente, sin mencionar fuentes. Una primera versión decía
  «busca ante hechos actuales y cita la fuente», y el propietario notó al Cogitador más lento.
- Opcional (`VOX_SHOW_SEARCH_SOURCES`, **desactivado**): la línea de estado del ojo mostraría primero
  las **sugerencias de búsqueda** («GOOGLE: …») y después las **fuentes** («FUENTE 1/3: AEMET.ES»),
  rotando cada 3 s.
- Mientras Gemini busca o razona, el reposo de 15 s queda **en suspenso** y el ojo se mantiene ámbar.
- La palabra de despertar, el toque (abrir, interrumpir, cerrar), el semidúplex del audio, la
  ganancia, el volumen y el vigilante de modelo mudo funcionan igual que en 1.1.0.

Se puede desactivar sin tocar el código con Kconfig `VOX_GOOGLE_SEARCH=n`.

## 2. Mensaje `setup`

`tools` es un campo de primer nivel de `setup`, hermano de `model`, `generationConfig` y
`systemInstruction`:

```json
{"setup":{
  "model":"models/gemini-3.1-flash-live-preview",
  "generationConfig":{"responseModalities":["AUDIO"],"speechConfig":{"…":"…"}},
  "systemInstruction":{"parts":[{"text":"Eres Cogitador M-01…"}]},
  "tools":[{"googleSearch":{}}],
  "inputAudioTranscription":{},"outputAudioTranscription":{},
  "realtimeInputConfig":{"automaticActivityDetection":{"disabled":false}},
  "contextWindowCompression":{"slidingWindow":{}},
  "sessionResumption":{}
}}
```

La documentación muestra `googleSearch` solo en los SDK (`{ googleSearch: {} }` en JS). La forma
camelCase sigue la convención JSON de los protos, igual que el resto de campos que ya usamos. **La
aceptación del servidor se confirma en la prueba 10** con el log `connected; setup sent (…, google search)`
seguido de `session ready`. Si el servidor rechazara el campo, la sesión no llegaría a
`setupComplete` y el cliente pasaría al modelo alternativo. En ese caso, recompila con
`VOX_GOOGLE_SEARCH=n`.

No se envía `thinkingConfig` (sección 6).

## 3. Mensajes del servidor que ahora se gestionan

| Mensaje / campo | Qué hace Cogitador 1.2.0 |
|---|---|
| `serverContent.modelTurn.parts[].inlineData` | Audio, como antes |
| Partes sin audio: `text`, `thought: true`, `executableCode`, `codeExecutionResult` | No se reproducen. Cuentan como actividad del modelo (`APP_GL_EVT_MODEL_ACTIVITY`) y se registran solo por tipo, a nivel DEBUG y sin contenido. La documentación avisa de que el modelo puede escribir y ejecutar código para buscar |
| `serverContent.groundingMetadata` | `webSearchQueries` (sugerencias) y `groundingChunks[].web.title`/`domain` (fuentes), sin duplicados y acumulados durante la respuesta. `searchEntryPoint.renderedContent` (HTML/CSS) no se puede pintar, así que solo se mide su tamaño. Las `uri` son redirecciones `vertexaisearch…` y no se usan |
| `turnComplete` + `interactionStatus` | `IN_PROGRESS` significa que acaba de terminar una frase de relleno y el modelo sigue trabajando, así que el turno sigue abierto. `IDLE` o sin estado cierra el turno |
| `toolCall` / `toolCallCancellation` | No se esperan, porque Cogitador no declara funciones y la búsqueda es del servidor. Si llegan, se registran y no se responden |
| `usageMetadata` | Se guarda la última y se escribe una línea por turno: `turn usage: prompt=… response=… tool=… thoughts=… total=…`. `tool` son los tokens de los resultados de búsqueda inyectados |

Diagnóstico: la línea `diag` añade `other=` (partes sin audio), `search{q=… grounded=…}` y
`pending=` (respuesta en curso). Con `grounded answer: N search suggestion(s), M source(s)`
(INFO) se sabe si una respuesta usó Google sin escribir en los logs lo que preguntó el usuario.

## 4. Presentación de fuentes (requisito de Google) y cómo lo cumplimos

Los [términos adicionales](https://aistudio.google.com/docs/terms), sección «Grounding with Google
Search», establecen, en resumen:

- Los resultados con grounding se muestran **junto con sus sugerencias de búsqueda** (Search
  Suggestions). Google entrega esas sugerencias como `searchEntryPoint.renderedContent`.
- Solo se muestran al **usuario final que hizo la pregunta**.
- No se modifican ni se mezcla otro contenido con ellos, salvo permiso escrito de Google.
- Prohibido: intersticiales, redirecciones, seguimiento de clics, caché, indexado o entrenamiento con
  ellos.
- El texto de los resultados se puede guardar como historial hasta 2 años. Google conserva
  peticiones y respuestas 30 días.

Los términos **no contemplan dispositivos de voz o sin navegador**. Hay dos consultas abiertas en el
foro oficial de desarrolladores, ambas sin respuesta de Google
([VR/voz](https://discuss.ai.google.dev/t/clarification-on-grounding-with-google-search-display-requirements-for-vr-voice-applications/182458),
[bot de texto](https://discuss.ai.google.dev/t/compliance-help-gemini-grounding-search-suggestions-in-text-only-irc-bot/80732)).
El ESP32-S31 no puede pintar el HTML/CSS de `renderedContent` en una pantalla redonda de 466×466.

Lo que hace Cogitador, como aproximación prudente:

1. **Muestra las sugerencias de búsqueda** (`webSearchQueries`) en la línea de estado con el prefijo
   «GOOGLE:», mientras suena la respuesta y antes de las fuentes. El texto se pasa a mayúsculas y se
   limpia para la fuente del ojo (máx. 24 caracteres, «...» si no cabe).
2. **Muestra las fuentes** (dominios) numeradas en la pantalla.
3. **No modifica la respuesta.** El audio de Gemini se reproduce tal cual; Cogitador solo puede
   interrumpirlo con un toque.
4. **No guarda nada.** Ni sugerencias, ni fuentes, ni respuestas se escriben en flash. Se borran con la
   siguiente pregunta o al cerrar, y en los logs solo aparecen a nivel DEBUG.
5. **Uso personal.** Solo el propietario que pregunta ve y oye el resultado. No hay clics, enlaces
   ni redirecciones.

**Riesgo abierto:** sin pintar `renderedContent`, el cumplimiento estricto no está confirmado. Para un
uso personal es razonable. **Antes de cualquier distribución pública**, pide confirmación a Google o
a asesoría legal, o añade una página o app acompañante que pinte `renderedContent` (sería un servicio
adicional, lo que este proyecto evita).

## 5. Cuotas y costes (consultados el 2026-11-25; compruébalos en AI Studio)

| Concepto | Valor |
|---|---|
| Google Search con modelos Gemini 3 o posteriores | Se cobra **por consulta de búsqueda** que ejecute el modelo, no por petición. Una respuesta puede lanzar varias consultas |
| Gratis | **5 000 consultas al mes**, compartidas entre todos los modelos Gemini 3+ del proyecto |
| Después | **14 $ por 1 000 consultas** (0,014 $ por consulta) |
| Límite por defecto | 1 500 consultas al día por proyecto |
| Audio de entrada (Live 3.1 / 3.8, de pago) | 3,00 $ por 1 M de tokens (≈ 0,005 $/min; ≈ 25 tokens/s) |
| Audio de salida | 12,00 $ por 1 M de tokens (≈ 0,018 $/min) |
| Texto de entrada / salida | 0,75 $ / 4,50 $ por 1 M de tokens. Los resultados de búsqueda inyectados (`toolUsePromptTokenCount`) cuentan como entrada |
| Nivel gratuito | Sin coste, pero Google **usa los datos para mejorar sus productos**. En el nivel de pago no |
| Sesión | La conexión dura unos 10 min (`goAway` y reanudación, ya gestionados). El audio sin compresión se limita a 15 min (Cogitador activa `slidingWindow`). Contexto de 128 k |
| Sesiones simultáneas (API de desarrolladores) | No publicado: la página de límites remite a AI Studio, que pide iniciar sesión. Cogitador solo abre una |

Ejemplo orientativo: 10 preguntas al día con una búsqueda cada una son unas 300 consultas al mes,
dentro del cupo gratuito. Por encima de 5 000 consultas al mes, cada búsqueda cuesta ≈ 0,014 $,
bastante más que un minuto de audio. Para vigilar el gasto, consulta el panel de uso de AI Studio
y la línea `turn usage` de los logs.

## 6. Pensamiento (`thinkingLevel`): comprobado, sin cambios

| Modelo | Opciones | Por defecto | Cogitador 1.2.0 |
|---|---|---|---|
| `gemini-3.1-flash-live-preview` | `generationConfig.thinkingConfig.thinkingLevel`: `minimal`, `low`, `medium`, `high` (en JSON, en mayúsculas en los ejemplos: `"LOW"`). Sin `thinkingBudget`. `includeThoughts` no está confirmado en Live | `minimal` (prioriza latencia) | No envía `thinkingConfig`, así que usa `minimal` |
| `gemini-3.8-live` | Razonamiento integrado. **`thinkingLevel` no se admite**: hay que omitir `thinkingConfig` | — | No lo envía |
| `gemini-3.8-live-extended-thinking` | `low`/`medium`/`high` (`minimal` se rechaza), `includeThoughts` | — | No se usa (ver [ROADMAP.md](ROADMAP.md)) |

Si en el futuro se ajusta `thinkingLevel` para 3.1, el `setup` debe incluirlo **solo** con ese modelo:
el alternativo 3.8 lo rechazaría. Más pensamiento significa más latencia (más silencio antes de
responder) y más tokens de salida.

Nota: la ficha de modelos califica `gemini-3.1-flash-live-preview` de *legacy preview* y recomienda
`gemini-3.8-live`. Se mantiene como principal por decisión del propietario (fue el único que respondió
de forma estable en las pruebas 2–7), pero conviene vigilar su retirada.

## 7. Reposo de 15 s durante búsquedas y razonamiento

Antes, en 1.1.0, se volvía a escuchar a los 15 s sin respuesta (PROCESSING) o a los 8 s de empezar a
hablar si la reproducción se vaciaba. Después corrían los 15 s de reposo. Una búsqueda tras una frase
de relleno («Consultaré los archivos…») podía acabar en reposo y cerrar la sesión.

Ahora la sesión lleva un estado de **respuesta pendiente** (`reply_pending`):

- Se activa con la primera transcripción del usuario o con cualquier salida del modelo: audio, partes
  sin audio, `groundingMetadata` o `turnComplete` con `IN_PROGRESS`.
- Se apaga con `turnComplete` (IDLE), con `interrupted`, con un toque de interrupción, al reconectar o
  al cerrar.
- Mientras está activa, **el reposo no corre**. Si la reproducción se vacía más de 2 s con el turno
  abierto, el ojo pasa de verde a **ámbar** (PROCESSING) en lugar de a rojo.
- Límite de seguridad: si no llega ninguna salida del modelo en **`VOX_RESPONSE_TIMEOUT_S` (30 s por
  defecto)**, se abandona la espera (`no model output for 30 s; reply abandoned`), se vuelve a
  escuchar y a partir de ahí corren los 15 s de reposo.

La interrupción con un toque, los 15 s tras terminar de hablar, el vigilante de modelo mudo y el
cierre con un toque no cambian.

## 8. Memoria interna y DMA

- **Sin reservas nuevas en RAM interna en tiempo de ejecución.** Los árboles cJSON (ya en PSRAM), las
  listas de grounding (2 × 384 B), los elementos de estado (768 B) y los mensajes grandes (búfer RX de
  256 KiB) están en PSRAM.
- Estático (`esp_idf_size`): `.bss` interno de `app_vox_session` **200 B** (unos 44 B más) y de
  `app_gemini_live` 1 B. El código va en PSRAM/flash. DIRAM total: 176 576 B usados (35 %).
- Al arrancar: un mutex de FreeRTOS (≈ 80 B) y unos 40 B más en la estructura del cliente, ambos en
  RAM interna y antes de crear la pantalla. No fragmentan en ejecución.
- La línea de estado ya existía (errores). Cambiar su texto cada 3 s no añade transacciones
  apreciables frente a la animación de 30 fps.
- **Vigilar en la prueba 10:** `largest=` ≥ 8192 en los logs `diag` durante una respuesta con búsqueda,
  y ningún `Display render failed`.

## 9. Pruebas

- **PC:** `tools/host_tests/run.ps1` (MSVC) compila `gl_grounding.c`, `vox_status_text.c` y cJSON
  con stubs, y comprueba lo siguiente. Resultado: `all grounding host tests passed`.
  - Análisis de `groundingMetadata`: duplicados, títulos y `domain`, entradas largas cortadas en
    límite UTF-8, búfer pequeño, metadatos vacíos.
  - Formato de la línea de estado: mayúsculas españolas, ü→U, signos, emojis, corte por palabra
    con «...».
- **Simulador:** escenas con «FUENTE 1/3: ELTIEMPO.ES» y «GOOGLE: MAÑANA EN MADRID» (24 caracteres)
  caben en la línea de estado. Solo 24 letras anchas («W…») se recortarían en los bordes.
- **Compilación:** ESP-IDF `7b9cc1ac79f8`, app 2 512 688 B. Los dos avisos ya conocidos: extensión
  no confiable de ESP-Iris y partición `vibe_mode` pequeña.
- **Dispositivo:** prueba 10 de [INSTALL.md](INSTALL.md), pendiente de autorización.
