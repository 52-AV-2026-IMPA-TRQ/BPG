/*
 * Functional Unity tests for bench_profiles (run on the ESP32 target).
 *
 * Every profile used here is TEST DATA only. The part numbers, designations and
 * configuration values are arbitrary and say nothing about real generators.
 */

#include <math.h>
#include <string.h>

#include "unity.h"

#include "bench_core.h"
#include "bench_profiles.h"

#define TEST_TAG "[bench_profiles]"

static void reset_components(void)
{
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_core_init());
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_init());
}

static bench_config_t make_test_config(float maximum_current_a)
{
    const bench_config_t config = {
        .pwm_frequency_hz = 1000U,
        .pwm_resolution_bits = 10U,
        .initial_pwm_percent = 5.0f,
        .maximum_pwm_percent = 80.0f,
        .pwm_increment_percent = 2.5f,
        .ramp_period_ms = 200U,
        .contactor_settle_ms = 300U,
        .maximum_test_time_ms = 7000U,
        .maximum_current_a = maximum_current_a
    };

    return config;
}

static bench_profile_t make_test_profile(uint8_t slot_number,
                                         const char *part_number,
                                         const char *designation,
                                         const char *description)
{
    bench_profile_t profile;

    memset(&profile, 0, sizeof(profile));
    profile.slot_number = slot_number;
    profile.slot_state = BENCH_PROFILE_SLOT_CONFIGURED;
    strncpy(profile.part_number, part_number, sizeof(profile.part_number) - 1U);
    strncpy(profile.designation, designation, sizeof(profile.designation) - 1U);
    strncpy(profile.description, description, sizeof(profile.description) - 1U);
    profile.revision = 0U;
    profile.test_config = make_test_config(12.5f);

    return profile;
}

static bench_profile_t make_test_profile_a(void)
{
    return make_test_profile(1U, "TEST-PN-001", "Perfil de prueba A", "Datos de prueba A");
}

static bench_profile_t make_test_profile_b(void)
{
    bench_profile_t profile =
        make_test_profile(1U, "TEST-PN-002", "Perfil de prueba B", "Datos de prueba B");
    profile.test_config = make_test_config(20.0f);
    profile.test_config.maximum_pwm_percent = 60.0f;
    return profile;
}

static void assert_config_equal(const bench_config_t *expected, const bench_config_t *actual)
{
    TEST_ASSERT_EQUAL_UINT32(expected->pwm_frequency_hz, actual->pwm_frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(expected->pwm_resolution_bits, actual->pwm_resolution_bits);
    TEST_ASSERT_EQUAL_FLOAT(expected->initial_pwm_percent, actual->initial_pwm_percent);
    TEST_ASSERT_EQUAL_FLOAT(expected->maximum_pwm_percent, actual->maximum_pwm_percent);
    TEST_ASSERT_EQUAL_FLOAT(expected->pwm_increment_percent, actual->pwm_increment_percent);
    TEST_ASSERT_EQUAL_UINT32(expected->ramp_period_ms, actual->ramp_period_ms);
    TEST_ASSERT_EQUAL_UINT32(expected->contactor_settle_ms, actual->contactor_settle_ms);
    TEST_ASSERT_EQUAL_UINT32(expected->maximum_test_time_ms, actual->maximum_test_time_ms);
    TEST_ASSERT_EQUAL_FLOAT(expected->maximum_current_a, actual->maximum_current_a);
}

static void assert_profile_equal(const bench_profile_t *expected, const bench_profile_t *actual)
{
    TEST_ASSERT_EQUAL_UINT8(expected->slot_number, actual->slot_number);
    TEST_ASSERT_EQUAL(expected->slot_state, actual->slot_state);
    TEST_ASSERT_EQUAL_STRING(expected->part_number, actual->part_number);
    TEST_ASSERT_EQUAL_STRING(expected->designation, actual->designation);
    TEST_ASSERT_EQUAL_STRING(expected->description, actual->description);
    TEST_ASSERT_EQUAL_UINT32(expected->revision, actual->revision);
    assert_config_equal(&expected->test_config, &actual->test_config);
}

static void assert_slot_is_empty(uint8_t slot_number)
{
    bench_profile_t profile;

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(slot_number, &profile));
    TEST_ASSERT_EQUAL_UINT8(slot_number, profile.slot_number);
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_EMPTY, profile.slot_state);
    TEST_ASSERT_EQUAL_UINT32(0U, profile.revision);
    TEST_ASSERT_EQUAL_STRING("", profile.part_number);
    TEST_ASSERT_EQUAL_STRING("", profile.designation);
    TEST_ASSERT_EQUAL_STRING("", profile.description);

    /* Safe default configuration, as defined by bench_config.h. */
    TEST_ASSERT_EQUAL_UINT32(BENCH_DEFAULT_PWM_FREQUENCY_HZ, profile.test_config.pwm_frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(BENCH_DEFAULT_PWM_RESOLUTION_BITS, profile.test_config.pwm_resolution_bits);
    TEST_ASSERT_EQUAL_FLOAT(BENCH_DEFAULT_INITIAL_PWM_PERCENT, profile.test_config.initial_pwm_percent);
    TEST_ASSERT_EQUAL_FLOAT(BENCH_DEFAULT_MAXIMUM_PWM_PERCENT, profile.test_config.maximum_pwm_percent);
    TEST_ASSERT_EQUAL_FLOAT(BENCH_DEFAULT_PWM_INCREMENT_PERCENT, profile.test_config.pwm_increment_percent);
    TEST_ASSERT_EQUAL_UINT32(BENCH_DEFAULT_RAMP_PERIOD_MS, profile.test_config.ramp_period_ms);
    TEST_ASSERT_EQUAL_UINT32(BENCH_DEFAULT_CONTACTOR_SETTLE_MS, profile.test_config.contactor_settle_ms);
    TEST_ASSERT_EQUAL_UINT32(BENCH_DEFAULT_MAX_TEST_TIME_MS, profile.test_config.maximum_test_time_ms);
    TEST_ASSERT_EQUAL_FLOAT(BENCH_DEFAULT_MAXIMUM_CURRENT_A, profile.test_config.maximum_current_a);
}

TEST_CASE("profiles initialize ten empty slots", TEST_TAG)
{
    reset_components();

    TEST_ASSERT_EQUAL_UINT32(10U, (uint32_t)bench_profiles_get_count());
    for (uint8_t slot_number = 1U; slot_number <= 10U; slot_number++) {
        assert_slot_is_empty(slot_number);
    }

    bench_profile_t profile;
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_NOT_FOUND, bench_profiles_get_selected(&profile));
}

TEST_CASE("profile slot boundaries are validated", TEST_TAG)
{
    reset_components();

    bench_profile_t profile = make_test_profile_a();
    const uint8_t invalid_slots[] = {0U, 11U};

    for (size_t i = 0; i < sizeof(invalid_slots); i++) {
        const uint8_t slot_number = invalid_slots[i];
        TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_get(slot_number, &profile));
        TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(slot_number, &profile));
        TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_clear(slot_number));
        TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_select(slot_number));
        TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set_enabled(slot_number, true));
    }

    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_get(1U, NULL));
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(1U, NULL));
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_get_selected(NULL));
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_validate(NULL));
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_config_validate(NULL));
    assert_slot_is_empty(1U);
}

TEST_CASE("configured profile can be stored and selected", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    input.slot_number = 7U;     /* must be normalized to the destination slot */
    input.revision = 99U;       /* must not be trusted */

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));

    bench_profile_t stored;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(1U, &stored));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_CONFIGURED, stored.slot_state);
    TEST_ASSERT_EQUAL_UINT8(1U, stored.slot_number);
    TEST_ASSERT_EQUAL_UINT32(1U, stored.revision);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-001", stored.part_number);
    TEST_ASSERT_EQUAL_STRING("Perfil de prueba A", stored.designation);
    TEST_ASSERT_EQUAL_STRING("Datos de prueba A", stored.description);
    assert_config_equal(&input.test_config, &stored.test_config);

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(1U));
    TEST_ASSERT_TRUE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(1U, bench_profiles_get_selected_slot());

    bench_profile_t selected;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get_selected(&selected));
    assert_profile_equal(&stored, &selected);
}

TEST_CASE("profile replacement increments revision", TEST_TAG)
{
    reset_components();

    bench_profile_t first = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &first));

    bench_profile_t stored;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(2U, &stored));
    TEST_ASSERT_EQUAL_UINT32(1U, stored.revision);

    bench_profile_t second = make_test_profile_b();
    second.description[0] = '\0';
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &second));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(2U, &stored));
    TEST_ASSERT_EQUAL_UINT32(2U, stored.revision);
    TEST_ASSERT_EQUAL_UINT8(2U, stored.slot_number);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-002", stored.part_number);
    TEST_ASSERT_EQUAL_STRING("Perfil de prueba B", stored.designation);
    TEST_ASSERT_EQUAL_STRING("", stored.description);
    assert_config_equal(&second.test_config, &stored.test_config);

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(2U));
    TEST_ASSERT_EQUAL_UINT8(2U, bench_profiles_get_selected_slot());
}

TEST_CASE("selected configured profile remains selected after valid replacement", TEST_TAG)
{
    reset_components();

    bench_profile_t first = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(3U, &first));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(3U));

    bench_profile_t second = make_test_profile_b();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(3U, &second));

    TEST_ASSERT_TRUE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(3U, bench_profiles_get_selected_slot());

    bench_profile_t selected;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get_selected(&selected));
    TEST_ASSERT_EQUAL_UINT8(3U, selected.slot_number);
    TEST_ASSERT_EQUAL_UINT32(2U, selected.revision);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-002", selected.part_number);
    TEST_ASSERT_EQUAL_STRING("Perfil de prueba B", selected.designation);
    assert_config_equal(&second.test_config, &selected.test_config);
}

TEST_CASE("disabling selected profile clears selection", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(4U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(4U));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(4U, false));

    bench_profile_t stored;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(4U, &stored));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_DISABLED, stored.slot_state);
    TEST_ASSERT_EQUAL_UINT32(1U, stored.revision);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_STATE, bench_profiles_select(4U));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(4U, true));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(4U, &stored));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_CONFIGURED, stored.slot_state);
    TEST_ASSERT_EQUAL_UINT32(1U, stored.revision);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
}

TEST_CASE("clearing selected profile normalizes slot", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(5U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(5U));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_clear(5U));

    assert_slot_is_empty(5U);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
}

TEST_CASE("set empty ignores residual caller content", TEST_TAG)
{
    reset_components();

    bench_profile_t residual = make_test_profile_a();
    residual.slot_state = BENCH_PROFILE_SLOT_EMPTY;
    residual.revision = 1234U;
    residual.test_config.pwm_frequency_hz = 0U;
    residual.test_config.maximum_pwm_percent = -5.0f;
    TEST_ASSERT_TRUE(bench_config_validate(&residual.test_config) != ESP_OK);

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(6U, &residual));
    assert_slot_is_empty(6U);

    /* Same result when replacing an occupied and selected slot. */
    bench_profile_t valid = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(6U, &valid));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(6U));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(6U, &residual));
    assert_slot_is_empty(6U);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
}

static void assert_rejected_atomically(const bench_profile_t *invalid_profile)
{
    bench_profile_t before;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(9U, &before));

    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(9U, invalid_profile));

    bench_profile_t after;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(9U, &after));
    assert_profile_equal(&before, &after);
    TEST_ASSERT_EQUAL_UINT32(1U, after.revision);
    TEST_ASSERT_EQUAL_UINT8(9U, bench_profiles_get_selected_slot());
}

TEST_CASE("invalid configured profiles are rejected atomically", TEST_TAG)
{
    reset_components();

    bench_profile_t original = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(9U, &original));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(9U));

    bench_profile_t invalid = make_test_profile_b();
    invalid.part_number[0] = '\0';
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    invalid.designation[0] = '\0';
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    memset(invalid.part_number, 'X', sizeof(invalid.part_number));
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    memset(invalid.designation, 'X', sizeof(invalid.designation));
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    memset(invalid.description, 'X', sizeof(invalid.description));
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    invalid.test_config.maximum_current_a = -1.0f;
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    invalid.test_config.maximum_pwm_percent = 100.5f;
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    invalid.test_config.pwm_increment_percent = NAN;
    assert_rejected_atomically(&invalid);

    invalid = make_test_profile_b();
    invalid.slot_state = (bench_profile_slot_state_t)99;
    assert_rejected_atomically(&invalid);
}

TEST_CASE("disabled profile content remains available", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(8U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(8U, false));

    bench_profile_t stored;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(8U, &stored));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_DISABLED, stored.slot_state);
    TEST_ASSERT_EQUAL_UINT8(8U, stored.slot_number);
    TEST_ASSERT_EQUAL_UINT32(1U, stored.revision);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-001", stored.part_number);
    TEST_ASSERT_EQUAL_STRING("Perfil de prueba A", stored.designation);
    TEST_ASSERT_EQUAL_STRING("Datos de prueba A", stored.description);
    assert_config_equal(&input.test_config, &stored.test_config);

    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_STATE, bench_profiles_select(8U));
    TEST_ASSERT_FALSE(bench_profiles_has_selection());

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(8U, true));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(8U));
    TEST_ASSERT_EQUAL_UINT8(8U, bench_profiles_get_selected_slot());

    /* EMPTY slots cannot be enabled or disabled. */
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_STATE, bench_profiles_set_enabled(10U, true));
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_STATE, bench_profiles_select(10U));
}

TEST_CASE("initialization resets profiles and selection", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(10U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(10U));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_init());

    for (uint8_t slot_number = 1U; slot_number <= 10U; slot_number++) {
        assert_slot_is_empty(slot_number);
    }
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
}

TEST_CASE("configuration validation accepts disabled overcurrent protection", TEST_TAG)
{
    reset_components();

    const bench_config_t config = make_test_config(0.0f);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_config_validate(&config));

    bench_profile_t input = make_test_profile_a();
    input.test_config = config;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));

    bench_profile_t stored;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(1U, &stored));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, stored.test_config.maximum_current_a);
}
