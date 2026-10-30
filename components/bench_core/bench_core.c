#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

#include "bench_core.h"

static portMUX_TYPE s_bench_lock = portMUX_INITIALIZER_UNLOCKED;

static bench_status_t s_status;
static bench_config_t s_config;
static QueueHandle_t s_command_queue = NULL;

static bench_config_t bench_default_config(void)
{
    const bench_config_t config = {
        .pwm_frequency_hz = BENCH_DEFAULT_PWM_FREQUENCY_HZ,
        .pwm_resolution_bits = BENCH_DEFAULT_PWM_RESOLUTION_BITS,
        .initial_pwm_percent = BENCH_DEFAULT_INITIAL_PWM_PERCENT,
        .maximum_pwm_percent = BENCH_DEFAULT_MAXIMUM_PWM_PERCENT,
        .pwm_increment_percent = BENCH_DEFAULT_PWM_INCREMENT_PERCENT,
        .ramp_period_ms = BENCH_DEFAULT_RAMP_PERIOD_MS,
        .contactor_settle_ms = BENCH_DEFAULT_CONTACTOR_SETTLE_MS,
        .maximum_test_time_ms = BENCH_DEFAULT_MAX_TEST_TIME_MS,
        .maximum_current_a = BENCH_DEFAULT_MAXIMUM_CURRENT_A
    };

    return config;
}

esp_err_t bench_core_init(void)
{
    const bench_status_t initial_status = {
        .state = BENCH_STATE_INITIALIZING,
        .fault = BENCH_FAULT_NONE,
        .fc1_safe = false,
        .fc2_torque_reached = false,
        .contactor_active = false,
        .ready_led_active = false,
        .test_led_active = false,
        .fault_led_active = false,
        .pwm_percent = 0.0f,
        .current_a = 0.0f,
        .maximum_current_a = 0.0f,
        .elapsed_ms = 0,
        .sequence_number = 0,
        .simulation_enabled = false
    };

    taskENTER_CRITICAL(&s_bench_lock);
    s_status = initial_status;
    s_config = bench_default_config();
    taskEXIT_CRITICAL(&s_bench_lock);

    if (s_command_queue == NULL) {
        s_command_queue = xQueueCreate(BENCH_COMMAND_QUEUE_LENGTH, sizeof(bench_command_t));
        if (s_command_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xQueueReset(s_command_queue);
    }

    return ESP_OK;
}

esp_err_t bench_core_send_command(const bench_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xQueueSend(s_command_queue, command, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t bench_core_receive_command(bench_command_t *command, TickType_t timeout)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xQueueReceive(s_command_queue, command, timeout) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void bench_core_get_status(bench_status_t *status)
{
    if (status == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_bench_lock);
    *status = s_status;
    taskEXIT_CRITICAL(&s_bench_lock);
}

void bench_core_publish_status(const bench_status_t *status)
{
    if (status == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_bench_lock);
    const uint32_t next_sequence_number = s_status.sequence_number + 1U;
    s_status = *status;
    s_status.sequence_number = next_sequence_number;
    taskEXIT_CRITICAL(&s_bench_lock);
}

void bench_core_get_config(bench_config_t *config)
{
    if (config == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_bench_lock);
    *config = s_config;
    taskEXIT_CRITICAL(&s_bench_lock);
}

esp_err_t bench_config_validate(const bench_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->pwm_frequency_hz == 0U ||
        config->pwm_resolution_bits == 0U ||
        config->maximum_pwm_percent < 0.0f ||
        config->maximum_pwm_percent > 100.0f ||
        config->initial_pwm_percent < 0.0f ||
        config->initial_pwm_percent > config->maximum_pwm_percent ||
        config->pwm_increment_percent <= 0.0f ||
        config->ramp_period_ms == 0U ||
        config->maximum_test_time_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Written as a positive test so NaN is rejected; 0 disables the protection. */
    if (!isfinite(config->maximum_pwm_percent) ||
        !isfinite(config->initial_pwm_percent) ||
        !isfinite(config->pwm_increment_percent) ||
        !isfinite(config->maximum_current_a) ||
        !(config->maximum_current_a >= 0.0f)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t bench_core_set_config(const bench_config_t *config)
{
    const esp_err_t validation_result = bench_config_validate(config);
    if (validation_result != ESP_OK) {
        return validation_result;
    }

    taskENTER_CRITICAL(&s_bench_lock);
    s_config = *config;
    taskEXIT_CRITICAL(&s_bench_lock);

    return ESP_OK;
}

const char *bench_state_to_string(bench_state_t state)
{
    switch (state) {
    case BENCH_STATE_INITIALIZING:
        return "INITIALIZING";
    case BENCH_STATE_SELF_TEST:
        return "SELF_TEST";
    case BENCH_STATE_READY:
        return "READY";
    case BENCH_STATE_TESTING:
        return "TESTING";
    case BENCH_STATE_SAFE_CUTOFF:
        return "SAFE_CUTOFF";
    case BENCH_STATE_COMPLETED:
        return "COMPLETED";
    case BENCH_STATE_STOPPED:
        return "STOPPED";
    case BENCH_STATE_FAULT:
        return "FAULT";
    case BENCH_STATE_COOLDOWN:
        return "COOLDOWN";
    default:
        return "UNKNOWN";
    }
}

const char *bench_fault_to_string(bench_fault_t fault)
{
    switch (fault) {
    case BENCH_FAULT_NONE:
        return "NONE";
    case BENCH_FAULT_FC1_SECURITY:
        return "FC1_SECURITY";
    case BENCH_FAULT_TIMEOUT:
        return "TIMEOUT";
    case BENCH_FAULT_OVERCURRENT:
        return "OVERCURRENT";
    case BENCH_FAULT_SENSOR:
        return "SENSOR";
    case BENCH_FAULT_INTERNAL:
        return "INTERNAL";
    default:
        return "UNKNOWN";
    }
}
