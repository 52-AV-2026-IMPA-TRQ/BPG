#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool fc1_safe;
    bool fc2_torque_reached;
    float current_a;
} bench_simulator_inputs_t;

esp_err_t bench_simulator_start(void);
esp_err_t bench_simulator_stop(void);
bool bench_simulator_is_running(void);

/* Thread-safe access to the simulated inputs and measurements. */
void bench_simulator_get_inputs(bench_simulator_inputs_t *inputs);

/* Overrides all inputs and disables the automatic current model. */
void bench_simulator_set_inputs(const bench_simulator_inputs_t *inputs);

void bench_simulator_set_fc1_safe(bool fc1_safe);
void bench_simulator_set_fc2_torque_reached(bool fc2_torque_reached);

/* Re-enables the deterministic current model (default). */
void bench_simulator_enable_auto_current(void);

#ifdef __cplusplus
}
#endif
