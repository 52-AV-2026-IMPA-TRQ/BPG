#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "bench_profiles.h"
#include "bench_profiles_internal.h"
#include "bench_profiles_storage.h"

/*
 * Concurrency model:
 *  - s_profiles_lock (spinlock) only guards short RAM reads/writes: snapshots,
 *    getters and the final commit of a change. It is never held during NVS access.
 *  - s_operation_mutex serializes every mutation (init, set, clear, set_enabled,
 *    select) for the whole "persist to NVS, then update RAM" sequence, so RAM
 *    can only change inside that sequence and never diverges from NVS.
 *
 * RAM is updated only after NVS accepted the change. Each public operation
 * writes its slot and, when the selection is dropped, the selection in a single
 * NVS transaction (one handle, one commit), so a failure leaves both NVS and
 * RAM untouched. The RAM update (slot and selection) is one critical section.
 */
static portMUX_TYPE s_profiles_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_operation_mutex = NULL;
static StaticSemaphore_t s_operation_mutex_buffer;

/* Indexed by slot index (0..9), i.e. slot_number - 1. */
static bench_profile_t s_profiles[BENCH_PROFILE_SLOT_COUNT];
/* Scratch used only by init, under s_operation_mutex. */
static bench_profile_t s_loaded_profiles[BENCH_PROFILE_SLOT_COUNT];
static uint8_t s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
static bool s_initialized = false;

typedef struct {
    bench_profile_slot_state_t slot_state;
    uint32_t revision;
    uint8_t selected_slot_number;
} slot_base_t;

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

void bench_profiles_internal_make_empty(uint8_t slot_number, bench_profile_t *profile)
{
    *profile = make_empty_profile(slot_number);
}

static bool text_is_terminated(const char *text, size_t capacity)
{
    return memchr(text, '\0', capacity) != NULL;
}

static uint32_t next_revision(bench_profile_slot_state_t previous_state, uint32_t previous_revision)
{
    if (previous_state == BENCH_PROFILE_SLOT_EMPTY) {
        return 1U;
    }
    return (previous_revision == UINT32_MAX) ? UINT32_MAX : previous_revision + 1U;
}

static esp_err_t operation_begin(void)
{
    if (s_operation_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);
    if (!is_initialized()) {
        xSemaphoreGive(s_operation_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void operation_end(void)
{
    xSemaphoreGive(s_operation_mutex);
}

static slot_base_t take_base(size_t slot_index)
{
    slot_base_t base;

    taskENTER_CRITICAL(&s_profiles_lock);
    base.slot_state = s_profiles[slot_index].slot_state;
    base.revision = s_profiles[slot_index].revision;
    base.selected_slot_number = s_selected_slot_number;
    taskEXIT_CRITICAL(&s_profiles_lock);

    return base;
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
    /* Called once from app_main before concurrent use; the mutex is created here. */
    if (s_operation_mutex == NULL) {
        s_operation_mutex = xSemaphoreCreateMutexStatic(&s_operation_mutex_buffer);
        if (s_operation_mutex == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);

    taskENTER_CRITICAL(&s_profiles_lock);
    for (size_t slot_index = 0; slot_index < BENCH_PROFILE_SLOT_COUNT; slot_index++) {
        s_profiles[slot_index] = make_empty_profile((uint8_t)(slot_index + 1U));
    }
    s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
    s_initialized = false;
    taskEXIT_CRITICAL(&s_profiles_lock);

    uint8_t loaded_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
    esp_err_t result = bench_profiles_storage_init();
    if (result == ESP_OK) {
        result = bench_profiles_storage_load(s_loaded_profiles, &loaded_selected_slot_number);
    }

    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_profiles_lock);
        memcpy(s_profiles, s_loaded_profiles, sizeof(s_profiles));
        s_selected_slot_number = loaded_selected_slot_number;
        s_initialized = true;
        taskEXIT_CRITICAL(&s_profiles_lock);
    }

    xSemaphoreGive(s_operation_mutex);
    return result;
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

    esp_err_t result = operation_begin();
    if (result != ESP_OK) {
        return result;
    }

    const size_t slot_index = slot_index_from_number(slot_number);
    const slot_base_t base = take_base(slot_index);
    candidate.revision = next_revision(base.slot_state, base.revision);

    const bool drops_selection = base.selected_slot_number == slot_number &&
                                 candidate.slot_state != BENCH_PROFILE_SLOT_CONFIGURED;
    result = bench_profiles_storage_save_slot_and_selection(
        &candidate, drops_selection, BENCH_PROFILE_NO_SELECTION_SLOT);

    if (result == ESP_OK) {
        /* The mutex serializes mutations, so RAM still matches the snapshot. */
        taskENTER_CRITICAL(&s_profiles_lock);
        s_profiles[slot_index] = candidate;
        if (drops_selection) {
            s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
        }
        taskEXIT_CRITICAL(&s_profiles_lock);
    }

    operation_end();
    return result;
}

esp_err_t bench_profiles_clear(uint8_t slot_number)
{
    if (!slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    const bench_profile_t empty_profile = make_empty_profile(slot_number);

    esp_err_t result = operation_begin();
    if (result != ESP_OK) {
        return result;
    }

    const size_t slot_index = slot_index_from_number(slot_number);
    const slot_base_t base = take_base(slot_index);

    const bool drops_selection = base.selected_slot_number == slot_number;
    result = bench_profiles_storage_clear_slot_and_selection(
        slot_number, drops_selection, BENCH_PROFILE_NO_SELECTION_SLOT);

    if (result == ESP_OK) {
        taskENTER_CRITICAL(&s_profiles_lock);
        s_profiles[slot_index] = empty_profile;
        if (drops_selection) {
            s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
        }
        taskEXIT_CRITICAL(&s_profiles_lock);
    }

    operation_end();
    return result;
}

esp_err_t bench_profiles_set_enabled(uint8_t slot_number, bool enabled)
{
    if (!slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = operation_begin();
    if (result != ESP_OK) {
        return result;
    }

    const size_t slot_index = slot_index_from_number(slot_number);
    const bench_profile_slot_state_t target_state =
        enabled ? BENCH_PROFILE_SLOT_CONFIGURED : BENCH_PROFILE_SLOT_DISABLED;

    bench_profile_t updated;
    slot_base_t base;

    taskENTER_CRITICAL(&s_profiles_lock);
    updated = s_profiles[slot_index];
    base.slot_state = updated.slot_state;
    base.revision = updated.revision;
    base.selected_slot_number = s_selected_slot_number;
    taskEXIT_CRITICAL(&s_profiles_lock);

    if (base.slot_state == BENCH_PROFILE_SLOT_EMPTY) {
        result = ESP_ERR_INVALID_STATE;
    } else if (base.slot_state != target_state) {
        updated.slot_state = target_state;

        const bool drops_selection = !enabled && base.selected_slot_number == slot_number;
        result = bench_profiles_storage_save_slot_and_selection(
            &updated, drops_selection, BENCH_PROFILE_NO_SELECTION_SLOT);

        if (result == ESP_OK) {
            /* The mutex serializes mutations, so RAM still matches the snapshot. */
            taskENTER_CRITICAL(&s_profiles_lock);
            s_profiles[slot_index] = updated;
            if (drops_selection) {
                s_selected_slot_number = BENCH_PROFILE_NO_SELECTION_SLOT;
            }
            taskEXIT_CRITICAL(&s_profiles_lock);
        }
    }

    operation_end();
    return result;
}

esp_err_t bench_profiles_select(uint8_t slot_number)
{
    if (!slot_number_is_valid(slot_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = operation_begin();
    if (result != ESP_OK) {
        return result;
    }

    const slot_base_t base = take_base(slot_index_from_number(slot_number));

    if (base.slot_state != BENCH_PROFILE_SLOT_CONFIGURED) {
        result = ESP_ERR_INVALID_STATE;
    } else if (base.selected_slot_number != slot_number) {
        result = bench_profiles_storage_save_selection(slot_number);
        if (result == ESP_OK) {
            taskENTER_CRITICAL(&s_profiles_lock);
            s_selected_slot_number = slot_number;
            taskEXIT_CRITICAL(&s_profiles_lock);
        }
    }

    operation_end();
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
