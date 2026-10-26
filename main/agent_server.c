#include "agent_server.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "agent.h"

static const char *TAG = "agent_srv";

static httpd_handle_t s_server;

/* ---------- Auxiliares ---------- */
static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, json);
}

/* ---------- Handlers ---------- */
static esp_err_t health_get(httpd_req_t *req)
{
    agent_status_t st = agent_status();
    char json[256];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"version\":\"%s\",\"mode\":\"agent\",\"ip\":\"%s\",\"hostname\":\"%s\",\"uptime_s\":%" PRIu32 "}",
             esp_app_get_description()->version, st.ip, wifi_sta_hostname(),
             (uint32_t)(esp_timer_get_time() / 1000000));
    return send_json(req, json);
}

static esp_err_t status_get(httpd_req_t *req)
{
    return send_json(req, "{\"state\":\"idle\"}");
}

static esp_err_t start_post(httpd_req_t *req)
{
    return send_error(req, "501 Not Implemented", "Programas de pulsos ainda não implementados.");
}

static esp_err_t stop_post(httpd_req_t *req)
{
    return send_json(req, "{\"state\":\"idle\"}");
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    return send_error(req, "404 Not Found", "Rota inexistente. Use /health, /status, /start ou /stop.");
}

/* ---------- Início e fim ---------- */
esp_err_t agent_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar o servidor: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/health", .method = HTTP_GET,  .handler = health_get },
        { .uri = "/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/start",  .method = HTTP_POST, .handler = start_post },
        { .uri = "/stop",   .method = HTTP_POST, .handler = stop_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "servidor de comandos no ar");
    return ESP_OK;
}

void agent_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
