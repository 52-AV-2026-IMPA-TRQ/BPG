#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bench_commands.h"
#include "bench_core.h"
#include "bench_simulator.h"
#include "bench_rest_api.h"

#define BENCH_API_MAX_BODY_BYTES 256

static const char *TAG = "bench-api";

typedef struct {
    const char *name;
    bench_command_type_t type;
} bench_command_entry_t;

static const bench_command_entry_t s_commands[] = {
    { "START", BENCH_COMMAND_START },
    { "STOP", BENCH_COMMAND_STOP },
    { "RESET", BENCH_COMMAND_RESET },
};

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to serialize response");
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return result;
}

/*
 * Reads a non-empty body into a bounded buffer. On failure the HTTP error
 * response has already been sent and the caller must only return ESP_FAIL.
 */
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t capacity)
{
    const size_t total = req->content_len;
    if (total == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (total >= capacity) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < total) {
        const int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout reading body");
            }
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    buf[received] = '\0';
    return ESP_OK;
}

/* True when every member of the JSON object is one of the allowed names. */
static bool only_allowed_fields(const cJSON *root, const char *const *allowed, size_t count)
{
    for (const cJSON *child = root->child; child != NULL; child = child->next) {
        bool found = false;
        for (size_t i = 0; i < count && !found; i++) {
            found = (child->string != NULL) && (strcmp(child->string, allowed[i]) == 0);
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static const char *const s_command_fields[] = { "command" };
static const char *const s_simulation_fields[] = { "fc1Safe", "fc2TorqueReached", "currentA", "autoCurrent" };

static const bench_command_entry_t *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(name, s_commands[i].name) == 0) {
            return &s_commands[i];
        }
    }
    return NULL;
}

static esp_err_t command_response(httpd_req_t *req, const char *status, bool accepted, const char *name)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(root, "accepted", accepted);
    cJSON_AddStringToObject(root, "command", name);
    return send_json(req, status, root);
}

static esp_err_t bench_command_post_handler(httpd_req_t *req)
{
    char body[BENCH_API_MAX_BODY_BYTES];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    const bench_command_entry_t *entry = NULL;
    /* Exactly one member, and it must be "command". */
    if (cJSON_IsObject(root) && root->child != NULL && root->child->next == NULL &&
        only_allowed_fields(root, s_command_fields, 1)) {
        const cJSON *item = root->child;
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            entry = find_command(item->valuestring);
        }
    }
    cJSON_Delete(root);

    if (entry == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown or missing command");
    }

    const bench_command_t command = {
        .type = entry->type,
        .source = BENCH_COMMAND_SOURCE_WEB,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000)
    };

    if (bench_core_send_command(&command) != ESP_OK) {
        ESP_LOGW(TAG, "command %s not queued", entry->name);
        return command_response(req, "503 Service Unavailable", false, entry->name);
    }

    return command_response(req, "202 Accepted", true, entry->name);
}

typedef struct {
    bool has_fc1;
    bool fc1_safe;
    bool has_fc2;
    bool fc2_torque_reached;
    bool has_current;
    float current_a;
    bool has_auto_current;
    bool auto_current;
} simulation_request_t;

static bool parse_optional_bool(const cJSON *root, const char *key, bool *present, bool *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *present = true;
    *value = cJSON_IsTrue(item);
    return true;
}

static bool parse_optional_current(const cJSON *root, simulation_request_t *req)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "currentA");
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)3.0e38f) {
        return false;
    }
    req->has_current = true;
    req->current_a = (float)item->valuedouble;
    return true;
}

static bool parse_simulation_request(const cJSON *root, simulation_request_t *req)
{
    memset(req, 0, sizeof(*req));

    if (!cJSON_IsObject(root) ||
        !only_allowed_fields(root, s_simulation_fields,
                             sizeof(s_simulation_fields) / sizeof(s_simulation_fields[0]))) {
        return false;
    }
    if (!parse_optional_bool(root, "fc1Safe", &req->has_fc1, &req->fc1_safe) ||
        !parse_optional_bool(root, "fc2TorqueReached", &req->has_fc2, &req->fc2_torque_reached) ||
        !parse_optional_bool(root, "autoCurrent", &req->has_auto_current, &req->auto_current) ||
        !parse_optional_current(root, req)) {
        return false;
    }

    /* A manual current contradicts enabling the automatic model. */
    return !(req->has_current && req->has_auto_current && req->auto_current);
}

static void apply_simulation_request(const simulation_request_t *req)
{
    bench_simulator_inputs_t inputs;
    bench_simulator_get_inputs(&inputs);

    if (req->has_fc1) {
        inputs.fc1_safe = req->fc1_safe;
    }
    if (req->has_fc2) {
        inputs.fc2_torque_reached = req->fc2_torque_reached;
    }

    if (req->has_current || (req->has_auto_current && !req->auto_current)) {
        /* Manual current: set_inputs freezes the automatic model. */
        if (req->has_current) {
            inputs.current_a = req->current_a;
        }
        bench_simulator_set_inputs(&inputs);
        return;
    }

    if (req->has_fc1) {
        bench_simulator_set_fc1_safe(inputs.fc1_safe);
    }
    if (req->has_fc2) {
        bench_simulator_set_fc2_torque_reached(inputs.fc2_torque_reached);
    }
    if (req->has_auto_current) {
        bench_simulator_enable_auto_current();
    }
}

static esp_err_t simulation_response(httpd_req_t *req)
{
    bench_simulator_inputs_t inputs;
    bench_simulator_get_inputs(&inputs);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(root, "fc1Safe", inputs.fc1_safe);
    cJSON_AddBoolToObject(root, "fc2TorqueReached", inputs.fc2_torque_reached);
    cJSON_AddNumberToObject(root, "currentA", inputs.current_a);
    return send_json(req, "200 OK", root);
}

static esp_err_t bench_simulation_post_handler(httpd_req_t *req)
{
    char body[BENCH_API_MAX_BODY_BYTES];
    simulation_request_t request;

    if (req->content_len == 0) {
        return simulation_response(req);
    }
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    const bool valid = parse_simulation_request(root, &request);
    cJSON_Delete(root);

    if (!valid) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid simulation fields");
    }

    apply_simulation_request(&request);
    return simulation_response(req);
}

esp_err_t bench_rest_api_register(httpd_handle_t server, void *user_ctx)
{
    const httpd_uri_t command_uri = {
        .uri = "/api/v1/bench/command",
        .method = HTTP_POST,
        .handler = bench_command_post_handler,
        .user_ctx = user_ctx
    };
    const httpd_uri_t simulation_uri = {
        .uri = "/api/v1/bench/simulation",
        .method = HTTP_POST,
        .handler = bench_simulation_post_handler,
        .user_ctx = user_ctx
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &command_uri), TAG, "register command");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &simulation_uri), TAG, "register simulation");
    return ESP_OK;
}
