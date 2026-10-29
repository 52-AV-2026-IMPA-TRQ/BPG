#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bench_controller.h"
#include "bench_core.h"
#include "bench_simulator.h"

#define BENCH_CONTROLLER_TASK_STACK_SIZE   4096
#define BENCH_CONTROLLER_TASK_PRIORITY        5
#define BENCH_CONTROLLER_PERIOD_MS           10

#define BENCH_CONTROLLER_INITIALIZING_MS    500U
#define BENCH_CONTROLLER_SELF_TEST_MS      1000U
#define BENCH_CONTROLLER_SAFE_CUTOFF_MS      30U
#define BENCH_CONTROLLER_COMPLETED_HOLD_MS 1000U
/* TEMPORARY: short cooldown for the simulated phase; the final value is 120000 ms. */
#define BENCH_CONTROLLER_COOLDOWN_MS       5000U
#define BENCH_CONTROLLER_OVERCURRENT_FACTOR 1.05f

typedef struct {
    bench_status_t status;
    bench_config_t config;
    bench_simulator_inputs_t inputs;
    uint32_t state_entered_ms;
    uint32_t test_start_ms;
    uint32_t last_ramp_ms;
    bool settling;
    bench_state_t cutoff_final_state;
    bench_fault_t cutoff_fault;
} bench_controller_t;

static const char *TAG = "bench_controller";

static TaskHandle_t s_controller_task = NULL;
static bench_controller_t s_ctrl;

static uint32_t bench_controller_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t bench_controller_in_state_ms(const bench_controller_t *ctrl, uint32_t now)
{
    return now - ctrl->state_entered_ms;
}

static void bench_controller_enter_state(bench_controller_t *ctrl, bench_state_t state, uint32_t now)
{
    ESP_LOGI(TAG, "%s -> %s", bench_state_to_string(ctrl->status.state), bench_state_to_string(state));

    ctrl->status.state = state;
    ctrl->state_entered_ms = now;
    ctrl->status.ready_led_active = (state == BENCH_STATE_READY);
    ctrl->status.test_led_active = (state == BENCH_STATE_TESTING);
    ctrl->status.fault_led_active = (state == BENCH_STATE_FAULT);

    if (state == BENCH_STATE_SELF_TEST) {
        ctrl->status.fault = BENCH_FAULT_NONE;
    }
}

static void bench_controller_begin_cutoff(bench_controller_t *ctrl, bench_state_t final_state,
                                          bench_fault_t fault, uint32_t now)
{
    ctrl->status.pwm_percent = 0.0f;
    ctrl->cutoff_final_state = final_state;
    ctrl->cutoff_fault = fault;
    bench_controller_enter_state(ctrl, BENCH_STATE_SAFE_CUTOFF, now);
}

static void bench_controller_start_test(bench_controller_t *ctrl, uint32_t now)
{
    bench_core_get_config(&ctrl->config);

    ctrl->status.contactor_active = true;
    ctrl->status.pwm_percent = 0.0f;
    ctrl->status.elapsed_ms = 0;
    ctrl->status.maximum_current_a = 0.0f;
    ctrl->settling = true;
    bench_controller_enter_state(ctrl, BENCH_STATE_TESTING, now);
}

static void bench_controller_reject(const bench_controller_t *ctrl, const bench_command_t *command)
{
    ESP_LOGW(TAG, "command %d rejected in state %s", (int)command->type,
             bench_state_to_string(ctrl->status.state));
}

static void bench_controller_on_start(bench_controller_t *ctrl, const bench_command_t *command, uint32_t now)
{
    if (ctrl->status.state == BENCH_STATE_READY && ctrl->inputs.fc1_safe) {
        bench_controller_start_test(ctrl, now);
    } else {
        bench_controller_reject(ctrl, command);
    }
}

static void bench_controller_on_stop(bench_controller_t *ctrl, const bench_command_t *command, uint32_t now)
{
    if (ctrl->status.state == BENCH_STATE_TESTING) {
        bench_controller_begin_cutoff(ctrl, BENCH_STATE_STOPPED, BENCH_FAULT_NONE, now);
    } else if (ctrl->status.state == BENCH_STATE_SAFE_CUTOFF) {
        /* STOP overrides a pending COMPLETED, but never hides a pending fault. */
        if (ctrl->cutoff_final_state == BENCH_STATE_COMPLETED) {
            ctrl->cutoff_final_state = BENCH_STATE_STOPPED;
        }
    } else {
        bench_controller_reject(ctrl, command);
    }
}

static void bench_controller_on_reset(bench_controller_t *ctrl, const bench_command_t *command, uint32_t now)
{
    switch (ctrl->status.state) {
    case BENCH_STATE_COMPLETED:
    case BENCH_STATE_STOPPED:
    case BENCH_STATE_FAULT:
        bench_controller_enter_state(ctrl, BENCH_STATE_SELF_TEST, now);
        break;
    default:
        bench_controller_reject(ctrl, command);
        break;
    }
}

static void bench_controller_handle_command(bench_controller_t *ctrl, const bench_command_t *command,
                                            uint32_t now)
{
    switch (command->type) {
    case BENCH_COMMAND_START:
        bench_controller_on_start(ctrl, command, now);
        break;
    case BENCH_COMMAND_STOP:
        bench_controller_on_stop(ctrl, command, now);
        break;
    case BENCH_COMMAND_RESET:
        bench_controller_on_reset(ctrl, command, now);
        break;
    default:
        bench_controller_reject(ctrl, command);
        break;
    }
}

static void bench_controller_process_commands(bench_controller_t *ctrl, uint32_t now)
{
    bench_command_t command;

    while (bench_core_receive_command(&command, 0) == ESP_OK) {
        bench_controller_handle_command(ctrl, &command, now);
    }
}

static bool bench_controller_overcurrent(const bench_controller_t *ctrl)
{
    const float limit = ctrl->config.maximum_current_a;

    /* A limit of 0 means the protection is disabled. */
    return limit > 0.0f && ctrl->inputs.current_a > limit * BENCH_CONTROLLER_OVERCURRENT_FACTOR;
}

static void bench_controller_ramp(bench_controller_t *ctrl, uint32_t now)
{
    if (now - ctrl->last_ramp_ms < ctrl->config.ramp_period_ms) {
        return;
    }

    ctrl->last_ramp_ms = now;
    float pwm = ctrl->status.pwm_percent + ctrl->config.pwm_increment_percent;
    if (pwm > ctrl->config.maximum_pwm_percent) {
        pwm = ctrl->config.maximum_pwm_percent;
    }
    ctrl->status.pwm_percent = pwm;
}

static void bench_controller_step_settling(bench_controller_t *ctrl, uint32_t now)
{
    if (bench_controller_in_state_ms(ctrl, now) < ctrl->config.contactor_settle_ms) {
        return;
    }

    ctrl->settling = false;
    ctrl->test_start_ms = now;
    ctrl->last_ramp_ms = now;
    ctrl->status.pwm_percent = ctrl->config.initial_pwm_percent;
}

/*
 * Absolute priority: while the logical contactor is active, loss of FC1 forces
 * the final outcome to FAULT/FC1_SECURITY. Runs before command processing so
 * no command can override it. An ongoing SAFE_CUTOFF keeps its 30 ms timer.
 */
static void bench_controller_check_fc1(bench_controller_t *ctrl, uint32_t now)
{
    if (!ctrl->status.contactor_active || ctrl->inputs.fc1_safe) {
        return;
    }

    if (ctrl->status.state == BENCH_STATE_SAFE_CUTOFF) {
        ctrl->cutoff_final_state = BENCH_STATE_FAULT;
        ctrl->cutoff_fault = BENCH_FAULT_FC1_SECURITY;
    } else {
        bench_controller_begin_cutoff(ctrl, BENCH_STATE_FAULT, BENCH_FAULT_FC1_SECURITY, now);
    }
}

static void bench_controller_step_testing(bench_controller_t *ctrl, uint32_t now)
{
    if (ctrl->settling) {
        bench_controller_step_settling(ctrl, now);
        return;
    }

    ctrl->status.elapsed_ms = now - ctrl->test_start_ms;
    bench_controller_ramp(ctrl, now);

    if (ctrl->inputs.current_a > ctrl->status.maximum_current_a) {
        ctrl->status.maximum_current_a = ctrl->inputs.current_a;
    }

    if (bench_controller_overcurrent(ctrl)) {
        bench_controller_begin_cutoff(ctrl, BENCH_STATE_FAULT, BENCH_FAULT_OVERCURRENT, now);
    } else if (ctrl->inputs.fc2_torque_reached) {
        bench_controller_begin_cutoff(ctrl, BENCH_STATE_COMPLETED, BENCH_FAULT_NONE, now);
    } else if (ctrl->status.elapsed_ms >= ctrl->config.maximum_test_time_ms) {
        bench_controller_begin_cutoff(ctrl, BENCH_STATE_FAULT, BENCH_FAULT_TIMEOUT, now);
    }
}

static void bench_controller_step_cutoff(bench_controller_t *ctrl, uint32_t now)
{
    ctrl->status.pwm_percent = 0.0f;

    if (bench_controller_in_state_ms(ctrl, now) < BENCH_CONTROLLER_SAFE_CUTOFF_MS) {
        return;
    }

    ctrl->status.contactor_active = false;
    ctrl->status.fault = ctrl->cutoff_fault;
    bench_controller_enter_state(ctrl, ctrl->cutoff_final_state, now);
}

static void bench_controller_step_self_test(bench_controller_t *ctrl, uint32_t now)
{
    if (bench_controller_in_state_ms(ctrl, now) < BENCH_CONTROLLER_SELF_TEST_MS) {
        return;
    }

    if (ctrl->inputs.fc1_safe) {
        bench_controller_enter_state(ctrl, BENCH_STATE_READY, now);
    } else {
        ctrl->status.fault = BENCH_FAULT_FC1_SECURITY;
        bench_controller_enter_state(ctrl, BENCH_STATE_FAULT, now);
    }
}

static void bench_controller_step_timed(bench_controller_t *ctrl, uint32_t now, uint32_t duration_ms,
                                        bench_state_t next_state)
{
    if (bench_controller_in_state_ms(ctrl, now) >= duration_ms) {
        bench_controller_enter_state(ctrl, next_state, now);
    }
}

static void bench_controller_step(bench_controller_t *ctrl, uint32_t now)
{
    switch (ctrl->status.state) {
    case BENCH_STATE_INITIALIZING:
        bench_controller_step_timed(ctrl, now, BENCH_CONTROLLER_INITIALIZING_MS, BENCH_STATE_SELF_TEST);
        break;
    case BENCH_STATE_SELF_TEST:
        bench_controller_step_self_test(ctrl, now);
        break;
    case BENCH_STATE_TESTING:
        bench_controller_step_testing(ctrl, now);
        break;
    case BENCH_STATE_SAFE_CUTOFF:
        bench_controller_step_cutoff(ctrl, now);
        break;
    case BENCH_STATE_COMPLETED:
        bench_controller_step_timed(ctrl, now, BENCH_CONTROLLER_COMPLETED_HOLD_MS, BENCH_STATE_COOLDOWN);
        break;
    case BENCH_STATE_COOLDOWN:
        bench_controller_step_timed(ctrl, now, BENCH_CONTROLLER_COOLDOWN_MS, BENCH_STATE_SELF_TEST);
        break;
    default:
        break;
    }
}

static void bench_controller_publish(bench_controller_t *ctrl)
{
    ctrl->status.fc1_safe = ctrl->inputs.fc1_safe;
    ctrl->status.fc2_torque_reached = ctrl->inputs.fc2_torque_reached;
    ctrl->status.current_a = ctrl->inputs.current_a;
    ctrl->status.simulation_enabled = true;
    bench_core_publish_status(&ctrl->status);
}

static void bench_controller_init_context(bench_controller_t *ctrl, uint32_t now)
{
    bench_core_get_status(&ctrl->status);
    bench_core_get_config(&ctrl->config);

    ctrl->status.state = BENCH_STATE_INITIALIZING;
    ctrl->status.fault = BENCH_FAULT_NONE;
    ctrl->status.contactor_active = false;
    ctrl->status.ready_led_active = false;
    ctrl->status.test_led_active = false;
    ctrl->status.fault_led_active = false;
    ctrl->status.fc2_torque_reached = false;
    ctrl->status.pwm_percent = 0.0f;
    ctrl->status.elapsed_ms = 0;
    ctrl->status.current_a = 0.0f;
    ctrl->status.maximum_current_a = 0.0f;
    ctrl->status.simulation_enabled = true;
    ctrl->state_entered_ms = now;
    ctrl->test_start_ms = now;
    ctrl->last_ramp_ms = now;
    ctrl->settling = false;
    ctrl->cutoff_final_state = BENCH_STATE_STOPPED;
    ctrl->cutoff_fault = BENCH_FAULT_NONE;
}

static void bench_controller_task(void *argument)
{
    (void)argument;

    bench_controller_t *ctrl = &s_ctrl;
    bench_controller_init_context(ctrl, bench_controller_now_ms());

    for (;;) {
        const uint32_t now = bench_controller_now_ms();

        bench_simulator_get_inputs(&ctrl->inputs);
        bench_controller_check_fc1(ctrl, now);
        bench_controller_process_commands(ctrl, now);
        bench_controller_step(ctrl, now);
        bench_controller_publish(ctrl);

        vTaskDelay(pdMS_TO_TICKS(BENCH_CONTROLLER_PERIOD_MS));
    }
}

esp_err_t bench_controller_start(void)
{
    if (s_controller_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result = xTaskCreate(
        bench_controller_task,
        "bench_controller",
        BENCH_CONTROLLER_TASK_STACK_SIZE,
        NULL,
        BENCH_CONTROLLER_TASK_PRIORITY,
        &s_controller_task
    );

    if (result != pdPASS) {
        s_controller_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
