#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bench_types.h"

typedef struct {
    bench_state_t state;
    bench_fault_t fault;

    bool fc1_safe;
    bool fc2_torque_reached;

    bool contactor_active;
    bool ready_led_active;
    bool test_led_active;
    bool fault_led_active;

    float pwm_percent;
    float current_a;
    float maximum_current_a;

    uint32_t elapsed_ms;
    uint32_t sequence_number;

    bool simulation_enabled;
} bench_status_t;
