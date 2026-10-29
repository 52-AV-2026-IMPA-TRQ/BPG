#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the controller task, the only publisher of the full bench status. */
esp_err_t bench_controller_start(void);

#ifdef __cplusplus
}
#endif
