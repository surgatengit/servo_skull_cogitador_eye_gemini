// SPDX-License-Identifier: Apache-2.0

#include "app_vox_session.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_audio.h"
#include "app_camera.h"
#include "app_gemini_live.h"
#include "app_photos.h"
#include "app_rol_web.h"
#include "app_secrets.h"
#include "app_wake.h"
#include "app_wifi.h"
#include "cJSON.h"
#include "esp_random.h"
#include "vox_rol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"
#include "vox_status_text.h"

#if CONFIG_VOX_GOOGLE_SEARCH
#define VOX_GOOGLE_SEARCH 1
#else
#define VOX_GOOGLE_SEARCH 0
#endif
#if CONFIG_VOX_CAMERA
#define VOX_CAMERA 1
#define VOX_CAMERA_JPEG_MAX_BYTES ((size_t)CONFIG_VOX_CAMERA_JPEG_MAX_KB * 1024U)
#else
#define VOX_CAMERA 0
#define VOX_CAMERA_JPEG_MAX_BYTES 0U
#endif

static const char *TAG = "vox_session";

#define SESSION_TASK_STACK      8192    /* camera/PPA/JPEG calls run on this task */
#define SESSION_TASK_PRIO       8
#define SESSION_TASK_CORE       0
#define SESSION_QUEUE_LEN       24
#define SESSION_TICK_MS         50
#define RESPONSE_TIMEOUT_US     ((int64_t)CONFIG_VOX_RESPONSE_TIMEOUT_S * 1000 * 1000)
#define SPEAKING_GAP_US         (2 * 1000 * 1000)   /* playback drained, turn still open */
#define CONNECTING_TIMEOUT_US   (45 * 1000 * 1000)
#define SQUELCH_MAX_US          (10 * 1000 * 1000)
#define DIAG_PERIOD_US          (5 * 1000 * 1000)
#define PLAY_TIMEOUT_MS         1500
#define IDLE_TIMEOUT_US         ((int64_t)CONFIG_VOX_IDLE_TIMEOUT_S * 1000 * 1000)
#define PREROLL_FRAMES          (CONFIG_VOX_PREROLL_MS / 40)   /* 40 ms uplink frames */
#define GROUNDING_ITEMS_CAPACITY 384    /* formatted status lines, '\n' separated (PSRAM) */
#define GROUNDING_RAW_CAPACITY  384     /* scratch copy of queries/sources (PSRAM) */
#define GROUNDING_ROTATE_US     (3 * 1000 * 1000)
#define STATUS_CAPACITY         48
#define STATUS_MAX_CHARS        24      /* fits the round panel's bottom chord at 20 px bold */
/* Pending function calls (WebSocket -> session). The model may send several in one
 * message: a secret per player plus a roll, so room for six players and more. */
#define TOOL_SLOTS              8
#define TOOL_ID_CAPACITY        96
#define TOOL_NAME_CAPACITY      32
#define TOOL_CAPTURE            "capturar_imagen"
#define TOOL_DICE               "tirar_dados"
#define TOOL_VOICE              "cambiar_voz"
#define TOOL_ROLE               "modo_rol"
#define TOOL_SECRET             "mostrar_secreto"
#define TOOL_PREPARE            "preparar_tirada"
#define TOOL_STATUS             "estado_jugadores"
#define TOOL_SHEET              "actualizar_ficha"
#define TOOL_SAVE               "guardar_partida"
#define TOOL_LOAD               "cargar_partida"
#define TOOL_ARGS_CAPACITY      6144    /* function call arguments (JSON): the saved-game summary */
#define ROLE_IDLE_TIMEOUT_US    ((int64_t)CONFIG_VOX_ROLE_IDLE_TIMEOUT_S * 1000 * 1000)
#define PLAYER_TEXT_CAPACITY    256     /* one player roll as text (10 dice fit) */
#define NOTES_CAPACITY          1024    /* pending notes for the master (rolls, phone notices) */
#define NOTES_RETRY_US          (500 * 1000)        /* after a failed send */
#define NOTES_EXPIRE_US         (90 * 1000 * 1000)  /* undelivered notes are dropped */
#define NOTICE_PREFIX           "Aviso de VOX (no lo dice un jugador): "
#define GALLERY_IDLE_US         (30 * 1000 * 1000)  /* back to the eye without touches */
#define NOTICE_US               (3 * 1000 * 1000)   /* "FOTOS BORRADAS" on the status line */

static const char PERSONA[] =
    "Eres VOX M-01, un servocráneo del Adeptus Mechanicus al servicio de tu operador. "
    "Hablas siempre en español de España, con tono solemne, ceremonial y preciso, como un "
    "archivo vivo de la Forja. De vez en cuando, sin abusar, haces referencias al Omnissiah "
    "o al Dios Máquina. Ante todo eres útil: respondes con claridad y exactitud a cualquier "
    "pregunta legítima, incluidas operaciones matemáticas sencillas, datos, explicaciones y "
    "conversación cotidiana. Tus respuestas habladas son breves, normalmente de dos a cuatro "
    "frases, salvo que se te pida más detalle. Si no sabes algo, lo reconoces y no inventas."
    " Si te piden cambiar de voz, usa cambiar_voz: Charon y Algenib son voces de chico "
    "(Algenib, más grave) y Kore y Sulafat de chica (Kore, firme; Sulafat, cálida). Si te piden "
    "tirar dados, usa siempre tirar_dados y nunca inventes un resultado."
    " MODO ROL: si te piden jugar una partida de rol, llama a modo_rol con activar a verdadero y "
    "haz de máster. Pregunta cuántos jugadores son, sus nombres y a qué juego quieren jugar; si "
    "no lo saben, propón uno corto. Son principiantes: explica en pocas frases la ambientación y "
    "las reglas básicas, cómo se tira y qué significa el resultado. Al empezar, diles que vas a "
    "facilitarles una dirección para el móvil donde verán su ficha y lo que solo deben saber "
    "ellos, y espera a que entren. VOX te avisa con mensajes que empiezan por «Aviso de VOX», "
    "que no dice ningún jugador: responde a cada uno con una bienvenida muy breve. Empieza "
    "cuando digan que están listos; quien llegue tarde puede entrar después. Si hay una partida "
    "guardada, pregunta antes si quieren una nueva, continuarla o una aventura nueva con los "
    "mismos personajes, y usa cargar_partida. Crea una ficha breve de cada personaje con "
    "habilidades, puntos de vida y equipo, cuéntala en pocas palabras y escríbela con "
    "actualizar_ficha; actualízala cuando cambie algo. Da a cada personaje un secreto con "
    "mostrar_secreto; para quien no tenga móvil usa en_pantalla y pide a los demás que no "
    "miren. Antes de un momento clave consulta estado_jugadores y avisa por su nombre a quien "
    "tenga un secreto sin leer. Si piden guardar, y siempre al terminar, ofrece guardar la "
    "partida con guardar_partida y un resumen completo para poder retomarla. "
    "Nunca digas un secreto en voz alta y haz que los secretos choquen en algún momento. La partida "
    "dura unos diez minutos, tres o cuatro turnos, y si está muy interesante puedes alargarla "
    "dos a cuatro turnos más. Prepara un guion con un giro sorpresa y un desenlace claro, sin "
    "revelarlo. En cada turno narra de forma viva y breve, interpreta a cada personaje con una "
    "voz propia cambiando tono, ritmo y actitud (narrador, villano, criaturas), añade un consejo "
    "de máster de una frase sobre una regla o idea del juego, y termina preguntando qué hace "
    "cada jugador, por orden. Cuando hagan algo arriesgado, di qué habilidad usan y qué número "
    "necesitan. Usa el sistema de dados del juego: suma (1d20, 3d6...), porcentaje (d100 con dos "
    "d10, decenas y unidades; 0 y 0 es 100) o reserva (un dado por punto de habilidad, contando "
    "éxitos). Tira tú con tirar_dados; si un jugador quiere tirar él, prepara antes su tirada con "
    "preparar_tirada, dile cuántos dados tira y espera su resultado: él toca el botón de dados "
    "de la pantalla. Las reglas especiales del juego, como dieces que repiten o unos que restan, "
    "las aplicas tú con los valores que recibes. En el d20, un 20 es crítico y un 1 es pifia: "
    "explícalo la primera vez que salga. Acepta cualquier acción, aunque sea absurda o de humor, y hazla "
    "encajar con consecuencias. Al terminar narra el desenlace y un epílogo, resume cómo queda "
    "cada personaje y qué han aprendido del juego, deja un cabo suelto para otro día y llama a "
    "modo_rol con activar a falso. Todo es hablado: sin títulos, listas ni símbolos."
#if CONFIG_VOX_GOOGLE_SEARCH
    " Responde de inmediato con lo que sabes. Usa la búsqueda de Google solo cuando el "
    "operador te lo pida o cuando no conozcas la respuesta por ser un dato reciente; en ese "
    "caso responde directamente, sin mencionar fuentes ni que has buscado."
#endif
    ;

typedef enum {
    ST_CLOSED = 0,
    ST_CONNECTING,
    ST_LISTENING,
    ST_PROCESSING,
    ST_SPEAKING,
    ST_ERROR,
} session_state_t;

typedef enum {
    ERR_NONE = 0,
    ERR_NO_WIFI,
    ERR_WIFI_UNCONFIGURED,
    ERR_NO_KEY,
    ERR_KEY_REJECTED,
    ERR_LINK,
    ERR_INTERNAL,
} session_error_t;

typedef enum {
    EV_TAP = 0,
    EV_WIFI,
    EV_SECRETS,
    EV_GL,
    EV_AUDIO_START,
    EV_WAKE,
    EV_TOOL,        /* value = tool slot */
    EV_GALLERY,     /* value = app_vox_session_gallery_t */
    EV_ACTION,      /* value = app_vox_session_action_t */
} event_kind_t;

/* Parameters shared by tirar_dados and preparar_tirada. */
#define DICE_PARAMS                                                                         \
    "{\"modo\":{\"type\":\"STRING\",\"enum\":[\"suma\",\"porcentaje\",\"reserva\"],"        \
    "\"description\":\"suma: se suman los dados. porcentaje: d100 con dos d10 (decenas y "     \
    "unidades, 0 y 0 es 100); ignora cantidad y caras. reserva: se cuentan los dados que "    \
    "igualan o superan exito_con.\"},"                                                        \
    "\"cantidad\":{\"type\":\"INTEGER\",\"description\":\"Número de dados, de 1 a 10.\"},"    \
    "\"caras\":{\"type\":\"INTEGER\",\"description\":\"Caras de cada dado: 4, 6, 8, 10, 12, " \
    "20 o 100.\"},"                                                                           \
    "\"exito_con\":{\"type\":\"INTEGER\",\"description\":\"Solo en reserva: valor mínimo "   \
    "para que un dado sea un éxito (por defecto 8 en un d10).\"}},"                           \
    "\"required\":[\"modo\"]"

/* Functions Gemini may call (Live setup functionDeclarations). */
static const char FUNCTION_DECLARATIONS[] =
    "["
#if CONFIG_VOX_CAMERA
    "{\"name\":\"" TOOL_CAPTURE "\","
    "\"description\":\"Toma una fotografía con la cámara de VOX y la envía como imagen para "
    "que puedas ver lo que hay delante. Úsala solo cuando el operador te pida que mires, que "
    "describas lo que ves, que leas algo o que identifiques un objeto. Tras la respuesta, la "
    "imagen es el último fotograma de vídeo que has recibido.\"},"
#endif
    "{\"name\":\"" TOOL_DICE "\","
    "\"description\":\"Tira dados reales con el generador aleatorio de VOX y los muestra "
    "animados en la pantalla. Úsala siempre que haya que tirar dados. Devuelve cada resultado "
    "y, según el modo, el total (con crítico o pifia en un d20), el porcentaje o los éxitos.\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":" DICE_PARAMS "}},"
    "{\"name\":\"" TOOL_PREPARE "\","
    "\"description\":\"Modo rol: prepara la tirada del botón de dados de la pantalla, el que "
    "tocan los jugadores. Úsala antes de pedir a un jugador que tire, por ejemplo 6 dados de "
    "10 en reserva si su habilidad es 6, o porcentaje en un juego de d100. Se mantiene hasta "
    "que la cambies; al empezar la partida es 2d20 en suma.\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":" DICE_PARAMS "}},"
    "{\"name\":\"" TOOL_VOICE "\","
    "\"description\":\"Cambia la voz de VOX. Al terminar tu respuesta la conversación se "
    "reconecta con la voz nueva (se pierde lo hablado).\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"voz\":{\"type\":\"STRING\",\"enum\":[\"Charon\",\"Algenib\",\"Kore\",\"Sulafat\"],"
    "\"description\":\"Charon y Algenib: chico. Kore y Sulafat: chica.\"}},"
    "\"required\":[\"voz\"]}},"
    "{\"name\":\"" TOOL_ROLE "\","
    "\"description\":\"Activa o desactiva el modo rol: más paciencia antes de cerrar la "
    "conversación y botón de dados para los jugadores en la pantalla.\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"activar\":{\"type\":\"BOOLEAN\"}},\"required\":[\"activar\"]}},"
    "{\"name\":\"" TOOL_SECRET "\","
    "\"description\":\"Da a un jugador un secreto que solo debe saber él. Llega a su móvil; si "
    "aún no ha entrado en la página, le espera hasta que entre con su nombre. Con en_pantalla "
    "verdadero se muestra en la pantalla de VOX hasta que la toquen (para quien no tiene "
    "móvil). No lo digas en voz alta.\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"jugador\":{\"type\":\"STRING\",\"description\":\"Nombre del jugador.\"},"
    "\"texto\":{\"type\":\"STRING\",\"description\":\"El secreto, máximo 400 caracteres.\"},"
    "\"en_pantalla\":{\"type\":\"BOOLEAN\",\"description\":\"Verdadero solo para un jugador "
    "sin móvil.\"}},"
    "\"required\":[\"jugador\",\"texto\"]}},"
    "{\"name\":\"" TOOL_STATUS "\","
    "\"description\":\"Modo rol: quién está en la página del móvil, quién tiene un secreto sin "
    "leer y quién tiene ficha, y si hay una partida guardada en VOX.\"},"
    "{\"name\":\"" TOOL_SHEET "\","
    "\"description\":\"Modo rol: escribe o sustituye la ficha de personaje que el jugador ve en "
    "su móvil. Úsala al crear el personaje y cada vez que cambie algo (vida, equipo, "
    "estado). Envía siempre la ficha completa.\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"jugador\":{\"type\":\"STRING\",\"description\":\"Nombre del jugador.\"},"
    "\"ficha\":{\"type\":\"STRING\",\"description\":\"Texto de la ficha, máximo 900 "
    "caracteres, una línea por dato: personaje, clase u oficio, atributos o habilidades con "
    "su valor, puntos de vida actuales y máximos, equipo y notas.\"}},"
    "\"required\":[\"jugador\",\"ficha\"]}},"
    "{\"name\":\"" TOOL_SAVE "\","
    "\"description\":\"Guarda la partida en VOX para continuarla otro día: fichas y secretos "
    "de los jugadores, el tipo de tirada y tu resumen. Sustituye a la partida guardada "
    "anterior. También se puede descargar desde la página del móvil.\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"titulo\":{\"type\":\"STRING\",\"description\":\"Título corto de la partida.\"},"
    "\"juego\":{\"type\":\"STRING\",\"description\":\"Juego o ambientación y sistema de "
    "dados.\"},"
    "\"resumen\":{\"type\":\"STRING\",\"description\":\"Para que tú mismo puedas retomarla, "
    "máximo 4000 caracteres: lo que ha pasado, dónde están, personajes no jugadores, pistas, "
    "giros pendientes, cabos sueltos y tu guion previsto.\"}},"
    "\"required\":[\"titulo\",\"resumen\"]}},"
    "{\"name\":\"" TOOL_LOAD "\","
    "\"description\":\"Carga la partida guardada en VOX: activa el modo rol, devuelve tu "
    "resumen, las fichas y los secretos, y prepara las fichas en los móviles (cada jugador "
    "entra con el mismo nombre).\","
    "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
    "\"modo\":{\"type\":\"STRING\",\"enum\":[\"continuar\",\"mismos_personajes\"],"
    "\"description\":\"continuar: retomar la misma historia con sus secretos. "
    "mismos_personajes: nueva aventura con los mismos personajes; lo vivido es su pasado y "
    "los secretos no se recuperan.\"}},"
    "\"required\":[\"modo\"]}}"
    "]";

typedef struct {
    char id[TOOL_ID_CAPACITY];
    char name[TOOL_NAME_CAPACITY];
    char args[TOOL_ARGS_CAPACITY]; /* JSON object, "{}" when none */
    uint32_t cancelled;     /* atomic: toolCallCancellation for this id */
} tool_slot_t;

typedef struct {
    event_kind_t kind;
    int value;      /* wifi state or GL event type */
    int code;       /* GL close code */
} session_event_t;

/* Singleton. Session-task-only fields: state, error, timers, config.
 * Cross-task flags (WebSocket/mic tasks) use __atomic builtins. */
static struct {
    bool started;
    app_vox_session_config_t config;
    QueueHandle_t queue;
    app_gemini_live_handle_t gl;
    session_state_t state;
    session_error_t error;
    int wifi_state;
    int64_t processing_since;
    int64_t connecting_since;
    int64_t squelch_since;
    int64_t last_diag_us;
    bool turn_done;
    uint32_t voiced_frames; /* atomic: loud uplink frames sent (capture task) */
    uint32_t voiced_base;   /* silent-model watchdog baselines, set at READY */
    uint32_t transcripts_base;
    uint32_t audio_base;
    bool model_swapped;     /* the watchdog swaps at most once per boot */
    uint32_t mic_forward;   /* atomic: forward mic frames to Gemini */
    uint32_t speaking;      /* atomic: first audio of the turn already queued */
    uint32_t squelch;       /* atomic: drop audio of an interrupted turn */
    /* Wake word and inactivity. */
    bool wake_available;
    int64_t last_activity_us;
    uint32_t voiced_seen;   /* voiced_frames value at the last idle check */
    uint32_t wake_feed;     /* atomic: idle mic frames go to app_wake (capture task) */
    /* Pre-roll after "Hola ESP": the capture task writes frames while connecting;
     * the session task drains them at READY after clearing preroll_on and waiting
     * for preroll_busy to drop (single writer, single reader). */
    int16_t *preroll;       /* PREROLL_FRAMES * APP_AUDIO_MIC_FRAME, PSRAM */
    uint32_t preroll_count; /* atomic: frames written (monotonic) */
    uint32_t preroll_on;    /* atomic */
    uint32_t preroll_busy;  /* atomic: writer inside a frame copy */
    /* Pending reply: from the user's turn (first input transcription or model
     * output) until turnComplete. While pending, the idle return is suspended
     * and silence is shown as PROCESSING, so searches and thinking pauses are
     * never mistaken for an abandoned conversation (VOX_RESPONSE_TIMEOUT_S). */
    bool reply_pending;
    int64_t model_progress_us; /* last model output seen by the session task */
    uint32_t model_rx_ms;   /* atomic: time of the latest model output (WebSocket task) */
    uint32_t model_rx_seen; /* model_rx_ms value at the last tick */
    uint32_t activity_queued; /* atomic: one MODEL_ACTIVITY event in the queue at most */
    /* Grounding shown on the status line (session task only). */
    char *items;            /* ITEMS + RAW capacity, PSRAM; items "" when none */
    size_t item_count;
    size_t item_index;      /* rotation position */
    int64_t item_shown_us;
    /* Function calls: the WebSocket task fills a free slot and posts EV_TOOL;
     * the session task runs it and frees the slot (busy flag, atomic). */
    tool_slot_t *tools;     /* TOOL_SLOTS, PSRAM (about 6 KiB) */
    uint32_t tool_busy[TOOL_SLOTS];
    uint32_t stills_sent;   /* diagnostics */
    /* Gallery (session task only): photos are read from flash here, never on
     * the GSP task (its stack may be in PSRAM). */
    bool gallery_open;
    size_t gallery_pos;     /* 0 = oldest .. count-1 = newest */
    int64_t gallery_touch_us;
    uint8_t *photo_buf;     /* PSRAM, app_photos_max_size() */
    int64_t notice_until_us; /* transient status text, 0 = none */
    /* RoleMaster (session task only). */
    char voice[APP_SECRETS_VOICE_CAPACITY]; /* voice of the next/current conversation */
    bool voice_reconnect;   /* cambiar_voz: reconnect once the reply has been spoken */
    bool role_mode;
    /* Player rolls (D20 badge) not yet delivered to the master: the WebSocket
     * client is busy while the model streams a reply, so they wait for
     * turnComplete and are retried until sent or expired. */
    vox_rol_spec_t player_roll; /* what the dice badge rolls (preparar_tirada) */
    char notes[NOTES_CAPACITY];
    int64_t notes_since_us;  /* first pending note */
    int64_t notes_retry_us;  /* earliest next attempt */
} s;

/* Last failed heap allocation anywhere in the firmware (any task or ISR): only
 * plain stores here; the diag line reports it. */
static volatile uint32_t s_alloc_fail_count;
static volatile uint32_t s_alloc_fail_size;
static volatile uint32_t s_alloc_fail_caps;

static void on_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    (void)function_name;
    s_alloc_fail_size = (uint32_t)size;
    s_alloc_fail_caps = caps;
    s_alloc_fail_count++;
}

/* Gallery helpers (defined with the gallery logic below). */
static void update_photo_count(void);
static void gallery_close(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void post(event_kind_t kind, int value, int code)
{
    if (!s.queue) {
        return;
    }
    const session_event_t ev = { .kind = kind, .value = value, .code = code };
    if (xQueueSend(s.queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full; dropped kind %d", (int)kind);
    }
}

static app_vox_view_t view_for(session_state_t st)
{
    switch (st) {
    case ST_CLOSED:     return APP_VOX_VIEW_IDLE;
    case ST_CONNECTING: return APP_VOX_VIEW_PROCESSING;
    case ST_LISTENING:  return APP_VOX_VIEW_LISTENING;
    case ST_PROCESSING: return APP_VOX_VIEW_PROCESSING;
    case ST_SPEAKING:   return APP_VOX_VIEW_SPEAKING;
    case ST_ERROR:
    default:            return APP_VOX_VIEW_ERROR;
    }
}

static const char *status_for(session_error_t err)
{
    switch (err) {
    case ERR_NO_WIFI:           return "SIN WI-FI";
    case ERR_WIFI_UNCONFIGURED: return "CONFIGURA WI-FI EN VIBE";
    case ERR_NO_KEY:            return "SIN CLAVE";
    case ERR_KEY_REJECTED:      return "CLAVE RECHAZADA";
    case ERR_LINK:              return "ERROR DE CONEXIÓN";
    case ERR_INTERNAL:          return "ERROR INTERNO";
    case ERR_NONE:
    default:                    return "";
    }
}

/* The capture callback runs while any consumer needs audio: Gemini uplink,
 * local wake-word listening or the post-wake pre-roll. */
static void update_audio_gate(void)
{
    const bool on = __atomic_load_n(&s.mic_forward, __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&s.wake_feed, __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE);
    app_audio_set_mic_enabled(on);
}

static void set_mic(bool on)
{
    __atomic_store_n(&s.mic_forward, on ? 1U : 0U, __ATOMIC_RELEASE);
    update_audio_gate();
}

/* Local "Hola ESP" listening only while the conversation is closed: no WebSocket
 * exists then, and nothing leaves the device. */
static void set_wake_listening(bool on)
{
    on = on && s.wake_available;
    __atomic_store_n(&s.wake_feed, on ? 1U : 0U, __ATOMIC_RELEASE);
    app_wake_set_active(on);
    update_audio_gate();
}

static void set_preroll(bool on)
{
    if (on) {
        __atomic_store_n(&s.preroll_count, 0U, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s.preroll_on, (on && s.preroll) ? 1U : 0U, __ATOMIC_RELEASE);
    update_audio_gate();
}

/* Send the frames captured since "Hola ESP" before live audio (session task). */
static void flush_preroll(void)
{
    if (!__atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&s.preroll_on, 0U, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s.preroll_busy, __ATOMIC_ACQUIRE)) {
        vTaskDelay(1);
    }
    const uint32_t count = __atomic_load_n(&s.preroll_count, __ATOMIC_ACQUIRE);
    const uint32_t n = count < PREROLL_FRAMES ? count : PREROLL_FRAMES;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t slot = (count - n + i) % PREROLL_FRAMES;
        (void)app_gemini_live_send_audio(s.gl, &s.preroll[slot * APP_AUDIO_MIC_FRAME],
                                         APP_AUDIO_MIC_FRAME);
    }
    ESP_LOGI(TAG, "sent %u pre-roll frames (%u ms)", (unsigned)n, (unsigned)(n * 40));
}

/* Status line: an actionable error wins; otherwise the current grounding item
 * (Search Suggestion "GOOGLE: ..." or "FUENTE 1/3: RAE.ES"), rotated every
 * GROUNDING_ROTATE_US; otherwise hidden. */
static void refresh_status(void)
{
    if (!s.config.ui.set_status) {
        return;
    }
    if (s.error != ERR_NONE) {
        s.config.ui.set_status(s.config.ui.ui, status_for(s.error));
        return;
    }
    if (s.role_mode && s.item_count == 0) {
        char address[40];
        char line[STATUS_CAPACITY];
        if (app_rol_web_running() && app_rol_web_address(address, sizeof(address)) == ESP_OK) {
            snprintf(line, sizeof(line), "MÓVIL: %s", address + 7); /* without "http://" */
        } else {
            snprintf(line, sizeof(line), "MODO ROL");
        }
        s.config.ui.set_status(s.config.ui.ui, line);
        return;
    }
    char text[STATUS_CAPACITY] = "";
    const char *p = s.items;
    for (size_t n = s.item_index; p && *p && n > 0; --n) {
        p += strcspn(p, "\n");
        p += (*p == '\n');
    }
    if (p && s.item_count > 0) {
        const size_t len = strcspn(p, "\n");
        if (len < sizeof(text)) {
            memcpy(text, p, len);
            text[len] = '\0';
        }
    }
    s.config.ui.set_status(s.config.ui.ui, text);
}

/* Append one formatted line to s.items; false when it does not fit. */
static bool add_item(const char *prefix, const char *text, size_t len)
{
    char line[STATUS_CAPACITY];
    if (vox_status_text(prefix, text, len, line, sizeof(line), STATUS_MAX_CHARS) == 0) {
        return false;
    }
    const size_t used = strlen(s.items);
    const size_t n = strlen(line);
    if (used + n + 2 > GROUNDING_ITEMS_CAPACITY) {
        return false;
    }
    if (used) {
        s.items[used] = '\n';
    }
    memcpy(s.items + used + (used ? 1 : 0), line, n + 1);
    s.item_count++;
    return true;
}

static void clear_grounding(void)
{
    app_gemini_live_clear_sources(s.gl);
    const bool had = s.item_count > 0;
    if (s.items) {
        s.items[0] = '\0';
    }
    s.item_count = 0;
    s.item_index = 0;
    if (had) {
        refresh_status();
    }
}

/* New groundingMetadata: rebuild the status items. Google's terms show grounded
 * results together with their Search Suggestions, so the queries come first,
 * then the web sources. Content is logged only at DEBUG (it mirrors the user's
 * question). */
static void load_grounding(void)
{
#if !CONFIG_VOX_SHOW_SEARCH_SOURCES
    /* Quiet search (default): count it for the diagnostics, show nothing. */
    ESP_LOGD(TAG, "grounded answer (sources not shown)");
    return;
#endif
    if (!s.items) {
        return;
    }
    char *raw = s.items + GROUNDING_ITEMS_CAPACITY;
    const size_t old_count = s.item_count;
    s.items[0] = '\0';
    s.item_count = 0;
    const size_t queries = app_gemini_live_get_queries(s.gl, raw, GROUNDING_RAW_CAPACITY);
    ESP_LOGD(TAG, "search suggestions: %s", raw);
    for (const char *q = raw; *q;) {
        const size_t len = strcspn(q, "\n");
        (void)add_item("GOOGLE: ", q, len);
        q += len;
        q += (*q == '\n');
    }
    const size_t sources = app_gemini_live_get_sources(s.gl, raw, GROUNDING_RAW_CAPACITY);
    ESP_LOGD(TAG, "sources: %s", raw);
    size_t index = 0;
    for (const char *d = raw; *d; ++index) {
        const size_t len = strcspn(d, "\n");
        char prefix[24];
        if (sources == 1) {
            snprintf(prefix, sizeof(prefix), "FUENTE: ");
        } else {
            snprintf(prefix, sizeof(prefix), "FUENTE %u/%u: ", (unsigned)(index + 1),
                     (unsigned)sources);
        }
        (void)add_item(prefix, d, len);
        d += len;
        d += (*d == '\n');
    }
    if (s.item_count == old_count) {
        return; /* nothing new to show: keep the rotation where it is */
    }
    ESP_LOGI(TAG, "grounded answer: %u search suggestion(s), %u source(s)", (unsigned)queries,
             (unsigned)sources);
    s.item_index = 0;
    s.item_shown_us = esp_timer_get_time();
    refresh_status();
}

static void rotate_grounding(int64_t now)
{
    if (s.item_count < 2 || s.error != ERR_NONE || now - s.item_shown_us < GROUNDING_ROTATE_US) {
        return;
    }
    s.item_shown_us = now;
    s.item_index = (s.item_index + 1) % s.item_count;
    refresh_status();
}

static void enter(session_state_t st)
{
    if (st == s.state) {
        return;
    }
    ESP_LOGI(TAG, "state %d -> %d", (int)s.state, (int)st);
    s.state = st;
    s.last_activity_us = esp_timer_get_time();
    if (st != ST_ERROR) {
        s.error = ERR_NONE;
    }
    if (st != ST_CONNECTING) {
        set_preroll(false);
    }
    set_wake_listening(st == ST_CLOSED);
    set_mic(st == ST_LISTENING || st == ST_PROCESSING);
    if (st == ST_CONNECTING) {
        s.connecting_since = esp_timer_get_time();
    }
    if (st == ST_PROCESSING) {
        s.processing_since = esp_timer_get_time();
        /* The user started a new turn: never keep dropping replies to it. */
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    }
    if (st == ST_SPEAKING) {
        s.turn_done = false;
    } else {
        __atomic_store_n(&s.speaking, 0U, __ATOMIC_RELEASE);
    }
    if (st == ST_CLOSED || st == ST_ERROR || st == ST_CONNECTING) {
        s.reply_pending = false;
    }
    if (s.config.ui.set_view) {
        s.config.ui.set_view(s.config.ui.ui, view_for(st));
    }
    refresh_status();
}

/* The user's turn is over or the model started answering: suspend the idle
 * return until turnComplete (or VOX_RESPONSE_TIMEOUT_S without model output). */
static void begin_reply(void)
{
    if (!s.reply_pending) {
        s.reply_pending = true;
        clear_grounding(); /* a new question: the previous answer's sources go away */
    }
    s.model_progress_us = esp_timer_get_time();
}

static void fail(session_error_t err)
{
    s.error = err;
    s.state = ST_CLOSED; /* force enter() to repaint */
    enter(ST_ERROR);
}

/* The dice badge rolls `spec` from now on. */
static void set_player_roll(vox_rol_spec_t spec)
{
    s.player_roll = spec;
    char badge[8];
    if (s.config.ui.set_role_badge && vox_rol_badge(&spec, badge, sizeof(badge)) > 0) {
        s.config.ui.set_role_badge(s.config.ui.ui, badge);
    }
}

static void set_role_mode(bool on)
{
    if (s.role_mode == on) {
        return;
    }
    s.role_mode = on;
    ESP_LOGI(TAG, "role mode %s", on ? "on" : "off");
    if (on) {
        set_player_roll(vox_rol_spec_default()); /* every game starts with 2d20 */
    }
#if CONFIG_VOX_ROLE_PHONE_PAGE
    if (on) {
        (void)app_rol_web_start(); /* optional: secrets fall back to the screen card */
    } else {
        app_rol_web_stop();
    }
#endif
    if (s.config.ui.set_role_mode) {
        s.config.ui.set_role_mode(s.config.ui.ui, on);
    }
    refresh_status();
}

/* Voice for the next connection; the Gemini client takes it only while
 * disconnected (the open conversation keeps its voice). */
static void use_voice(const char *name)
{
    snprintf(s.voice, sizeof(s.voice), "%s", name);
    (void)app_gemini_live_set_voice(s.gl, s.voice);
    if (s.config.ui.set_voice_initial) {
        s.config.ui.set_voice_initial(s.config.ui.ui, s.voice[0]);
    }
}

static void load_stored_voice(void)
{
    char stored[APP_SECRETS_VOICE_CAPACITY];
    use_voice(app_secrets_load_voice(stored, sizeof(stored)) == ESP_OK ? stored
                                                                     : CONFIG_VOX_GEMINI_VOICE);
}

static void show_notice(const char *text)
{
    if (s.config.ui.set_status) {
        s.config.ui.set_status(s.config.ui.ui, text);
    }
    s.notice_until_us = esp_timer_get_time() + NOTICE_US;
}

static uint32_t hw_rand32(void *ctx)
{
    (void)ctx;
    return esp_random(); /* hardware RNG (true random while the radio is on) */
}

/* Roll with the hardware RNG and show it on the dice panel. */
static void roll(const vox_rol_spec_t *spec, uint16_t *values)
{
    vox_rol_roll(spec, hw_rand32, NULL, values);
    if (!s.config.ui.show_roll) {
        return;
    }
    vox_rol_view_t view;
    vox_rol_view(spec, values, &view);
    app_vox_session_roll_t shown = { .count = view.count, .tumble = view.tumble };
    _Static_assert(sizeof(shown.faces) == sizeof(view.faces), "dice faces");
    memcpy(shown.faces, view.faces, sizeof(shown.faces));
    memcpy(shown.marks, view.marks, sizeof(shown.marks));
    memcpy(shown.title, view.title, sizeof(shown.title));
    memcpy(shown.result, view.result, sizeof(shown.result));
    s.config.ui.show_roll(s.config.ui.ui, &shown);
}

/* Spec from tirar_dados / preparar_tirada arguments. */
static vox_rol_spec_t spec_from_args(const cJSON *args, int default_count, int default_sides)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(args, "modo");
    const vox_rol_mode_t m = vox_rol_mode_parse(cJSON_IsString(mode) ? mode->valuestring : NULL);
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(args, "cantidad");
    const cJSON *sides = cJSON_GetObjectItemCaseSensitive(args, "caras");
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(args, "exito_con");
    return vox_rol_spec_make(m, cJSON_IsNumber(count) ? (int)count->valuedouble : default_count,
                             cJSON_IsNumber(sides) ? (int)sides->valuedouble : default_sides,
                             cJSON_IsNumber(target) ? (int)target->valuedouble : 0);
}

/* Notes for the master (player rolls, phone notices) belong to the open
 * conversation only. */
static void drop_notes(const char *why)
{
    if (s.notes[0]) {
        ESP_LOGW(TAG, "notes not delivered (%s): %s", why, s.notes);
        s.notes[0] = '\0';
    }
}

static void flush_notes(int64_t now);

/* Queue a text turn for the master; it is sent at once if the model is quiet,
 * otherwise when its reply ends (the WebSocket client is busy meanwhile). */
static void queue_note(const char *text, int64_t now)
{
    const size_t used = strlen(s.notes);
    if (used == 0) {
        snprintf(s.notes, sizeof(s.notes), "%s", text);
        s.notes_since_us = now;
        s.notes_retry_us = 0;
    } else if (used + 1 + strlen(text) < sizeof(s.notes)) {
        s.notes[used] = ' ';
        memcpy(s.notes + used + 1, text, strlen(text) + 1); /* fits: checked above */
    } else {
        ESP_LOGW(TAG, "notes queue full; dropped: %s", text);
    }
    flush_notes(now);
}

/* Send the pending notes once the model is not streaming a reply. */
static void flush_notes(int64_t now)
{
    if (!s.notes[0]) {
        return;
    }
    if (now - s.notes_since_us > NOTES_EXPIRE_US) {
        drop_notes("expired");
        return;
    }
    if (now < s.notes_retry_us || s.reply_pending || !app_gemini_live_is_ready(s.gl)) {
        return;
    }
    if (app_gemini_live_send_text(s.gl, s.notes) == ESP_OK) {
        ESP_LOGI(TAG, "notes delivered");
        s.notes[0] = '\0';
        s.last_activity_us = now;
        begin_reply(); /* the master answers */
    } else {
        s.notes_retry_us = now + NOTES_RETRY_US;
    }
}

static void close_conversation(void)
{
    set_mic(false);
    drop_notes("conversation closed");
    s.voice_reconnect = false;
    set_role_mode(false);
    (void)app_gemini_live_disconnect(s.gl);
    use_voice(s.voice); /* a voice chosen during the conversation applies now */
    app_audio_flush_playback();
    __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    clear_grounding();
    enter(ST_CLOSED);
}

static void open_conversation(void)
{
    if (s.wifi_state == APP_WIFI_UNCONFIGURED) {
        fail(ERR_WIFI_UNCONFIGURED);
        return;
    }
    if (s.wifi_state != APP_WIFI_CONNECTED) {
        fail(ERR_NO_WIFI);
        return;
    }
    char key[APP_SECRETS_API_KEY_CAPACITY];
    esp_err_t err = app_secrets_load_api_key(key, sizeof(key));
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(key, sizeof(key));
        ESP_LOGW(TAG, "API key unavailable (%s); provision it with tools/provision_key.py",
                 esp_err_to_name(err));
        fail(ERR_NO_KEY);
        return;
    }
    __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    enter(ST_CONNECTING);
    err = app_gemini_live_connect(s.gl, key);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed (%s)", esp_err_to_name(err));
        fail(ERR_LINK);
    }
}

static void on_tap(void)
{
    ESP_LOGI(TAG, "tap in state %d", (int)s.state);
    s.last_activity_us = esp_timer_get_time();
    switch (s.state) {
    case ST_CLOSED:
        open_conversation();
        break;
    case ST_SPEAKING:
        /* Interrupt VOX without closing: drop queued speech and listen again. */
        __atomic_store_n(&s.squelch, 1U, __ATOMIC_RELEASE);
        s.squelch_since = esp_timer_get_time();
        app_audio_flush_playback();
        s.reply_pending = false; /* the user dropped this answer */
        enter(ST_LISTENING);
        break;
    case ST_ERROR:
        /* Acknowledge the error; disconnect is a no-op when no link exists. */
        (void)app_gemini_live_disconnect(s.gl);
        enter(ST_CLOSED);
        break;
    case ST_CONNECTING:
    case ST_LISTENING:
    case ST_PROCESSING:
    default:
        if (s.role_mode) {
            /* A game must not end by accident: holding the eye (or asking the
             * master to end the game) leaves it. */
            show_notice("MANTÉN PARA SALIR");
            break;
        }
        close_conversation();
        break;
    }
}

static void on_gl(int type, int code)
{
    const bool open = s.state != ST_CLOSED && s.state != ST_ERROR;
    switch ((app_gemini_live_event_type_t)type) {
    case APP_GL_EVT_READY:
        if (open) {
            app_gemini_live_stats_t stats;
            app_gemini_live_get_stats(s.gl, &stats);
            s.voiced_base = __atomic_load_n(&s.voiced_frames, __ATOMIC_ACQUIRE);
            s.transcripts_base = stats.rx_input_transcripts;
            s.audio_base = stats.rx_audio_chunks;
            __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
            s.reply_pending = false;
            flush_preroll(); /* the question spoken right after "Hola ESP" goes first */
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_INPUT_TRANSCRIPT:
        s.last_activity_us = esp_timer_get_time();
        if (open) {
            begin_reply();
        }
        if (s.state == ST_LISTENING) {
            enter(ST_PROCESSING);
        }
        break;
    case APP_GL_EVT_MODEL_ACTIVITY:
    case APP_GL_EVT_GROUNDING:
        /* Searching, thinking or text parts: the model is working on the turn. */
        if ((app_gemini_live_event_type_t)type == APP_GL_EVT_MODEL_ACTIVITY) {
            __atomic_store_n(&s.activity_queued, 0U, __ATOMIC_RELEASE);
        }
        s.last_activity_us = esp_timer_get_time();
        if (!open || __atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE)) {
            break; /* a turn the user interrupted: ignore its leftovers */
        }
        begin_reply();
        if ((app_gemini_live_event_type_t)type == APP_GL_EVT_GROUNDING) {
            load_grounding();
        }
        if (s.state == ST_LISTENING) {
            enter(ST_PROCESSING);
        }
        break;
    case APP_GL_EVT_TURN_COMPLETE:
        s.last_activity_us = esp_timer_get_time();
        if (code) {
            /* interactionStatus IN_PROGRESS: a spoken filler ended, the model
             * keeps searching or thinking. Wait for the real end of the turn
             * (an interrupted turn stays squelched until then). */
            if (open && !__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE)) {
                begin_reply();
            }
            break;
        }
        s.turn_done = true;
        s.reply_pending = false;
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
        if (s.state == ST_PROCESSING) {
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_INTERRUPTED:
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
        s.reply_pending = false;
        if (s.state == ST_SPEAKING || s.state == ST_PROCESSING) {
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_DISCONNECTED:
        if (open) {
            app_audio_flush_playback();
            enter(ST_CONNECTING); /* client reconnects and resumes automatically */
        }
        break;
    case APP_GL_EVT_FAILED:
        ESP_LOGE(TAG, "Gemini Live unavailable (code %d)", code);
        (void)app_gemini_live_disconnect(s.gl);
        app_audio_flush_playback();
        fail(code == 401 || code == 403 ? ERR_KEY_REJECTED : ERR_LINK);
        break;
    default:
        break;
    }
}

/* One line every DIAG_PERIOD_US while a conversation is open: enough to tell a
 * silent microphone, failing sends and a silent server apart. */
static void log_diagnostics(int64_t now)
{
    const bool idle = s.state == ST_CLOSED || s.state == ST_ERROR;
    if (now - s.last_diag_us < (idle ? 2 * DIAG_PERIOD_US : DIAG_PERIOD_US)) {
        return;
    }
    s.last_diag_us = now;
    /* The panel needs contiguous internal DMA bounce buffers on every transfer:
     * report the largest such block next to the free total. */
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const size_t int_free = heap_caps_get_free_size(dma_caps);
    const size_t int_largest = heap_caps_get_largest_free_block(dma_caps);
    const uint32_t fails = s_alloc_fail_count;
    static uint32_t fails_logged;
    if (fails != fails_logged) {
        fails_logged = fails;
        ESP_LOGW(TAG, "allocation failures: %u (last %u B, caps 0x%x)", (unsigned)fails,
                 (unsigned)s_alloc_fail_size, (unsigned)s_alloc_fail_caps);
    }
    if (idle) {
        ESP_LOGI(TAG, "diag st=%d wake=%d heap{int_dma_free=%u largest=%u}", (int)s.state,
                 (int)__atomic_load_n(&s.wake_feed, __ATOMIC_ACQUIRE), (unsigned)int_free,
                 (unsigned)int_largest);
        return;
    }
    app_audio_mic_stats_t mic;
    app_audio_get_mic_stats(&mic);
    app_gemini_live_stats_t gl;
    app_gemini_live_get_stats(s.gl, &gl);
    const float peak_dbfs = mic.peak ? 20.0f * log10f((float)mic.peak / 32768.0f) : -96.0f;
    ESP_LOGI(TAG, "diag st=%d mic{frames=%u peak=%.0f dBFS slot=%u on=%d} "
             "tx{ok=%u fail=%u notready=%u} rx{msgs=%u audio=%u transcripts=%u other=%u} "
             "search{q=%u grounded=%u} tools{calls=%u stills=%u} pending=%d "
             "heap{int_dma_free=%u largest=%u}",
             (int)s.state, (unsigned)mic.frames, (double)peak_dbfs, (unsigned)mic.slot,
             (int)mic.enabled, (unsigned)gl.tx_messages, (unsigned)gl.tx_failures,
             (unsigned)gl.tx_not_ready, (unsigned)gl.rx_messages, (unsigned)gl.rx_audio_chunks,
             (unsigned)gl.rx_input_transcripts, (unsigned)gl.rx_other_parts,
             (unsigned)gl.search_queries, (unsigned)gl.rx_grounding, (unsigned)gl.tool_calls,
             (unsigned)s.stills_sent, (int)s.reply_pending,
             (unsigned)int_free, (unsigned)int_largest);
}

/* A model that completes setup but never transcribes (gemini-3.8-live in the
 * 2026-11-24 tests) defeats the setup-failure fallback. If the user has clearly
 * spoken on this connection and no transcription arrived, reconnect once with the
 * other configured model. Returns true when the conversation was reopened. */
static bool check_silent_model(void)
{
    if (s.model_swapped || (s.state != ST_LISTENING && s.state != ST_PROCESSING)) {
        return false;
    }
    const uint32_t voiced = __atomic_load_n(&s.voiced_frames, __ATOMIC_ACQUIRE) - s.voiced_base;
    if (voiced < (uint32_t)CONFIG_VOX_SILENT_MODEL_VOICED_FRAMES) {
        return false;
    }
    app_gemini_live_stats_t stats;
    app_gemini_live_get_stats(s.gl, &stats);
    if (stats.rx_input_transcripts != s.transcripts_base || stats.rx_audio_chunks != s.audio_base) {
        return false; /* the model is listening or answering */
    }
    s.model_swapped = true;
    ESP_LOGW(TAG, "model %s silent after %u voiced frames; switching model",
             app_gemini_live_active_model(s.gl), (unsigned)voiced);
    set_mic(false);
    (void)app_gemini_live_disconnect(s.gl);
    app_audio_flush_playback();
    if (app_gemini_live_swap_model(s.gl) != ESP_OK) {
        fail(ERR_LINK);
        return true;
    }
    s.state = ST_CLOSED;
    open_conversation();
    return true;
}

#if CONFIG_VOX_CAMERA_BOOT_TEST
/* Diagnostic still while idle, once per boot: tells conversation load apart
 * from a camera/driver problem. Logged only, never sent. */
static void camera_boot_test(int64_t now)
{
    static bool done;
    if (done || now < 8LL * 1000 * 1000 || s.state != ST_CLOSED) {
        return;
    }
    done = true;
    app_camera_still_t still;
    ESP_LOGI(TAG, "camera boot test: taking one still while idle");
    const esp_err_t err = app_camera_capture_jpeg(VOX_CAMERA_JPEG_MAX_BYTES, &still);
    ESP_LOGI(TAG, "camera boot test: %s", err == ESP_OK ? "OK" : esp_err_to_name(err));
}
#endif

static void on_tick(void)
{
    const int64_t now = esp_timer_get_time();
#if CONFIG_VOX_CAMERA_BOOT_TEST
    camera_boot_test(now);
#endif
    log_diagnostics(now);
    if (check_silent_model()) {
        return;
    }
    /* Local voice counts as activity at once: Gemini's transcription arrives later,
     * and a short timeout must never close the conversation mid-sentence. */
    const uint32_t voiced = __atomic_load_n(&s.voiced_frames, __ATOMIC_ACQUIRE);
    if (voiced != s.voiced_seen) {
        s.voiced_seen = voiced;
        s.last_activity_us = now;
    }
    /* Any model output (audio included) proves the turn is alive. */
    const uint32_t model_rx = __atomic_load_n(&s.model_rx_ms, __ATOMIC_ACQUIRE);
    if (model_rx != s.model_rx_seen) {
        s.model_rx_seen = model_rx;
        s.last_activity_us = now;
        s.model_progress_us = now;
    }
    rotate_grounding(now);
    if (s.gallery_open && now - s.gallery_touch_us > GALLERY_IDLE_US) {
        gallery_close();
    }
    if (s.notice_until_us && now > s.notice_until_us) {
        s.notice_until_us = 0;
        refresh_status(); /* back to the error/sources line or hidden */
    }
    if (s.reply_pending && now - s.model_progress_us > RESPONSE_TIMEOUT_US) {
        ESP_LOGW(TAG, "no model output for %d s; reply abandoned", CONFIG_VOX_RESPONSE_TIMEOUT_S);
        s.reply_pending = false;
        if (s.state == ST_PROCESSING) {
            enter(ST_LISTENING); /* the idle countdown starts now */
        }
    }
#if CONFIG_VOX_ROLE_PHONE_PAGE
    if (s.role_mode && s.state != ST_CLOSED && s.state != ST_ERROR) {
        char notice[384];
        if (app_rol_web_take_notices(notice, sizeof(notice)) > 0) {
            char note[sizeof(notice) + sizeof(NOTICE_PREFIX)];
            snprintf(note, sizeof(note), NOTICE_PREFIX "%s", notice);
            ESP_LOGI(TAG, "phone notice: %s", notice);
            queue_note(note, now);
        }
    }
#endif
    flush_notes(now);
    if (s.voice_reconnect && s.state == ST_LISTENING && !s.reply_pending &&
            !app_audio_is_playing()) {
        /* cambiar_voz: the goodbye has been spoken; reopen with the new voice. */
        ESP_LOGI(TAG, "reconnecting with voice %s", s.voice);
        s.voice_reconnect = false;
        set_mic(false);
        drop_notes("voice change");
        (void)app_gemini_live_disconnect(s.gl);
        use_voice(s.voice);
        open_conversation();
        return;
    }
    const int64_t idle_limit = s.role_mode ? ROLE_IDLE_TIMEOUT_US : IDLE_TIMEOUT_US;
    if (s.state == ST_LISTENING && !s.reply_pending && !app_audio_is_playing() &&
            now - s.last_activity_us > idle_limit) {
        ESP_LOGI(TAG, "%d s without activity; back to idle",
                 (int)(idle_limit / (1000 * 1000)));
        close_conversation();
        return;
    }
    if (__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE) && now - s.squelch_since > SQUELCH_MAX_US) {
        /* The interrupted turn never reported completion: stop discarding audio. */
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    }
    if (s.state == ST_CONNECTING && now - s.connecting_since > CONNECTING_TIMEOUT_US) {
        ESP_LOGW(TAG, "connection not ready after %d s; giving up",
                 (int)(CONNECTING_TIMEOUT_US / 1000000));
        (void)app_gemini_live_disconnect(s.gl);
        app_audio_flush_playback();
        fail(ERR_LINK);
        return;
    }
    if (s.state == ST_SPEAKING && !app_audio_is_playing()) {
        if (s.turn_done || !s.reply_pending) {
            enter(ST_LISTENING);
        } else if (now - s.model_progress_us > SPEAKING_GAP_US) {
            /* Speech drained but the turn is still open (e.g. "let me check" and
             * then a search): show the model working, keep the idle return off. */
            enter(ST_PROCESSING);
        }
    } else if (s.state == ST_PROCESSING && !s.reply_pending &&
               now - s.processing_since > RESPONSE_TIMEOUT_US) {
        ESP_LOGW(TAG, "no response; listening again");
        enter(ST_LISTENING);
    }
}

static void answer_tool(const tool_slot_t *slot, const char *response_json)
{
    if (__atomic_load_n(&slot->cancelled, __ATOMIC_ACQUIRE)) {
        ESP_LOGI(TAG, "tool %s cancelled; no response", slot->name);
        return;
    }
    (void)app_gemini_live_send_tool_response(s.gl, slot->id, slot->name, response_json);
}

/* capturar_imagen: one still, sent as realtimeInput video, then the response.
 * Runs on the session task (about 1-2 s); the uplink pauses meanwhile so the
 * large message does not compete with microphone frames for the socket. */
static void run_capture(const tool_slot_t *slot)
{
    if (s.config.ui.acknowledge) {
        s.config.ui.acknowledge(s.config.ui.ui);
    }
    if (s.config.ui.set_status) {
        s.config.ui.set_status(s.config.ui.ui, "OBSERVANDO");
    }
    set_mic(false);
    app_camera_still_t still;
    const esp_err_t err = app_camera_capture_jpeg(VOX_CAMERA_JPEG_MAX_BYTES, &still);
    if (err == ESP_OK && !__atomic_load_n(&slot->cancelled, __ATOMIC_ACQUIRE) &&
            app_gemini_live_send_image(s.gl, still.data, still.size) == ESP_OK) {
        s.stills_sent++;
        answer_tool(slot, "{\"result\":\"Imagen enviada: es el último fotograma de vídeo "
                    "recibido. Descríbela al operador.\"}");
        /* Keep it for the gallery after Gemini has it (the erase takes ~0.5 s). */
        if (app_photos_save(still.data, still.size) == ESP_OK) {
            update_photo_count();
        }
    } else if (err == ESP_ERR_NOT_FOUND) {
        answer_tool(slot, "{\"error\":\"No hay ninguna cámara conectada en la ranura izquierda.\"}");
    } else {
        ESP_LOGW(TAG, "capture failed (%s)", esp_err_to_name(err));
        answer_tool(slot, "{\"error\":\"La cámara no ha podido tomar la imagen.\"}");
    }
    const int64_t now = esp_timer_get_time();
    s.last_activity_us = now;
    s.model_progress_us = now;  /* the model now answers about the image */
    set_mic(s.state == ST_LISTENING || s.state == ST_PROCESSING);
    refresh_status();
}

static const char *arg_str(const cJSON *args, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void answer_json(const tool_slot_t *slot, cJSON *root, const char *fallback);
static void saved_title(char *out, size_t cap);

static void run_dice(const tool_slot_t *slot, const cJSON *args)
{
    const vox_rol_spec_t spec = spec_from_args(args, 1, 0);
    uint16_t values[VOX_ROL_MAX_DICE];
    roll(&spec, values);
    char json[320];
    if (vox_rol_roll_json(&spec, values, json, sizeof(json)) == 0) {
        snprintf(json, sizeof(json), "{\"error\":\"No se pudo formatear la tirada.\"}");
    }
    ESP_LOGI(TAG, "dice %s", json);
    answer_tool(slot, json);
}

static void run_prepare(const tool_slot_t *slot, const cJSON *args)
{
    if (!s.role_mode) {
        answer_tool(slot, "{\"error\":\"El botón de dados solo está en modo rol: llama antes a "
                    "modo_rol.\"}");
        return;
    }
    set_player_roll(spec_from_args(args, 2, 0));
    char what[96];
    char badge[8];
    (void)vox_rol_spec_text(&s.player_roll, what, sizeof(what));
    (void)vox_rol_badge(&s.player_roll, badge, sizeof(badge));
    ESP_LOGI(TAG, "player roll prepared: %s", what);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tirada_preparada", what);
    cJSON_AddStringToObject(root, "boton", badge);
    cJSON_AddStringToObject(root, "result",
                            "Dile al jugador que toque el botón de dados de la derecha de la "
                            "pantalla. Recibirás su resultado como un mensaje de texto.");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    answer_tool(slot, json ? json : "{\"result\":\"Tirada preparada.\"}");
    cJSON_free(json);
}

static void run_voice(const tool_slot_t *slot, const cJSON *args)
{
    const char *name = arg_str(args, "voz");
    if (!name || vox_rol_voice_index(name) < 0) {
        answer_tool(slot, "{\"error\":\"Voz no disponible: usa Charon, Algenib, Kore o Sulafat.\"}");
        return;
    }
    (void)app_secrets_store_voice(name);
    snprintf(s.voice, sizeof(s.voice), "%s", name);
    if (s.config.ui.set_voice_initial) {
        s.config.ui.set_voice_initial(s.config.ui.ui, s.voice[0]);
    }
    s.voice_reconnect = true;
    ESP_LOGI(TAG, "voice %s requested by the model", name);
    answer_tool(slot, "{\"result\":\"Voz cambiada. Despídete con una frase muy corta: al "
                "terminar de hablar VOX se reconecta con la voz nueva y el operador vuelve a "
                "hablarte.\"}");
}

static void run_role(const tool_slot_t *slot, const cJSON *args)
{
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(args, "activar");
    set_role_mode(cJSON_IsTrue(on));
    if (!s.role_mode) {
        answer_tool(slot, "{\"result\":\"Modo rol desactivado.\"}");
        return;
    }
    char address[40];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "result",
                            "Modo rol activado: la conversación espera más antes de cerrarse y "
                            "los jugadores tienen un botón de dados en la pantalla, que tira 2d20 "
                            "sumados hasta que prepares otra tirada con preparar_tirada. Un "
                            "toque solo te interrumpe; para salir de la partida se mantiene "
                            "pulsada la pantalla o te lo piden.");
    if (app_rol_web_running() && app_rol_web_address(address, sizeof(address)) == ESP_OK) {
        cJSON_AddStringToObject(root, "pagina_movil", address);
        cJSON_AddStringToObject(root, "instrucciones",
                                "Diles que vas a facilitarles una dirección: que la abran en el "
                                "móvil, conectados a la misma Wi-Fi (sale en la parte de abajo de "
                                "la pantalla), y escriban su nombre; allí verán su ficha y los "
                                "secretos que solo deben saber ellos. Espera a que entren: VOX "
                                "te avisará de cada uno. Empieza cuando te digan que están "
                                "listos; quien llegue tarde o cambie de móvil puede entrar "
                                "después con el mismo nombre.");
    }
    char title[ROL_GAME_TEXT_CAP];
    saved_title(title, sizeof(title));
    if (title[0]) {
        cJSON_AddStringToObject(root, "partida_guardada", title);
        cJSON_AddStringToObject(root, "pregunta",
                                "Hay una partida guardada: pregunta si quieren una partida "
                                "nueva, continuarla (cargar_partida modo continuar) o una "
                                "aventura nueva con los mismos personajes (modo "
                                "mismos_personajes).");
    }
    answer_json(slot, root, "{\"result\":\"Modo rol activado.\"}");
}

/* Answer with a cJSON object and free it. */
static void answer_json(const tool_slot_t *slot, cJSON *root, const char *fallback)
{
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    answer_tool(slot, json ? json : fallback);
    cJSON_free(json);
}

static void run_secret(const tool_slot_t *slot, const cJSON *args)
{
    const char *player = arg_str(args, "jugador");
    const char *text = arg_str(args, "texto");
    const bool on_screen = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "en_pantalla"));
    if (!text || !text[0]) {
        answer_tool(slot, "{\"error\":\"Falta el texto del secreto.\"}");
        return;
    }
    const app_rol_web_put_t put = (player && !on_screen) ? app_rol_web_deliver(player, text)
                                                         : APP_ROL_WEB_FAILED;
    if (put == APP_ROL_WEB_PHONE) {
        ESP_LOGI(TAG, "secret sent to the phone of %s", player);
        answer_tool(slot, "{\"result\":\"Secreto enviado al móvil del jugador; solo él lo "
                    "ve. No lo digas en voz alta.\"}");
        return;
    }
    if (put == APP_ROL_WEB_PENDING) {
        ESP_LOGI(TAG, "secret kept for %s (no phone yet)", player);
        answer_tool(slot, "{\"result\":\"Ese jugador aún no ha entrado en la página del móvil: "
                    "el secreto le espera y lo verá en cuanto entre con su nombre. Si no tiene "
                    "móvil, vuelve a llamar con en_pantalla verdadero. No lo digas en voz "
                    "alta.\"}");
        return;
    }
    if (!s.config.ui.show_secret) {
        answer_tool(slot, "{\"error\":\"No se puede mostrar el secreto.\"}");
        return;
    }
    s.config.ui.show_secret(s.config.ui.ui, player ? player : "", text);
    ESP_LOGI(TAG, "secret shown on screen for %s", player ? player : "?");
    char joined[160];
    app_rol_web_players(joined, sizeof(joined));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "result",
                            "El secreto se muestra en la pantalla de VOX hasta que la toquen. "
                            "Pide a los demás que no miren y no lo digas en voz alta.");
    cJSON_AddStringToObject(root, "jugadores_con_movil", joined);
    answer_json(slot, root, "{\"result\":\"Secreto mostrado en la pantalla.\"}");
}

/* Saved game title ("" if none). */
static void saved_title(char *out, size_t cap)
{
    out[0] = '\0';
    char *json = app_rol_web_load_game();
    rol_game_meta_t meta;
    if (json && rol_game_restore(json, &meta, NULL, false)) {
        snprintf(out, cap, "%s", meta.title[0] ? meta.title : "sin título");
    }
    free(json);
}

static void run_status(const tool_slot_t *slot)
{
    cJSON *root = cJSON_CreateObject();
    char address[40];
    char *players = app_rol_web_status_json();
    if (!players) {
        cJSON_AddStringToObject(root, "result", "El modo rol no está activo: no hay página "
                                "del móvil.");
    } else {
        if (app_rol_web_address(address, sizeof(address)) == ESP_OK) {
            cJSON_AddStringToObject(root, "pagina_movil", address);
        }
        cJSON_AddItemToObject(root, "jugadores", cJSON_Parse(players));
        cJSON_free(players);
    }
    char title[ROL_GAME_TEXT_CAP];
    saved_title(title, sizeof(title));
    if (title[0]) {
        cJSON_AddStringToObject(root, "partida_guardada", title);
    }
    answer_json(slot, root, "{\"error\":\"Sin memoria.\"}");
}

static void run_sheet(const tool_slot_t *slot, const cJSON *args)
{
    const char *player = arg_str(args, "jugador");
    const char *sheet = arg_str(args, "ficha");
    if (!player || !sheet || !sheet[0]) {
        answer_tool(slot, "{\"error\":\"Faltan el jugador o la ficha.\"}");
        return;
    }
    switch (app_rol_web_set_sheet(player, sheet)) {
    case APP_ROL_WEB_PHONE:
        ESP_LOGI(TAG, "sheet updated on the phone of %s", player);
        answer_tool(slot, "{\"result\":\"Ficha actualizada en su móvil.\"}");
        break;
    case APP_ROL_WEB_PENDING:
        ESP_LOGI(TAG, "sheet kept for %s (no phone yet)", player);
        answer_tool(slot, "{\"result\":\"Ficha guardada: la verá en cuanto entre en la página "
                    "del móvil con su nombre.\"}");
        break;
    default:
        answer_tool(slot, "{\"error\":\"No se puede guardar la ficha: activa antes el modo rol "
                    "o ya hay seis jugadores.\"}");
        break;
    }
}

static const char *mode_name(const vox_rol_spec_t *spec)
{
    return spec->mode == VOX_ROL_PERCENT ? "porcentaje"
           : (spec->mode == VOX_ROL_POOL ? "reserva" : "suma");
}

static void run_save(const tool_slot_t *slot, const cJSON *args)
{
    const char *title = arg_str(args, "titulo");
    const char *game = arg_str(args, "juego");
    const char *summary = arg_str(args, "resumen");
    if (!summary || !summary[0]) {
        answer_tool(slot, "{\"error\":\"Falta el resumen de la partida.\"}");
        return;
    }
    rol_game_meta_t meta = {
        .dice_count = s.player_roll.count,
        .dice_sides = s.player_roll.sides,
        .dice_target = s.player_roll.target,
    };
    snprintf(meta.title, sizeof(meta.title), "%s", title && title[0] ? title : "Partida de VOX");
    snprintf(meta.game, sizeof(meta.game), "%s", game ? game : "");
    snprintf(meta.dice_mode, sizeof(meta.dice_mode), "%s", mode_name(&s.player_roll));
    char *json = app_rol_web_build_game(&meta, summary);
    const esp_err_t err = json ? app_rol_web_store_game(json) : ESP_ERR_INVALID_SIZE;
    const size_t bytes = json ? strlen(json) : 0;
    cJSON_free(json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "game not saved (%s)", esp_err_to_name(err));
        answer_tool(slot, "{\"error\":\"No se ha podido guardar la partida: acorta el resumen "
                    "y vuelve a intentarlo.\"}");
        return;
    }
    ESP_LOGI(TAG, "game saved: %s (%u bytes)", meta.title, (unsigned)bytes);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "result", "Partida guardada en VOX. Para continuarla otro "
                            "día pedidme continuar la partida.");
    char address[40];
    if (app_rol_web_running() && app_rol_web_address(address, sizeof(address)) == ESP_OK) {
        char url[56];
        snprintf(url, sizeof(url), "%s/partida", address);
        cJSON_AddStringToObject(root, "descarga", url);
        cJSON_AddStringToObject(root, "como_descargar", "En la página del móvil, apartado "
                                "Partida guardada, botón Descargar.");
    }
    answer_json(slot, root, "{\"result\":\"Partida guardada.\"}");
}

static void run_load(const tool_slot_t *slot, const cJSON *args)
{
    const char *mode = arg_str(args, "modo");
    const bool keep_story = !mode || strcmp(mode, "mismos_personajes") != 0;
    char *json = app_rol_web_load_game();
    cJSON *game = json ? cJSON_Parse(json) : NULL;
    if (!game) {
        free(json);
        answer_tool(slot, "{\"error\":\"No hay ninguna partida guardada en VOX. Se puede subir "
                    "un archivo desde la página del móvil.\"}");
        return;
    }
    set_role_mode(true);
    rol_game_meta_t meta;
    const bool on_phones = app_rol_web_restore_game(json, &meta, keep_story);
    if (!on_phones) {
        (void)rol_game_restore(json, &meta, NULL, false);
    }
    free(json);
    set_player_roll(vox_rol_spec_make(vox_rol_mode_parse(meta.dice_mode), meta.dice_count,
                                      meta.dice_sides, meta.dice_target));
    if (!keep_story) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(game, "jugadores")) {
            cJSON_DeleteItemFromObjectCaseSensitive(item, "secreto");
        }
    }
    ESP_LOGI(TAG, "game loaded (%s): %s", keep_story ? "continue" : "same characters", meta.title);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "result", keep_story
        ? "Partida cargada. Haz un breve resumen de lo ocurrido sin revelar secretos y continúa "
          "donde se quedó. Los jugadores entran en la página del móvil con el mismo nombre y "
          "recuperan su ficha y su secreto."
        : "Personajes cargados para una aventura nueva: lo vivido es su pasado. Prepara un guion "
          "nuevo y nuevos secretos. Los jugadores entran en la página del móvil con el mismo "
          "nombre y recuperan su ficha.");
    char address[40];
    if (app_rol_web_running() && app_rol_web_address(address, sizeof(address)) == ESP_OK) {
        cJSON_AddStringToObject(root, "pagina_movil", address);
    }
    cJSON_AddItemToObject(root, "partida", game);
    answer_json(slot, root, "{\"error\":\"Sin memoria para cargar la partida.\"}");
}

static void run_tool(int index)
{
    if (index < 0 || index >= TOOL_SLOTS) {
        return;
    }
    const tool_slot_t *slot = &s.tools[index];
    const bool open = s.state != ST_CLOSED && s.state != ST_ERROR;
    cJSON *args = open ? cJSON_Parse(slot->args) : NULL;
    if (open && strcmp(slot->name, TOOL_CAPTURE) == 0 && VOX_CAMERA) {
        run_capture(slot);
    } else if (open && strcmp(slot->name, TOOL_DICE) == 0) {
        run_dice(slot, args);
    } else if (open && strcmp(slot->name, TOOL_VOICE) == 0) {
        run_voice(slot, args);
    } else if (open && strcmp(slot->name, TOOL_ROLE) == 0) {
        run_role(slot, args);
    } else if (open && strcmp(slot->name, TOOL_SECRET) == 0) {
        run_secret(slot, args);
    } else if (open && strcmp(slot->name, TOOL_PREPARE) == 0) {
        run_prepare(slot, args);
    } else if (open && strcmp(slot->name, TOOL_STATUS) == 0) {
        run_status(slot);
    } else if (open && strcmp(slot->name, TOOL_SHEET) == 0) {
        run_sheet(slot, args);
    } else if (open && strcmp(slot->name, TOOL_SAVE) == 0) {
        run_save(slot, args);
    } else if (open && strcmp(slot->name, TOOL_LOAD) == 0) {
        run_load(slot, args);
    } else if (open) {
        answer_tool(slot, "{\"error\":\"Función desconocida.\"}");
    }
    cJSON_Delete(args);
    __atomic_store_n(&s.tool_busy[index], 0U, __ATOMIC_RELEASE);
}

/* WebSocket task: copy a function call into a free slot and hand it over. */
static void queue_tool(const app_gemini_live_event_t *event)
{
    for (int i = 0; i < TOOL_SLOTS; ++i) {
        if (__atomic_exchange_n(&s.tool_busy[i], 1U, __ATOMIC_ACQ_REL) == 0U) {
            tool_slot_t *slot = &s.tools[i];
            snprintf(slot->id, sizeof(slot->id), "%s", event->tool_id ? event->tool_id : "");
            snprintf(slot->name, sizeof(slot->name), "%s", event->tool_name ? event->tool_name : "");
            snprintf(slot->args, sizeof(slot->args), "%s", event->text ? event->text : "{}");
            if (event->text && strlen(event->text) >= sizeof(slot->args)) {
                ESP_LOGW(TAG, "tool call %s: arguments longer than %u bytes",
                         slot->name, (unsigned)sizeof(slot->args));
            }
            __atomic_store_n(&slot->cancelled, 0U, __ATOMIC_RELEASE);
            post(EV_TOOL, i, 0);
            return;
        }
    }
    ESP_LOGW(TAG, "tool call %s dropped: %d calls already pending", event->tool_name, TOOL_SLOTS);
}

static void cancel_tool(const char *id)
{
    for (int i = 0; i < TOOL_SLOTS; ++i) {
        if (__atomic_load_n(&s.tool_busy[i], __ATOMIC_ACQUIRE) && id &&
                strcmp(s.tools[i].id, id) == 0) {
            __atomic_store_n(&s.tools[i].cancelled, 1U, __ATOMIC_RELEASE);
        }
    }
}

static void update_photo_count(void)
{
    if (s.config.ui.set_photo_count) {
        s.config.ui.set_photo_count(s.config.ui.ui, (unsigned)app_photos_count());
    }
}

static void gallery_close(void)
{
    if (s.gallery_open && s.config.ui.hide_gallery) {
        s.config.ui.hide_gallery(s.config.ui.ui);
    }
    s.gallery_open = false;
}

/* Read photo gallery_pos from flash and hand it to the UI (GSP copies it). */
static void gallery_show(void)
{
    const size_t count = app_photos_count();
    if (count == 0 || !s.photo_buf || !s.config.ui.show_photo) {
        gallery_close();
        return;
    }
    if (s.gallery_pos >= count) {
        s.gallery_pos = count - 1;
    }
    size_t size = 0;
    const esp_err_t err = app_photos_read(s.gallery_pos, s.photo_buf, app_photos_max_size(), &size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "photo %u unreadable (%s)", (unsigned)(s.gallery_pos + 1), esp_err_to_name(err));
    }
    s.config.ui.show_photo(s.config.ui.ui, err == ESP_OK ? s.photo_buf : NULL,
                           err == ESP_OK ? size : 0, (unsigned)(s.gallery_pos + 1), (unsigned)count);
    s.gallery_open = true;
    s.gallery_touch_us = esp_timer_get_time();
}

static void on_gallery(app_vox_session_gallery_t action)
{
    const size_t count = app_photos_count();
    switch (action) {
    case APP_VOX_SESSION_GALLERY_OPEN:
        /* Not while VOX is answering: the conversation keeps the screen. */
        if (s.gallery_open || count == 0 || s.state == ST_SPEAKING || s.state == ST_PROCESSING ||
                s.state == ST_CONNECTING) {
            return;
        }
        s.gallery_pos = count - 1; /* newest first */
        gallery_show();
        break;
    case APP_VOX_SESSION_GALLERY_PREV:
    case APP_VOX_SESSION_GALLERY_NEXT:
        if (!s.gallery_open || count == 0) {
            return;
        }
        if (action == APP_VOX_SESSION_GALLERY_PREV) {
            s.gallery_pos = s.gallery_pos == 0 ? count - 1 : s.gallery_pos - 1;
        } else {
            s.gallery_pos = s.gallery_pos + 1 >= count ? 0 : s.gallery_pos + 1;
        }
        gallery_show();
        break;
    case APP_VOX_SESSION_GALLERY_CLOSE:
        gallery_close();
        break;
    case APP_VOX_SESSION_GALLERY_DELETE_ALL:
        if (!s.gallery_open) {
            return;
        }
        gallery_close();
        if (s.config.ui.set_status) {
            s.config.ui.set_status(s.config.ui.ui, "BORRANDO...");
        }
        if (app_photos_clear() == ESP_OK) {
            ESP_LOGI(TAG, "all photos deleted by the owner");
        }
        update_photo_count();
        if (s.config.ui.set_status) {
            s.config.ui.set_status(s.config.ui.ui, "FOTOS BORRADAS");
        }
        s.notice_until_us = esp_timer_get_time() + NOTICE_US;
        break;
    case APP_VOX_SESSION_GALLERY_TOUCH:
    default:
        s.gallery_touch_us = esp_timer_get_time();
        break;
    }
}

static void on_action(app_vox_session_action_t action)
{
    if (action == APP_VOX_SESSION_ACTION_VOICE_NEXT) {
        const vox_rol_voice_t *next = vox_rol_voice_next(s.voice);
        (void)app_secrets_store_voice(next->name);
        const bool open = s.state != ST_CLOSED && s.state != ST_ERROR;
        use_voice(next->name); /* the open conversation keeps its voice until it closes */
        /* Closed: "KORE · CHICA · FIRME". Open: it applies when this conversation ends. */
        char text[STATUS_CAPACITY];
        if (open) {
            snprintf(text, sizeof(text), "PRÓXIMA VOZ: %s", next->name);
        } else {
            snprintf(text, sizeof(text), "%s · %s", next->name, next->label);
        }
        for (char *c = text; *c; ++c) {   /* ASCII names to the status charset */
            if (*c >= 'a' && *c <= 'z') {
                *c = (char)(*c - 32);
            }
        }
        show_notice(text);
        ESP_LOGI(TAG, "voice %s (%s)%s", next->name, next->label,
                 open ? " from the next conversation" : "");
        return;
    }
    if (action == APP_VOX_SESSION_ACTION_CLOSE_HOLD) {
        if (s.state != ST_CLOSED && s.state != ST_ERROR) {
            ESP_LOGI(TAG, "eye held in role mode; leaving the game");
            close_conversation();
        }
        return;
    }
    if (action == APP_VOX_SESSION_ACTION_ROLE_ROLL && s.role_mode) {
        uint16_t values[VOX_ROL_MAX_DICE];
        roll(&s.player_roll, values);
        char text[PLAYER_TEXT_CAPACITY];
        if (vox_rol_player_text(&s.player_roll, values, text, sizeof(text)) == 0) {
            return;
        }
        ESP_LOGI(TAG, "player roll: %s", text);
        const int64_t now = esp_timer_get_time();
        s.last_activity_us = now;
        queue_note(text, now); /* several rolls during a reply go as one message */
    }
}

static void session_task(void *arg)
{
    (void)arg;
    session_event_t ev;
    for (;;) {
        if (xQueueReceive(s.queue, &ev, pdMS_TO_TICKS(SESSION_TICK_MS)) == pdTRUE) {
            switch (ev.kind) {
            case EV_TAP:
                on_tap();
                break;
            case EV_WIFI:
                s.wifi_state = ev.value;
                if (s.state == ST_ERROR &&
                        (s.error == ERR_NO_WIFI || s.error == ERR_WIFI_UNCONFIGURED) &&
                        ev.value == APP_WIFI_CONNECTED) {
                    enter(ST_CLOSED);
                }
                break;
            case EV_SECRETS:
                if (s.state == ST_CLOSED || s.state == ST_ERROR) {
                    load_stored_voice(); /* --voice over USB */
                }
                if (s.state == ST_ERROR && (s.error == ERR_NO_KEY || s.error == ERR_KEY_REJECTED) &&
                        app_secrets_has_api_key()) {
                    enter(ST_CLOSED);
                }
                break;
            case EV_GL:
                on_gl(ev.value, ev.code);
                break;
            case EV_AUDIO_START:
                /* No audioStreamEnd here: the server already closed the user's turn,
                 * and the send collided with the incoming audio (ws-client lock). */
                /* Stale start from a turn the user just interrupted: ignore it. */
                if ((s.state == ST_LISTENING || s.state == ST_PROCESSING) &&
                        !__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE)) {
                    begin_reply();
                    enter(ST_SPEAKING);
                }
                break;
            case EV_TOOL:
                run_tool(ev.value);
                break;
            case EV_GALLERY:
                on_gallery((app_vox_session_gallery_t)ev.value);
                break;
            case EV_ACTION:
                on_action((app_vox_session_action_t)ev.value);
                break;
            case EV_WAKE:
                if (s.state == ST_CLOSED) {
                    gallery_close();
                    ESP_LOGI(TAG, "\"Hola ESP\" detected; opening conversation");
                    if (s.config.ui.acknowledge) {
                        s.config.ui.acknowledge(s.config.ui.ui);
                    }
                    set_preroll(true);
                    open_conversation();
                }
                break;
            }
        }
        on_tick();
    }
}

/* WebSocket task: keep it short; audio goes straight to the playback ring. */
static void gl_event(const app_gemini_live_event_t *event, void *ctx)
{
    (void)ctx;
    switch (event->type) {
    case APP_GL_EVT_AUDIO:
        if (__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE) || event->samples == 0) {
            return;
        }
        __atomic_store_n(&s.model_rx_ms, now_ms(), __ATOMIC_RELEASE);
        (void)app_audio_play(event->pcm, event->samples, PLAY_TIMEOUT_MS);
        /* A tap may interrupt while this chunk waited for ring space: drop what it
         * queued and never report it as the start of a new turn. */
        if (__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE)) {
            app_audio_flush_playback();
            return;
        }
        if (!__atomic_exchange_n(&s.speaking, 1U, __ATOMIC_ACQ_REL)) {
            post(EV_AUDIO_START, 0, 0);
        }
        return;
    case APP_GL_EVT_INTERRUPTED:
        app_audio_flush_playback();
        break;
    case APP_GL_EVT_INPUT_TRANSCRIPT:
        ESP_LOGD(TAG, "user: %s", event->text);
        break;
    case APP_GL_EVT_OUTPUT_TRANSCRIPT:
        ESP_LOGD(TAG, "vox: %s", event->text);
        return;
    case APP_GL_EVT_MODEL_ACTIVITY:
        __atomic_store_n(&s.model_rx_ms, now_ms(), __ATOMIC_RELEASE);
        /* Bursts of thought/code parts must not fill the queue and push out a
         * turnComplete: the timestamp above already keeps the turn alive. */
        if (__atomic_exchange_n(&s.activity_queued, 1U, __ATOMIC_ACQ_REL)) {
            return;
        }
        break;
    case APP_GL_EVT_GROUNDING:
        __atomic_store_n(&s.model_rx_ms, now_ms(), __ATOMIC_RELEASE);
        break;
    case APP_GL_EVT_TOOL_CALL:
        __atomic_store_n(&s.model_rx_ms, now_ms(), __ATOMIC_RELEASE);
        queue_tool(event);
        return;
    case APP_GL_EVT_TOOL_CANCEL:
        cancel_tool(event->tool_id);
        return;
    default:
        break;
    }
    post(EV_GL, (int)event->type,
         event->type == APP_GL_EVT_TURN_COMPLETE ? (int)event->in_progress : event->close_code);
}

/* Capture task: forward 40 ms frames while listening. */
/* Wake task: never block. */
static void on_wake(void *ctx)
{
    (void)ctx;
    post(EV_WAKE, 0, 0);
}

static void on_mic(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    if (!__atomic_load_n(&s.mic_forward, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE) && samples == APP_AUDIO_MIC_FRAME) {
            __atomic_store_n(&s.preroll_busy, 1U, __ATOMIC_RELEASE);
            if (__atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE)) {
                const uint32_t n = __atomic_load_n(&s.preroll_count, __ATOMIC_RELAXED);
                memcpy(&s.preroll[(n % PREROLL_FRAMES) * APP_AUDIO_MIC_FRAME], pcm,
                       samples * sizeof(int16_t));
                __atomic_store_n(&s.preroll_count, n + 1U, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&s.preroll_busy, 0U, __ATOMIC_RELEASE);
        } else if (__atomic_load_n(&s.wake_feed, __ATOMIC_ACQUIRE)) {
            app_wake_feed(pcm, samples);
        }
        return;
    }
    if (app_gemini_live_send_audio(s.gl, pcm, samples) == ESP_OK) {
        int peak = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int v = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
            peak = v > peak ? v : peak;
        }
        if (peak >= CONFIG_VOX_VOICED_PEAK_THRESHOLD) {
            __atomic_add_fetch(&s.voiced_frames, 1U, __ATOMIC_RELEASE);
        }
    }
}

void app_vox_session_tap(void)
{
    post(EV_TAP, 0, 0);
}

void app_vox_session_gallery(app_vox_session_gallery_t action)
{
    post(EV_GALLERY, (int)action, 0);
}

void app_vox_session_action(app_vox_session_action_t action)
{
    post(EV_ACTION, (int)action, 0);
}

void app_vox_session_wifi_changed(int app_wifi_state)
{
    post(EV_WIFI, app_wifi_state, 0);
}

void app_vox_session_secrets_changed(void)
{
    post(EV_SECRETS, 0, 0);
}

uint8_t app_vox_session_output_level(void)
{
    return app_audio_output_level();
}

esp_err_t app_vox_session_start(const app_vox_session_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    s.config = *config;
    s.state = ST_CLOSED;
    (void)heap_caps_register_failed_alloc_callback(on_alloc_failed);
    s.queue = xQueueCreate(SESSION_QUEUE_LEN, sizeof(session_event_t));
    if (!s.queue) {
        return ESP_ERR_NO_MEM;
    }
    s.tools = heap_caps_calloc(TOOL_SLOTS, sizeof(tool_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.tools) {
        vQueueDelete(s.queue);
        s.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Read after the queue exists: later changes arrive as EV_WIFI events. */
    s.wifi_state = app_wifi_get_state();

    /* Model: provisioned override first, the other approved model as fallback. */
    char override[APP_SECRETS_MODEL_CAPACITY];
    const char *primary = CONFIG_VOX_GEMINI_MODEL;
    const char *fallback = CONFIG_VOX_GEMINI_FALLBACK_MODEL;
    if (app_secrets_load_model(override, sizeof(override)) == ESP_OK) {
        primary = override;
        if (strcmp(override, CONFIG_VOX_GEMINI_MODEL) != 0) {
            fallback = CONFIG_VOX_GEMINI_MODEL;
        }
    }
    char stored_voice[APP_SECRETS_VOICE_CAPACITY];
    snprintf(s.voice, sizeof(s.voice), "%s",
             app_secrets_load_voice(stored_voice, sizeof(stored_voice)) == ESP_OK
             ? stored_voice : CONFIG_VOX_GEMINI_VOICE);
    const app_gemini_live_config_t gl_config = {
        .model = primary,
        .fallback_model = fallback,
        .voice = s.voice,
        .language = CONFIG_VOX_GEMINI_LANGUAGE,
        .system_instruction = PERSONA,
        .google_search = VOX_GOOGLE_SEARCH,
        .function_declarations = FUNCTION_DECLARATIONS,
        .on_event = gl_event,
    };
    esp_err_t err = app_gemini_live_create(&gl_config, &s.gl);
    if (err != ESP_OK) {
        goto fail_queue;
    }
    /* Photo store (optional: without the partition stills are only sent). */
    if (app_photos_init() == ESP_OK) {
        s.photo_buf = heap_caps_malloc(app_photos_max_size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    /* Optional: without it grounded answers simply show no source line. */
    s.items = heap_caps_calloc(1, GROUNDING_ITEMS_CAPACITY + GROUNDING_RAW_CAPACITY,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const app_audio_config_t audio = {
        .on_mic = on_mic,
        .mic_gain_db = config->mic_gain_db > 0.0f ? config->mic_gain_db : (float)CONFIG_VOX_MIC_GAIN_DB,
        .volume = config->volume > 0 ? config->volume : CONFIG_VOX_SPEAKER_VOLUME,
    };
    err = app_audio_start(&audio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable (%s)", esp_err_to_name(err));
        goto fail_gl;
    }
#if CONFIG_VOX_WAKE_WORD
    /* Optional: without the model partition VOX keeps working with touch only. */
    s.preroll = heap_caps_calloc((size_t)PREROLL_FRAMES * APP_AUDIO_MIC_FRAME, sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.wake_available = app_wake_init(on_wake, NULL) == ESP_OK;
    if (!s.wake_available) {
        ESP_LOGW(TAG, "wake word unavailable; touch only");
    }
#endif
    if (xTaskCreatePinnedToCore(session_task, "vox_session", SESSION_TASK_STACK, NULL,
                                SESSION_TASK_PRIO, NULL, SESSION_TASK_CORE) != pdPASS) {
        /* Audio tasks keep running on silence; nothing else references gl yet. */
        err = ESP_ERR_NO_MEM;
        goto fail_gl;
    }
    s.started = true;
    if (s.config.ui.set_view) {
        s.config.ui.set_view(s.config.ui.ui, APP_VOX_VIEW_IDLE);
    }
    set_wake_listening(true); /* starts CLOSED: arm local listening */
    update_photo_count();
    if (s.config.ui.set_voice_initial) {
        s.config.ui.set_voice_initial(s.config.ui.ui, s.voice[0]);
    }
    ESP_LOGI(TAG, "VOX M-01 ready (model %s, fallback %s, voice %s, search %s); tap%s to talk",
             primary, fallback, s.voice, VOX_GOOGLE_SEARCH ? "on" : "off",
             s.wake_available ? " or say \"Hola ESP\"" : "");
    return ESP_OK;

fail_gl:
    heap_caps_free(s.items);
    s.items = NULL;
    (void)app_gemini_live_destroy(&s.gl);
fail_queue:
    vQueueDelete(s.queue);
    s.queue = NULL;
    heap_caps_free(s.tools);
    s.tools = NULL;
    return err;
}
