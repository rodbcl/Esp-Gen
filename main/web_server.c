#include "web_server.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "automation.h"
#include "history.h"
#include "wifi_ap.h"

static const char *TAG = "web";

#define BODY_MAX    1024

// Página gerada por web/pack.py no build
extern const uint8_t page_start[] asm("_binary_index_html_gz_start");
extern const uint8_t page_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;

/* ---------- Campos do perfil (nomes iguais aos da página) ---------- */
typedef enum { F_U32, F_BOOL } field_type_t;

static const struct {
    const char *name;
    size_t off;
    field_type_t type;
} PROFILE_FIELDS[] = {
#define U32(f)  { #f, offsetof(auto_profile_t, f), F_U32 }
#define BOOL(f) { #f, offsetof(auto_profile_t, f), F_BOOL }
    U32(rate_min_mhz), U32(rate_max_mhz), U32(regime_s), U32(jitter_pct), U32(pulse_on_us),
    BOOL(stops_enabled), U32(stop_every_min_s), U32(stop_every_max_s), U32(stop_len_min_s), U32(stop_len_max_s),
    BOOL(noise_enabled), U32(noise_every_min_ms), U32(noise_every_max_ms), U32(noise_on_min_us), U32(noise_on_max_us),
    U32(noise_burst_min), U32(noise_burst_max), U32(noise_gap_us), U32(noise_guard_us),
#undef U32
#undef BOOL
};

/* ---------- Saída em partes, com buffer ---------- */
typedef struct {
    httpd_req_t *req;
    char buf[1024];
    size_t len;
} writer_t;

static void writer_flush(writer_t *w)
{
    if (w->len) {
        httpd_resp_send_chunk(w->req, w->buf, w->len);
        w->len = 0;
    }
}

static void writer_out(void *ctx, const char *text)
{
    writer_t *w = ctx;
    size_t n = strlen(text);
    if (w->len + n > sizeof(w->buf)) {
        writer_flush(w);
    }
    if (n > sizeof(w->buf)) {
        httpd_resp_send_chunk(w->req, text, n);
        return;
    }
    memcpy(w->buf + w->len, text, n);
    w->len += n;
}

static void writer_end(writer_t *w)
{
    writer_flush(w);
    httpd_resp_send_chunk(w->req, NULL, 0);
}

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

// Lê o corpo da requisição como JSON; NULL em caso de erro (já respondido)
static cJSON *read_json(httpd_req_t *req)
{
    char body[BODY_MAX + 1];
    if (req->content_len > BODY_MAX) {
        send_error(req, "413 Payload Too Large", "Requisição grande demais.");
        return NULL;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            send_error(req, "400 Bad Request", "Falha ao ler a requisição.");
            return NULL;
        }
        got += n;
    }
    body[got] = 0;
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_error(req, "400 Bad Request", "JSON inválido.");
    }
    return json;
}

static bool json_number(const cJSON *obj, const char *name, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    *out = it->valuedouble;
    return true;
}

/* ---------- Handlers ---------- */
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)page_start, page_end - page_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    automation_status_t st = automation_status();
    uint32_t valid[HISTORY_MINUTES], noise[HISTORY_MINUTES], rejects[HISTORY_MINUTES];
    history_per_minute(valid, noise, rejects);

    writer_t w = { .req = req };
    char line[256];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    snprintf(line, sizeof(line),
             "{\"ap\":\"%s\",\"active\":%s,\"running\":%s,\"time_ok\":%s,\"started_at\":%" PRIu32
             ",\"elapsed_s\":%" PRIu32 ",\"valid\":%" PRIu64 ",\"noise\":%" PRIu64 ",\"stops\":%" PRIu32,
             wifi_ap_ssid(), st.active ? "true" : "false", st.running ? "true" : "false",
             st.time_ok ? "true" : "false", st.started_at, st.elapsed_s, st.tally.valid, st.tally.noise,
             st.tally.stops);
    writer_out(&w, line);
    snprintf(line, sizeof(line), ",\"machine\":\"%s\",\"stop_left_s\":%" PRIu32 ",\"rate_mhz\":%" PRIu32,
             st.gen.stopped ? "stop" : "run", st.gen.stop_left_s, st.gen.rate_mhz);
    writer_out(&w, line);
    for (int k = 0; k < 2; k++) {
        const uint32_t *v = k ? noise : valid;
        writer_out(&w, k ? "],\"noise_per_min\":[" : ",\"per_min\":[");
        for (int i = 0; i < HISTORY_MINUTES; i++) {
            snprintf(line, sizeof(line), "%s%" PRIu32, i ? "," : "", v[i]);
            writer_out(&w, line);
        }
    }
    writer_out(&w, "]}");
    writer_end(&w);
    return ESP_OK;
}

static esp_err_t profile_get(httpd_req_t *req)
{
    auto_profile_t p;
    automation_get_profile(&p);
    writer_t w = { .req = req };
    char line[64];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (size_t i = 0; i < sizeof(PROFILE_FIELDS) / sizeof(PROFILE_FIELDS[0]); i++) {
        const uint8_t *f = (const uint8_t *)&p + PROFILE_FIELDS[i].off;
        if (PROFILE_FIELDS[i].type == F_BOOL) {
            snprintf(line, sizeof(line), "%c\"%s\":%s", i ? ',' : '{', PROFILE_FIELDS[i].name,
                     *(const bool *)f ? "true" : "false");
        } else {
            snprintf(line, sizeof(line), "%c\"%s\":%" PRIu32, i ? ',' : '{', PROFILE_FIELDS[i].name,
                     *(const uint32_t *)f);
        }
        writer_out(&w, line);
    }
    writer_out(&w, "}");
    writer_end(&w);
    return ESP_OK;
}

static esp_err_t profile_post(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    if (!json) {
        return ESP_OK;
    }
    auto_profile_t p;
    automation_get_profile(&p);
    for (size_t i = 0; i < sizeof(PROFILE_FIELDS) / sizeof(PROFILE_FIELDS[0]); i++) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(json, PROFILE_FIELDS[i].name);
        uint8_t *f = (uint8_t *)&p + PROFILE_FIELDS[i].off;
        if (PROFILE_FIELDS[i].type == F_BOOL && cJSON_IsBool(it)) {
            *(bool *)f = cJSON_IsTrue(it);
        } else if (PROFILE_FIELDS[i].type == F_U32 && cJSON_IsNumber(it) && it->valuedouble >= 0 &&
                   it->valuedouble <= UINT32_MAX) {
            *(uint32_t *)f = (uint32_t)it->valuedouble;
        } else {
            cJSON_Delete(json);
            char msg[96];
            snprintf(msg, sizeof(msg), "Campo do perfil inválido: %s.", PROFILE_FIELDS[i].name);
            return send_error(req, "400 Bad Request", msg);
        }
    }
    cJSON_Delete(json);
    const char *err = automation_set_profile(&p);
    if (err) {
        return send_error(req, "409 Conflict", err);
    }
    return profile_get(req);
}

static esp_err_t start_post(httpd_req_t *req)
{
    const char *err = automation_start();
    return err ? send_error(req, "409 Conflict", err) : send_json(req, "{}");
}

static esp_err_t stop_post(httpd_req_t *req)
{
    automation_stop();
    return send_json(req, "{}");
}

static esp_err_t time_post(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    if (!json) {
        return ESP_OK;
    }
    double epoch, tz = 0;
    bool ok = json_number(json, "epoch", &epoch) && epoch > 1600000000;
    json_number(json, "tz", &tz);
    cJSON_Delete(json);
    if (!ok) {
        return send_error(req, "400 Bad Request", "Hora inválida.");
    }
    automation_set_time((uint32_t)epoch, (int32_t)tz);
    return send_json(req, "{}");
}

static esp_err_t check_post(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    if (!json) {
        return ESP_OK;
    }
    double value;
    bool ok = json_number(json, "value", &value) && value >= 0;
    cJSON_Delete(json);
    if (!ok) {
        return send_error(req, "400 Bad Request", "Digite o valor mostrado no contador.");
    }
    uint64_t expected;
    const char *err = automation_check((uint64_t)value, &expected);
    if (err) {
        return send_error(req, "409 Conflict", err);
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"value\":%" PRIu64 ",\"expected\":%" PRIu64 "}", (uint64_t)value, expected);
    return send_json(req, out);
}

static esp_err_t history_get(httpd_req_t *req)
{
    writer_t w = { .req = req };
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    history_json(writer_out, &w);
    writer_end(&w);
    return ESP_OK;
}

static esp_err_t csv_get(httpd_req_t *req)
{
    bool events = strstr(req->uri, "events") != NULL;
    writer_t w = { .req = req };
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       events ? "attachment; filename=\"espgen-eventos.csv\""
                              : "attachment; filename=\"espgen-intervalos.csv\"");
    if (events) {
        history_csv_events(writer_out, &w);
    } else {
        history_csv_intervals(writer_out, &w);
    }
    writer_end(&w);
    return ESP_OK;
}

// Qualquer outro endereço leva à página (ajuda quando o celular abre outra URL ao conectar)
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- Início e fim ---------- */
esp_err_t web_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar o servidor: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/profile",     .method = HTTP_GET,  .handler = profile_get },
        { .uri = "/api/profile",     .method = HTTP_POST, .handler = profile_post },
        { .uri = "/api/start",       .method = HTTP_POST, .handler = start_post },
        { .uri = "/api/stop",        .method = HTTP_POST, .handler = stop_post },
        { .uri = "/api/time",        .method = HTTP_POST, .handler = time_post },
        { .uri = "/api/check",       .method = HTTP_POST, .handler = check_post },
        { .uri = "/api/history",     .method = HTTP_GET,  .handler = history_get },
        { .uri = "/api/history.csv", .method = HTTP_GET,  .handler = csv_get },
        { .uri = "/api/events.csv",  .method = HTTP_GET,  .handler = csv_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "servidor no ar: http://%s", WIFI_AP_IP);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
