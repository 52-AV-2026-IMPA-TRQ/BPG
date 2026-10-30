#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t pwm_frequency_hz;
    uint8_t pwm_resolution_bits;

    float initial_pwm_percent;
    float maximum_pwm_percent;
    float pwm_increment_percent;

    uint32_t ramp_period_ms;
    uint32_t contactor_settle_ms;
    uint32_t maximum_test_time_ms;

    float maximum_current_a;
} bench_config_t;

/*
 * Single source of truth for test configuration validation.
 * maximum_current_a == 0 is valid and means current protection disabled;
 * negative or non-finite values are rejected.
 * Returns ESP_ERR_INVALID_ARG when config is NULL or invalid.
 */
esp_err_t bench_config_validate(const bench_config_t *config);

#define BENCH_DEFAULT_PWM_FREQUENCY_HZ       1000U
#define BENCH_DEFAULT_PWM_RESOLUTION_BITS    12U
#define BENCH_DEFAULT_INITIAL_PWM_PERCENT     0.0f
#define BENCH_DEFAULT_MAXIMUM_PWM_PERCENT   100.0f
#define BENCH_DEFAULT_PWM_INCREMENT_PERCENT   1.0f
#define BENCH_DEFAULT_RAMP_PERIOD_MS         100U
#define BENCH_DEFAULT_CONTACTOR_SETTLE_MS    250U
#define BENCH_DEFAULT_MAX_TEST_TIME_MS      5000U
#define BENCH_DEFAULT_MAXIMUM_CURRENT_A        0.0f
