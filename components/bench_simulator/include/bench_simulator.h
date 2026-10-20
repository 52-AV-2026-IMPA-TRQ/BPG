#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bench_simulator_start(void);
esp_err_t bench_simulator_stop(void);
bool bench_simulator_is_running(void);

#ifdef __cplusplus
}
#endif
