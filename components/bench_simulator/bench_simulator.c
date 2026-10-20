#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bench_core.h"
#include "bench_simulator.h"

#define BENCH_SIMULATOR_TASK_STACK_SIZE  3072
#define BENCH_SIMULATOR_TASK_PRIORITY       3
#define BENCH_SIMULATOR_PERIOD_MS         100

static TaskHandle_t s_simulator_task = NULL;
static volatile bool s_stop_requested = false;

static void bench_simulator_task(void *argument)
{
    (void)argument;

    bench_status_t status = {
        .state = BENCH_STATE_READY,
        .fault = BENCH_FAULT_NONE,
        .fc1_safe = true,
        .fc2_torque_reached = false,
        .contactor_active = false,
        .ready_led_active = true,
        .test_led_active = false,
        .fault_led_active = false,
        .pwm_percent = 0.0f,
        .current_a = 0.0f,
        .maximum_current_a = 0.0f,
        .elapsed_ms = 0,
        .sequence_number = 0,
        .simulation_enabled = true
    };

    bench_core_publish_status(&status);

    while (!s_stop_requested) {
        bench_core_publish_status(&status);
        vTaskDelay(pdMS_TO_TICKS(BENCH_SIMULATOR_PERIOD_MS));
    }

    status.state = BENCH_STATE_STOPPED;
    status.contactor_active = false;
    status.ready_led_active = false;
    status.test_led_active = false;
    status.fault_led_active = false;
    status.pwm_percent = 0.0f;
    status.simulation_enabled = false;

    bench_core_publish_status(&status);

    s_simulator_task = NULL;
    s_stop_requested = false;

    vTaskDelete(NULL);
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
