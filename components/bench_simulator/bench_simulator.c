#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "bench_core.h"
#include "bench_simulator.h"

#define BENCH_SIMULATOR_TASK_STACK_SIZE  3072
#define BENCH_SIMULATOR_TASK_PRIORITY       3
#define BENCH_SIMULATOR_PERIOD_MS          20

/* Simulated current model: linear in PWM, only while the contactor is active. */
#define BENCH_SIMULATOR_CURRENT_PER_PWM_PERCENT_A  0.1f
#define BENCH_SIMULATOR_CURRENT_LIMIT_A           10.0f

static portMUX_TYPE s_inputs_lock = portMUX_INITIALIZER_UNLOCKED;
static bench_simulator_inputs_t s_inputs = {
    .fc1_safe = true,
    .fc2_torque_reached = false,
    .current_a = 0.0f
};
static bool s_auto_current = true;

static TaskHandle_t s_simulator_task = NULL;
static volatile bool s_stop_requested = false;

static float bench_simulator_model_current(const bench_status_t *status)
{
    if (!status->contactor_active) {
        return 0.0f;
    }

    float current = status->pwm_percent * BENCH_SIMULATOR_CURRENT_PER_PWM_PERCENT_A;
    if (current > BENCH_SIMULATOR_CURRENT_LIMIT_A) {
        current = BENCH_SIMULATOR_CURRENT_LIMIT_A;
    }
    return current < 0.0f ? 0.0f : current;
}

/* Read-only use of the published status; the simulator never publishes. */
static void bench_simulator_update_current(void)
{
    bench_status_t status;
    bench_core_get_status(&status);
    const float current = bench_simulator_model_current(&status);

    taskENTER_CRITICAL(&s_inputs_lock);
    if (s_auto_current) {
        s_inputs.current_a = current;
    }
    taskEXIT_CRITICAL(&s_inputs_lock);
}

static void bench_simulator_task(void *argument)
{
    (void)argument;

    while (!s_stop_requested) {
        bench_simulator_update_current();
        vTaskDelay(pdMS_TO_TICKS(BENCH_SIMULATOR_PERIOD_MS));
    }

    s_simulator_task = NULL;
    s_stop_requested = false;

    vTaskDelete(NULL);
}

void bench_simulator_get_inputs(bench_simulator_inputs_t *inputs)
{
    if (inputs == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_inputs_lock);
    *inputs = s_inputs;
    taskEXIT_CRITICAL(&s_inputs_lock);
}

void bench_simulator_set_inputs(const bench_simulator_inputs_t *inputs)
{
    if (inputs == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_inputs_lock);
    s_inputs = *inputs;
    s_auto_current = false;
    taskEXIT_CRITICAL(&s_inputs_lock);
}

void bench_simulator_set_fc1_safe(bool fc1_safe)
{
    taskENTER_CRITICAL(&s_inputs_lock);
    s_inputs.fc1_safe = fc1_safe;
    taskEXIT_CRITICAL(&s_inputs_lock);
}

void bench_simulator_set_fc2_torque_reached(bool fc2_torque_reached)
{
    taskENTER_CRITICAL(&s_inputs_lock);
    s_inputs.fc2_torque_reached = fc2_torque_reached;
    taskEXIT_CRITICAL(&s_inputs_lock);
}

void bench_simulator_enable_auto_current(void)
{
    taskENTER_CRITICAL(&s_inputs_lock);
    s_auto_current = true;
    taskEXIT_CRITICAL(&s_inputs_lock);
}

esp_err_t bench_simulator_start(void)
{
    if (s_simulator_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = false;

    const BaseType_t result = xTaskCreate(
        bench_simulator_task,
        "bench_simulator",
        BENCH_SIMULATOR_TASK_STACK_SIZE,
        NULL,
        BENCH_SIMULATOR_TASK_PRIORITY,
        &s_simulator_task
    );

    if (result != pdPASS) {
        s_simulator_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t bench_simulator_stop(void)
{
    if (s_simulator_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = true;
    return ESP_OK;
}

bool bench_simulator_is_running(void)
{
    return s_simulator_task != NULL;
}
