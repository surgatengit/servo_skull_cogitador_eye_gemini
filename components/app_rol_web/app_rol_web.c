// SPDX-License-Identifier: Apache-2.0
#include "app_rol_web.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rol_players.h"

static const char *TAG = "app_rol_web";

#define JOIN_BODY_MAX   96
#define SERVER_STACK    6144

extern const char rol_page_start[] asm("_binary_rol_page_html_start");
extern const char rol_page_end[] asm("_binary_rol_page_html_end");

/* Session task: start/stop/deliver. HTTP server task: handlers. The table is
 * shared under `lock`; it lives in PSRAM only while the server runs. */
static struct {
    httpd_handle_t server;
    SemaphoreHandle_t lock;
    rol_players_t *table;
} s;

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
    if (req->content_len == 0 || req->content_len > JOIN_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, name + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    char token[ROL_PLAYER_TOKEN_CAP];
    for (size_t i = 0; i < sizeof(raw); ++i) {
        snprintf(token + 2 * i, 3, "%02x", raw[i]);
    }
    rol_join_result_t result = ROL_JOIN_FULL;
    char shown[ROL_PLAYER_NAME_CAP] = "";
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.table) {
        result = rol_players_join(s.table, name, token);
        const rol_player_t *p = result == ROL_JOIN_OK ? rol_players_by_token(s.table, token) : NULL;
        if (p) {
            snprintf(shown, sizeof(shown), "%s", p->name);
        }
    }
    xSemaphoreGive(s.lock);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", result == ROL_JOIN_OK);
    if (result == ROL_JOIN_OK) {
        cJSON_AddStringToObject(root, "token", token);
        cJSON_AddStringToObject(root, "nombre", shown);
        ESP_LOGI(TAG, "player joined: %s", shown);
    } else {
        cJSON_AddStringToObject(root, "error",
                                result == ROL_JOIN_TAKEN ? "Ese nombre ya está en la partida."
                                : result == ROL_JOIN_FULL ? "La partida está llena (máximo 6)."
                                : "Escribe un nombre válido.");
    }
    return send_json(req, root);
}

static esp_err_t secret_get(httpd_req_t *req)
{
    char query[96] = "";
    char token[ROL_PLAYER_TOKEN_CAP] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "t", token, sizeof(token));
    }
    cJSON *root = cJSON_CreateObject();
    bool found = false;
    xSemaphoreTake(s.lock, portMAX_DELAY);
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
    }
    xSemaphoreGive(s.lock);
    cJSON_AddBoolToObject(root, "ok", found);
    return send_json(req, root);
}

esp_err_t app_rol_web_start(void)
{
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
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.table = table;
    xSemaphoreGive(s.lock);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32769;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    config.stack_size = SERVER_STACK;
    esp_err_t err = httpd_start(&s.server, &config);
    if (err == ESP_OK) {
        const httpd_uri_t uris[] = {
            { .uri = "/", .method = HTTP_GET, .handler = page_get },
            { .uri = "/join", .method = HTTP_POST, .handler = join_post },
            { .uri = "/secret", .method = HTTP_GET, .handler = secret_get },
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
        xSemaphoreTake(s.lock, portMAX_DELAY);
        rol_players_t *table = s.table;
        s.table = NULL;
        xSemaphoreGive(s.lock);
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

bool app_rol_web_deliver(const char *player, const char *secret)
{
    if (!s.lock) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool ok = s.table && rol_players_deliver(s.table, player, secret);
    xSemaphoreGive(s.lock);
    return ok;
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
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const size_t n = s.table ? rol_players_list(s.table, out, cap) : 0;
    xSemaphoreGive(s.lock);
    return n;
}
