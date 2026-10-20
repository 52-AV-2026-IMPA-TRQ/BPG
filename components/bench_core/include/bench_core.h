#pragma once

#include "esp_err.h"
#include "bench_config.h"
#include "bench_status.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bench_core_init(void);

void bench_core_get_status(bench_status_t *status);
void bench_core_publish_status(const bench_status_t *status);

void bench_core_get_config(bench_config_t *config);
esp_err_t bench_core_set_config(const bench_config_t *config);

const char *bench_state_to_string(bench_state_t state);
const char *bench_fault_to_string(bench_fault_t fault);

#ifdef __cplusplus
}
#endif
