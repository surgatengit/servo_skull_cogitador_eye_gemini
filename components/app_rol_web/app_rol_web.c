// SPDX-License-Identifier: Apache-2.0
#include "app_rol_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "rol_players.h"

static const char *TAG = "app_rol_web";

#define JOIN_BODY_MAX     96
#define SERVER_STACK      6144
#define NOTICES_CAP       320
#define GAME_NAMESPACE    "cog_game"
/* Namespace used up to 1.6; its saved game moves to GAME_NAMESPACE once (1.7). */
#define LEGACY_GAME_NAMESPACE "vox_game"
#define GAME_KEY          "partida"

extern const char rol_page_start[] asm("_binary_rol_page_html_start");
extern const char rol_page_end[] asm("_binary_rol_page_html_end");

/* Session task: start/stop and the public calls. HTTP server task: handlers.
 * The table and the notices are shared under `lock`; the table lives in PSRAM
 * only while the server runs. */
static struct {
    httpd_handle_t server;
    SemaphoreHandle_t lock;
    rol_players_t *table;
    char notices[NOTICES_CAP];
} s;

static void lock(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

/* Caller holds the lock. Notices that do not fit are dropped (the master can
 * still ask for the player status). */
static void add_notice_locked(const char *text)
{
    const size_t used = strlen(s.notices);
    if (used + 1 + strlen(text) < sizeof(s.notices)) {
        snprintf(s.notices + used, sizeof(s.notices) - used, "%s%s", used ? " " : "", text);
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root);
}

/* Read the whole body (content_len <= cap - 1) into buf, NUL-terminated. */
static bool recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) {
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return true;
}

static void query_token(httpd_req_t *req, char *token, size_t cap, unsigned *seq)
{
    char query[128] = "";
    token[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }
    (void)httpd_query_key_value(query, "t", token, cap);
    char number[12] = "";
    if (seq && httpd_query_key_value(query, "s", number, sizeof(number)) == ESP_OK) {
        *seq = (unsigned)strtoul(number, NULL, 10);
    }
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL that is not part of the page. */
    const size_t len = (size_t)(rol_page_end - rol_page_start);
    return httpd_resp_send(req, rol_page_start, len > 0 ? (ssize_t)(len - 1) : 0);
}

static esp_err_t join_post(httpd_req_t *req)
{
    char name[JOIN_BODY_MAX + 1] = {0};
    if (!recv_body(req, name, sizeof(name))) {
        return send_error(req, "Escribe un nombre válido.");
    }
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    char token[ROL_PLAYER_TOKEN_CAP];
    for (size_t i = 0; i < sizeof(raw); ++i) {
        snprintf(token + 2 * i, 3, "%02x", raw[i]);
    }
    rol_join_result_t result = ROL_JOIN_FULL;
    char shown[ROL_PLAYER_NAME_CAP] = "";
    lock();
    if (s.table) {
        result = rol_players_join(s.table, name, token);
        const rol_player_t *p = rol_players_by_token(s.table, token);
        if (p) {
            snprintf(shown, sizeof(shown), "%s", p->name);
            char notice[96];
            snprintf(notice, sizeof(notice),
                     result == ROL_JOIN_REJOINED ? "%s ha vuelto a entrar desde otro móvil."
                                                 : "%s se ha conectado al móvil.", p->name);
            add_notice_locked(notice);
        }
    }
    unlock();
    if (result != ROL_JOIN_OK && result != ROL_JOIN_REJOINED) {
        return send_error(req, result == ROL_JOIN_FULL ? "La partida está llena (máximo 6)."
                                                       : "Escribe un nombre válido.");
    }
    ESP_LOGI(TAG, "player %s: %s", result == ROL_JOIN_REJOINED ? "rejoined" : "joined", shown);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "token", token);
    cJSON_AddStringToObject(root, "nombre", shown);
    return send_json(req, root);
}

static esp_err_t me_get(httpd_req_t *req)
{
    char token[ROL_PLAYER_TOKEN_CAP];
    query_token(req, token, sizeof(token), NULL);
    cJSON *root = cJSON_CreateObject();
    bool found = false;
    lock();
    const rol_player_t *p = s.table ? rol_players_by_token(s.table, token) : NULL;
    if (p) {
        found = true;
        cJSON_AddStringToObject(root, "nombre", p->name);
        if (p->secret[0]) {
            cJSON_AddStringToObject(root, "secreto", p->secret);
        } else {
            cJSON_AddNullToObject(root, "secreto");
        }
        cJSON_AddNumberToObject(root, "seq", p->secret_seq);
        cJSON_AddBoolToObject(root, "leido", !rol_players_unread(p));
        if (p->sheet[0]) {
            cJSON_AddStringToObject(root, "ficha", p->sheet);
        } else {
            cJSON_AddNullToObject(root, "ficha");
        }
        cJSON_AddNumberToObject(root, "ficha_seq", p->sheet_seq);
    }
    unlock();
    cJSON_AddBoolToObject(root, "ok", found);
    return send_json(req, root);
}

static esp_err_t seen_post(httpd_req_t *req)
{
    char token[ROL_PLAYER_TOKEN_CAP];
    unsigned seq = 0;
    query_token(req, token, sizeof(token), &seq);
    lock();
    const bool ok = s.table && rol_players_mark_seen(s.table, token, seq);
    unlock();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    return send_json(req, root);
}

static esp_err_t game_get(httpd_req_t *req)
{
    char *json = app_rol_web_load_game();
    if (!json) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_error(req, "No hay ninguna partida guardada en Co.");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"partida_cogitador.json\"");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t game_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= ROL_GAME_MAX_BYTES) {
        return send_error(req, "El archivo no es una partida de Co o es demasiado grande.");
    }
    char *body = heap_caps_malloc(ROL_GAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return send_error(req, "Co no tiene memoria libre ahora mismo.");
    }
    rol_game_meta_t meta;
    const bool ok = recv_body(req, body, ROL_GAME_MAX_BYTES) &&
                    rol_game_restore(body, &meta, NULL, false) &&
                    app_rol_web_store_game(body) == ESP_OK;
    heap_caps_free(body);
    if (!ok) {
        return send_error(req, "El archivo no es una partida de Co o es demasiado grande.");
    }
    ESP_LOGI(TAG, "saved game uploaded: %s", meta.title);
    char notice[160];
    snprintf(notice, sizeof(notice), "Se ha subido desde el móvil la partida guardada «%s».",
             meta.title[0] ? meta.title : "sin título");
    lock();
    add_notice_locked(notice);
    unlock();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "titulo", meta.title);
    return send_json(req, root);
}

static void migrate_legacy_game(void);

esp_err_t app_rol_web_start(void)
{
    migrate_legacy_game();
    if (s.server) {
        return ESP_OK;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    rol_players_t *table = heap_caps_calloc(1, sizeof(*table), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!table) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    s.table = table;
    s.notices[0] = '\0';
    unlock();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32769;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.stack_size = SERVER_STACK;
    esp_err_t err = httpd_start(&s.server, &config);
    if (err == ESP_OK) {
        const httpd_uri_t uris[] = {
            { .uri = "/", .method = HTTP_GET, .handler = page_get },
            { .uri = "/join", .method = HTTP_POST, .handler = join_post },
            { .uri = "/me", .method = HTTP_GET, .handler = me_get },
            { .uri = "/seen", .method = HTTP_POST, .handler = seen_post },
            { .uri = "/partida", .method = HTTP_GET, .handler = game_get },
            { .uri = "/partida", .method = HTTP_POST, .handler = game_post },
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]) && err == ESP_OK; ++i) {
            err = httpd_register_uri_handler(s.server, &uris[i]);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "phone page unavailable (%s)", esp_err_to_name(err));
        app_rol_web_stop();
        return err;
    }
    char address[40] = "";
    (void)app_rol_web_address(address, sizeof(address));
    ESP_LOGI(TAG, "phone page at %s", address[0] ? address : "(no IP yet)");
    return ESP_OK;
}

void app_rol_web_stop(void)
{
    if (s.server) {
        (void)httpd_stop(s.server); /* waits for running handlers */
        s.server = NULL;
        ESP_LOGI(TAG, "phone page stopped");
    }
    if (s.lock) {
        lock();
        rol_players_t *table = s.table;
        s.table = NULL;
        s.notices[0] = '\0';
        unlock();
        if (table) {
            memset(table, 0, sizeof(*table)); /* secrets do not linger in PSRAM */
            heap_caps_free(table);
        }
    }
}

bool app_rol_web_running(void)
{
    return s.server != NULL;
}

esp_err_t app_rol_web_address(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(out, cap, "http://" IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

static app_rol_web_put_t to_put(rol_put_result_t r)
{
    return r == ROL_PUT_PHONE ? APP_ROL_WEB_PHONE
           : (r == ROL_PUT_PENDING ? APP_ROL_WEB_PENDING : APP_ROL_WEB_FAILED);
}

app_rol_web_put_t app_rol_web_deliver(const char *player, const char *secret)
{
    if (!s.lock) {
        return APP_ROL_WEB_FAILED;
    }
    lock();
    const rol_put_result_t r = s.table ? rol_players_deliver(s.table, player, secret)
                                       : ROL_PUT_FAILED;
    unlock();
    return to_put(r);
}

app_rol_web_put_t app_rol_web_set_sheet(const char *player, const char *sheet)
{
    if (!s.lock) {
        return APP_ROL_WEB_FAILED;
    }
    lock();
    const rol_put_result_t r = s.table ? rol_players_set_sheet(s.table, player, sheet)
                                       : ROL_PUT_FAILED;
    unlock();
    return to_put(r);
}

size_t app_rol_web_players(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!s.lock) {
        return 0;
    }
    lock();
    const size_t n = s.table ? rol_players_list(s.table, out, cap) : 0;
    unlock();
    return n;
}

char *app_rol_web_status_json(void)
{
    if (!s.lock) {
        return NULL;
    }
    cJSON *list = cJSON_CreateArray();
    if (!list) {
        return NULL;
    }
    bool running = false;
    lock();
    if (s.table) {
        running = true;
        for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
            const rol_player_t *p = &s.table->players[i];
            if (!p->used) {
                continue;
            }
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                break;
            }
            cJSON_AddStringToObject(item, "nombre", p->name);
            cJSON_AddBoolToObject(item, "movil", p->joined);
            cJSON_AddBoolToObject(item, "secreto_sin_leer", rol_players_unread(p));
            cJSON_AddBoolToObject(item, "ficha", p->sheet[0] != '\0');
            cJSON_AddItemToArray(list, item);
        }
    }
    unlock();
    char *json = running ? cJSON_PrintUnformatted(list) : NULL;
    cJSON_Delete(list);
    return json;
}

size_t app_rol_web_take_notices(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!s.lock || !s.notices[0]) {   /* racy peek: a missed notice comes next tick */
        return 0;
    }
    lock();
    snprintf(out, cap, "%s", s.notices);
    s.notices[0] = '\0';
    unlock();
    return strlen(out);
}

char *app_rol_web_build_game(const rol_game_meta_t *meta, const char *summary)
{
    if (!s.lock) {
        return rol_game_build(meta, summary, NULL);
    }
    lock();
    char *json = rol_game_build(meta, summary, s.table);
    unlock();
    return json;
}

/* True while the legacy namespace still holds entries. Erasing its keys leaves the
 * namespace name behind, so nvs_open() alone would report it on every boot. */
static bool legacy_has_entries(const char *ns)
{
    nvs_iterator_t it = NULL;
    const esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    nvs_release_iterator(it);
    return err == ESP_OK;
}

/* One-time move of the 1.6 saved game, on the session task: from app_rol_web_start()
 * before the server exists, or from an earlier store/load call. Later calls (including
 * the server's GET /partida) only read the flag. */
static void migrate_legacy_game(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    if (!legacy_has_entries(LEGACY_GAME_NAMESPACE)) {
        return;
    }
    nvs_handle_t from;
    if (nvs_open(LEGACY_GAME_NAMESPACE, NVS_READWRITE, &from) != ESP_OK) {
        return;
    }
    esp_err_t err = ESP_OK;
    size_t len = 0;
    if (nvs_get_blob(from, GAME_KEY, NULL, &len) == ESP_OK && len > 1 &&
            len <= ROL_GAME_MAX_BYTES) {
        char *json = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        nvs_handle_t to;
        err = json ? nvs_get_blob(from, GAME_KEY, json, &len) : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = nvs_open(GAME_NAMESPACE, NVS_READWRITE, &to);
            if (err == ESP_OK) {
                size_t have = 0;
                if (nvs_get_blob(to, GAME_KEY, NULL, &have) != ESP_OK) {
                    err = nvs_set_blob(to, GAME_KEY, json, len);
                }
                if (err == ESP_OK) {
                    err = nvs_commit(to);
                }
                nvs_close(to);
            }
        }
        free(json);
    }
    if (err == ESP_OK && nvs_erase_all(from) == ESP_OK) {
        err = nvs_commit(from);
    }
    nvs_close(from);
    ESP_LOGI(TAG, "legacy saved game migrated (%s)", esp_err_to_name(err));
}

esp_err_t app_rol_web_store_game(const char *json)
{
    migrate_legacy_game();
    if (!json || !rol_game_valid(json, strlen(json))) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GAME_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    const size_t len = strlen(json) + 1;
    err = nvs_set_blob(handle, GAME_KEY, json, len);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        /* The old copy and the new one do not fit together: replace it. */
        (void)nvs_erase_key(handle, GAME_KEY);
        err = nvs_set_blob(handle, GAME_KEY, json, len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved game not stored (%s)", esp_err_to_name(err));
    }
    return err;
}

char *app_rol_web_load_game(void)
{
    migrate_legacy_game();
    nvs_handle_t handle;
    if (nvs_open(GAME_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    char *json = NULL;
    if (nvs_get_blob(handle, GAME_KEY, NULL, &len) == ESP_OK && len > 1 &&
            len <= ROL_GAME_MAX_BYTES) {
        json = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (json && (nvs_get_blob(handle, GAME_KEY, json, &len) != ESP_OK ||
                     json[len - 1] != '\0')) {
            free(json);
            json = NULL;
        }
    }
    nvs_close(handle);
    return json;
}

bool app_rol_web_restore_game(const char *json, rol_game_meta_t *meta, bool with_secrets)
{
    if (!s.lock) {
        return false;
    }
    lock();
    const bool ok = s.table && rol_game_restore(json, meta, s.table, with_secrets);
    unlock();
    return ok;
}
