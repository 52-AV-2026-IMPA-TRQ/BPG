#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "bench_commands.h"
#include "bench_config.h"
#include "bench_status.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bench_core_init(void);

#define BENCH_COMMAND_QUEUE_LENGTH 8U

/* Non-blocking; returns ESP_ERR_NO_MEM when the queue is full. */
esp_err_t bench_core_send_command(const bench_command_t *command);
/* Returns ESP_ERR_TIMEOUT when no command arrived within timeout. */
esp_err_t bench_core_receive_command(bench_command_t *command, TickType_t timeout);

void bench_core_get_status(bench_status_t *status);
void bench_core_publish_status(const bench_status_t *status);

void bench_core_get_config(bench_config_t *config);
esp_err_t bench_core_set_config(const bench_config_t *config);

const char *bench_state_to_string(bench_state_t state);
const char *bench_fault_to_string(bench_fault_t fault);

#ifdef __cplusplus
}
#endif
