#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "bench_profiles.h"

static portMUX_TYPE s_profiles_lock = portMUX_INITIALIZER_UNLOCKED;

/* Indexed by slot index (0..9), i.e. slot_number - 1. */
static bench_profile_t s_profiles[BENCH_PROFILE_SLOT_COUNT];
static uint8_t s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
static bool s_initialized = false;

static bool is_initialized(void)
{
    taskENTER_CRITICAL(&s_profiles_lock);
    const bool initialized = s_initialized;
    taskEXIT_CRITICAL(&s_profiles_lock);

    return initialized;
}

static bool slot_number_is_valid(uint8_t slot_number)
{
    return slot_number >= 1U && slot_number <= BENCH_PROFILE_SLOT_COUNT;
}

static size_t slot_index_from_number(uint8_t slot_number)
{
    return (size_t)slot_number - 1U;
}

static bench_config_t safe_config(void)
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

static bool config_is_safe_default(const bench_config_t *config)
{
    const bench_config_t reference = safe_config();

    return config->pwm_frequency_hz == reference.pwm_frequency_hz &&
           config->pwm_resolution_bits == reference.pwm_resolution_bits &&
           config->initial_pwm_percent == reference.initial_pwm_percent &&
           config->maximum_pwm_percent == reference.maximum_pwm_percent &&
           config->pwm_increment_percent == reference.pwm_increment_percent &&
           config->ramp_period_ms == reference.ramp_period_ms &&
           config->contactor_settle_ms == reference.contactor_settle_ms &&
           config->maximum_test_time_ms == reference.maximum_test_time_ms &&
           config->maximum_current_a == reference.maximum_current_a;
}

static bench_profile_t make_empty_profile(uint8_t slot_number)
{
    bench_profile_t profile;

    memset(&profile, 0, sizeof(profile));
    profile.slot_number = slot_number;
    profile.slot_state = BENCH_PROFILE_SLOT_EMPTY;
    profile.revision = 0U;
    profile.test_config = safe_config();

    return profile;
}

static bool text_is_terminated(const char *text, size_t capacity)
{
    return memchr(text, '\0', capacity) != NULL;
}

esp_err_t bench_profiles_validate(const bench_profile_t *profile)
{
    if (profile == NULL || !slot_number_is_valid(profile->slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (profile->slot_state != BENCH_PROFILE_SLOT_EMPTY &&
        profile->slot_state != BENCH_PROFILE_SLOT_CONFIGURED &&
        profile->slot_state != BENCH_PROFILE_SLOT_DISABLED) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!text_is_terminated(profile->part_number, sizeof(profile->part_number)) ||
        !text_is_terminated(profile->designation, sizeof(profile->designation)) ||
        !text_is_terminated(profile->description, sizeof(profile->description))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (profile->slot_state == BENCH_PROFILE_SLOT_EMPTY) {
        if (profile->part_number[0] != '\0' ||
            profile->designation[0] != '\0' ||
            profile->description[0] != '\0' ||
            profile->revision != 0U ||
            !config_is_safe_default(&profile->test_config)) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    if (profile->part_number[0] == '\0' || profile->designation[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    return bench_config_validate(&profile->test_config);
}

esp_err_t bench_profiles_init(void)
{
    taskENTER_CRITICAL(&s_profiles_lock);
    for (size_t slot_index = 0; slot_index < BENCH_PROFILE_SLOT_COUNT; slot_index++) {
        s_profiles[slot_index] = make_empty_profile((uint8_t)(slot_index + 1U));
    }
    s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
    s_initialized = true;
    taskEXIT_CRITICAL(&s_profiles_lock);

    return ESP_OK;
}

size_t bench_profiles_get_count(void)
{
    return BENCH_PROFILE_SLOT_COUNT;
}

esp_err_t bench_profiles_get(uint8_t slot_number, bench_profile_t *profile)
{
    if (profile == NULL || !slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_profiles_lock);
    if (s_initialized) {
        *profile = s_profiles[slot_index_from_number(slot_number)];
    } else {
        result = ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_profiles_lock);

    return result;
}

esp_err_t bench_profiles_set(uint8_t slot_number, const bench_profile_t *profile)
{
    if (profile == NULL || !slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* EMPTY is a clear: nothing else from the caller is validated or stored. */
    if (profile->slot_state == BENCH_PROFILE_SLOT_EMPTY) {
        return bench_profiles_clear(slot_number);
    }

    /* Private copy: slot_number and revision from the caller are not trusted. */
    bench_profile_t candidate = *profile;
    candidate.slot_number = slot_number;
    candidate.revision = 0U;

    const esp_err_t validation_result = bench_profiles_validate(&candidate);
    if (validation_result != ESP_OK) {
        return validation_result;
    }

    const size_t slot_index = slot_index_from_number(slot_number);
    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_profiles_lock);
    if (!s_initialized) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        const bench_profile_t *current = &s_profiles[slot_index];

        if (current->slot_state == BENCH_PROFILE_SLOT_EMPTY) {
            candidate.revision = 1U;
        } else if (current->revision == UINT32_MAX) {
            candidate.revision = UINT32_MAX;
        } else {
            candidate.revision = current->revision + 1U;
        }

        s_profiles[slot_index] = candidate;

        if (s_selected_slot_number == slot_number &&
            candidate.slot_state != BENCH_PROFILE_SLOT_CONFIGURED) {
            s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
        }
    }
    taskEXIT_CRITICAL(&s_profiles_lock);

    return result;
}

esp_err_t bench_profiles_clear(uint8_t slot_number)
{
    if (!slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    const bench_profile_t empty_profile = make_empty_profile(slot_number);
    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_profiles_lock);
    if (!s_initialized) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        s_profiles[slot_index_from_number(slot_number)] = empty_profile;
        if (s_selected_slot_number == slot_number) {
            s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
        }
    }
    taskEXIT_CRITICAL(&s_profiles_lock);

    return result;
}

esp_err_t bench_profiles_set_enabled(uint8_t slot_number, bool enabled)
{
    if (!slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_profiles_lock);
    if (!s_initialized) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        bench_profile_t *profile = &s_profiles[slot_index_from_number(slot_number)];

        if (profile->slot_state == BENCH_PROFILE_SLOT_EMPTY) {
            result = ESP_ERR_INVALID_STATE;
        } else if (enabled) {
            profile->slot_state = BENCH_PROFILE_SLOT_CONFIGURED;
        } else {
            profile->slot_state = BENCH_PROFILE_SLOT_DISABLED;
            if (s_selected_slot_number == slot_number) {
                s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
            }
        }
    }
    taskEXIT_CRITICAL(&s_profiles_lock);

    return result;
}

esp_err_t bench_profiles_select(uint8_t slot_number)
{
    if (!slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_profiles_lock);
    if (!s_initialized) {
        result = ESP_ERR_INVALID_STATE;
    } else if (s_profiles[slot_index_from_number(slot_number)].slot_state !=
               BENCH_PROFILE_SLOT_CONFIGURED) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        s_selected_slot_number = slot_number;
    }
    taskEXIT_CRITICAL(&s_profiles_lock);

    return result;
}

esp_err_t bench_profiles_get_selected(bench_profile_t *profile)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_profiles_lock);
    if (!s_initialized) {
        result = ESP_ERR_INVALID_STATE;
    } else if (s_selected_slot_number == BENCH_PROFILE_NO_SELECTION_SLOT) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        *profile = s_profiles[slot_index_from_number(s_selected_slot_number)];
    }
    taskEXIT_CRITICAL(&s_profiles_lock);

    return result;
}

uint8_t bench_profiles_get_selected_slot(void)
{
    taskENTER_CRITICAL(&s_profiles_lock);
    const uint8_t selected_slot_number =
        s_initialized ? s_selected_slot_number : BENCH_PROFILE_NO_SELECTION_SLOT;
    taskEXIT_CRITICAL(&s_profiles_lock);

    return selected_slot_number;
}

bool bench_profiles_has_selection(void)
{
    return bench_profiles_get_selected_slot() != BENCH_PROFILE_NO_SELECTION_SLOT;
}
