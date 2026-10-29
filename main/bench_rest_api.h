#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Registers the simulated-bench diagnostic POST endpoints. */
esp_err_t bench_rest_api_register(httpd_handle_t server, void *user_ctx);
