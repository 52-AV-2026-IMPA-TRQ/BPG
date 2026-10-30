/*
 * Functional Unity tests for bench_profiles (run on the ESP32 target).
 *
 * Every profile used here is TEST DATA only. The part numbers, designations and
 * configuration values are arbitrary and say nothing about real generators.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "bench_core.h"
#include "bench_profiles.h"
#include "bench_profiles_storage.h"
#include "bench_profiles_storage_test_hooks.h"

#ifndef BENCH_PROFILES_TEST_HOOKS
#error "These tests require BENCH_PROFILES_TEST_HOOKS (isolated NVS test partition)."
#endif

#define TEST_TAG "[bench_profiles]"

/*
 * Isolated NVS: the tests never use the production "nvs" partition (Wi-Fi
 * credentials live there). They register a runtime partition "nvs_test" in
 * flash that the production partition table does not allocate (after "www",
 * which ends at 0x310000 on the 4 MB layout) and erase only that partition.
 * The range is checked against the flash size and every registered partition
 * before use.
 */
#define TEST_PARTITION_LABEL "nvs_test"
#define TEST_PARTITION_OFFSET 0x310000U
#define TEST_PARTITION_SIZE 0x6000U
#define TEST_OTHER_NAMESPACE "bpg_other"

static void ensure_test_partition_registered(void)
{
    static bool registered = false;
    if (registered) {
        return;
    }

    uint32_t flash_size = 0U;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, esp_flash_get_size(NULL, &flash_size));
    TEST_ASSERT_TRUE((uint64_t)TEST_PARTITION_OFFSET + TEST_PARTITION_SIZE <= flash_size);

    bool overlaps = false;
    esp_partition_iterator_t iterator =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition->flash_chip == esp_flash_default_chip &&
            partition->address < TEST_PARTITION_OFFSET + TEST_PARTITION_SIZE &&
            TEST_PARTITION_OFFSET < partition->address + partition->size) {
            overlaps = true;
        }
        iterator = esp_partition_next(iterator);
    }
    TEST_ASSERT_FALSE_MESSAGE(overlaps, "nvs_test range overlaps an existing partition");

    const esp_partition_t *test_partition = NULL;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK,
                            esp_partition_register_external(esp_flash_default_chip,
                                                            TEST_PARTITION_OFFSET,
                                                            TEST_PARTITION_SIZE,
                                                            TEST_PARTITION_LABEL,
                                                            ESP_PARTITION_TYPE_DATA,
                                                            ESP_PARTITION_SUBTYPE_DATA_NVS,
                                                            &test_partition));
    TEST_ASSERT_NOT_NULL(test_partition);
    registered = true;
}

/* Fresh, empty NVS test partition; storage redirected to it. */
static void prepare_test_nvs(void)
{
    ensure_test_partition_registered();

    (void)nvs_flash_deinit_partition(TEST_PARTITION_LABEL);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_flash_erase_partition(TEST_PARTITION_LABEL));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_flash_init_partition(TEST_PARTITION_LABEL));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK,
                            bench_profiles_storage_test_set_partition_label(TEST_PARTITION_LABEL));
}

/* Simulates a reboot: RAM is rebuilt from NVS, NVS is left as it is. */
static void reload_from_nvs(void)
{
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_init());
}

static void reset_components(void)
{
    prepare_test_nvs();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_core_init());
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_init());
}

static const char *slot_key(uint8_t slot_number)
{
    static const char *const keys[BENCH_PROFILE_SLOT_COUNT] = {
        "slot01", "slot02", "slot03", "slot04", "slot05",
        "slot06", "slot07", "slot08", "slot09", "slot10"
    };
    return keys[slot_number - 1U];
}

static nvs_handle_t raw_open(const char *name_space, nvs_open_mode_t mode)
{
    nvs_handle_t handle = 0;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK,
                            nvs_open_from_partition(TEST_PARTITION_LABEL, name_space, mode, &handle));
    return handle;
}

static size_t raw_read_slot(uint8_t slot_number, uint8_t *buffer, size_t capacity)
{
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READONLY);
    size_t length = capacity;
    const esp_err_t result = nvs_get_blob(handle, slot_key(slot_number), buffer, &length);
    nvs_close(handle);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, result);
    return length;
}

static void raw_write_slot(uint8_t slot_number, const uint8_t *buffer, size_t length)
{
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_blob(handle, slot_key(slot_number), buffer, length));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
}

static bool raw_key_exists(const char *name_space, const char *key)
{
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(TEST_PARTITION_LABEL, name_space, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    nvs_type_t type;
    const bool exists = nvs_find_key(handle, key, &type) == ESP_OK;
    nvs_close(handle);
    return exists;
}

static void raw_write_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_u8(handle, key, value));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
}

static void raw_write_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_u32(handle, key, value));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
}

static uint8_t raw_read_u8(const char *key)
{
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READONLY);
    uint8_t value = 0U;
    const esp_err_t result = nvs_get_u8(handle, key, &value);
    nvs_close(handle);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, result);
    return value;
}

/* Recomputes the trailing CRC32 so that only the intended field looks wrong. */
static void refresh_blob_crc(uint8_t *blob, size_t length)
{
    const uint32_t crc = esp_rom_crc32_le(0U, blob, (uint32_t)(length - 4U));
    blob[length - 4U] = (uint8_t)crc;
    blob[length - 3U] = (uint8_t)(crc >> 8);
    blob[length - 2U] = (uint8_t)(crc >> 16);
    blob[length - 1U] = (uint8_t)(crc >> 24);
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

    /* init reloads from NVS, so reset to empty needs the namespace erased first. */
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_storage_erase_all());
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

TEST_CASE("first boot without keys leaves ten empty slots and no selection", TEST_TAG)
{
    reset_components();

    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot01"));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));
    for (uint8_t slot_number = 1U; slot_number <= 10U; slot_number++) {
        assert_slot_is_empty(slot_number);
    }
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());

    reload_from_nvs();
    for (uint8_t slot_number = 1U; slot_number <= 10U; slot_number++) {
        assert_slot_is_empty(slot_number);
    }
}

TEST_CASE("saved profile survives RAM reinitialization but not an NVS erase", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    input.slot_number = 1U;
    input.revision = 1U;

    reload_from_nvs();
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(1U, &loaded));
    assert_profile_equal(&input, &loaded);
    assert_slot_is_empty(2U);

    /* Removing the raw key (an NVS erase) is what makes the slot empty again. */
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_erase_key(handle, slot_key(1U)));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    reload_from_nvs();
    assert_slot_is_empty(1U);
}

TEST_CASE("selection persists across reinitialization", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(4U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(4U));
    TEST_ASSERT_EQUAL_UINT8(4U, raw_read_u8(BENCH_PROFILES_NVS_KEY_SELECTED));

    reload_from_nvs();
    TEST_ASSERT_TRUE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_UINT8(4U, bench_profiles_get_selected_slot());

    bench_profile_t selected;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get_selected(&selected));
    TEST_ASSERT_EQUAL_STRING("TEST-PN-001", selected.part_number);
}

TEST_CASE("replacement persists the incremented revision", TEST_TAG)
{
    reset_components();

    bench_profile_t first = make_test_profile_a();
    bench_profile_t second = make_test_profile_b();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &first));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &second));

    reload_from_nvs();
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(2U, &loaded));
    second.slot_number = 2U;
    second.revision = 2U;
    assert_profile_equal(&second, &loaded);
}

TEST_CASE("disabled profile persists and is not selected", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(3U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(3U));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(3U, false));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));

    reload_from_nvs();
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(3U, &loaded));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_DISABLED, loaded.slot_state);
    TEST_ASSERT_EQUAL_UINT32(1U, loaded.revision);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-001", loaded.part_number);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_STATE, bench_profiles_select(3U));
}

TEST_CASE("clear removes the persisted profile", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(5U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(5U));
    TEST_ASSERT_TRUE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot05"));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_clear(5U));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot05"));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));

    reload_from_nvs();
    assert_slot_is_empty(5U);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
}

TEST_CASE("empty profile does not persist residual data", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(6U, &input));

    bench_profile_t residual = make_test_profile_b();
    residual.slot_state = BENCH_PROFILE_SLOT_EMPTY;
    residual.revision = 77U;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(6U, &residual));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot06"));

    reload_from_nvs();
    assert_slot_is_empty(6U);
}

TEST_CASE("corrupt slot loads as empty without affecting other slots", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    for (uint8_t slot_number = 1U; slot_number <= 5U; slot_number++) {
        TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(slot_number, &input));
    }

    /* Foreign namespace data (stands in for Wi-Fi credentials) must survive. */
    nvs_handle_t other = raw_open(TEST_OTHER_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_u32(other, "marker", 0xA5A5A5A5U));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(other));
    nvs_close(other);

    uint8_t blob[256];
    uint8_t longer[256];
    const size_t length = raw_read_slot(1U, blob, sizeof(blob));
    TEST_ASSERT_TRUE(length > 12U);

    /* slot01: not a blob at all. */
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_erase_key(handle, "slot01"));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_u32(handle, "slot01", 1U));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    /* slot02: truncated. */
    raw_write_slot(2U, blob, length - 1U);

    /* slot03: extended. */
    memcpy(longer, blob, length);
    longer[length] = 0U;
    raw_write_slot(3U, longer, length + 1U);

    /* slot04: slot01's blob under another key (wrong slot number). */
    raw_write_slot(4U, blob, length);

    reload_from_nvs();
    assert_slot_is_empty(1U);
    assert_slot_is_empty(2U);
    assert_slot_is_empty(3U);
    assert_slot_is_empty(4U);

    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(5U, &loaded));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_CONFIGURED, loaded.slot_state);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-001", loaded.part_number);

    TEST_ASSERT_TRUE(raw_key_exists(TEST_OTHER_NAMESPACE, "marker"));
    /* Corrupt entries are left in place, not erased. */
    TEST_ASSERT_TRUE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot04"));
}

TEST_CASE("corrupt selection is normalized to no selection", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &input));

    /* Out of range. */
    raw_write_u8(BENCH_PROFILES_NVS_KEY_SELECTED, 11U);
    reload_from_nvs();
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());

    /* Points to an EMPTY slot. */
    raw_write_u8(BENCH_PROFILES_NVS_KEY_SELECTED, 7U);
    reload_from_nvs();
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());

    /* Wrong NVS type. */
    nvs_handle_t handle = raw_open(BENCH_PROFILES_NVS_NAMESPACE, NVS_READWRITE);
    nvs_erase_key(handle, BENCH_PROFILES_NVS_KEY_SELECTED);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_u32(handle, BENCH_PROFILES_NVS_KEY_SELECTED, 2U));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(handle));
    nvs_close(handle);
    reload_from_nvs();
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());

    /* Points to a DISABLED slot. */
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(2U, false));
    raw_write_u8(BENCH_PROFILES_NVS_KEY_SELECTED, 2U);
    reload_from_nvs();
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());

    /* A stale selection must not resurface when the slot becomes usable. */
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(2U, true));
    reload_from_nvs();
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
}

TEST_CASE("unknown blob or store schema version is not loaded", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &input));

    /* Blob version 2 with a valid CRC: only the version is wrong. */
    uint8_t blob[256];
    const size_t length = raw_read_slot(1U, blob, sizeof(blob));
    blob[BENCH_PROFILES_BLOB_VERSION_OFFSET] = 2U;
    refresh_blob_crc(blob, length);
    raw_write_slot(1U, blob, length);

    reload_from_nvs();
    assert_slot_is_empty(1U);
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(2U, &loaded));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_CONFIGURED, loaded.slot_state);

    /* Unknown global schema: nothing is loaded and writes are refused. */
    raw_write_u32(BENCH_PROFILES_NVS_KEY_SCHEMA, BENCH_PROFILES_STORAGE_SCHEMA_VERSION + 1U);
    reload_from_nvs();
    assert_slot_is_empty(2U);
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_VERSION, bench_profiles_set(3U, &input));
    assert_slot_is_empty(3U);

    /* Explicit erase_all is the way out. */
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_storage_erase_all());
    reload_from_nvs();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(3U, &input));
}

TEST_CASE("incorrect CRC is rejected", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &input));

    uint8_t blob[256];
    size_t length = raw_read_slot(1U, blob, sizeof(blob));
    blob[length - 1U] ^= 0x01U;
    raw_write_slot(1U, blob, length);

    /* A changed text byte with the old CRC is rejected too. */
    length = raw_read_slot(2U, blob, sizeof(blob));
    blob[BENCH_PROFILES_BLOB_TEXT_OFFSET] ^= 0x01U;
    raw_write_slot(2U, blob, length);

    reload_from_nvs();
    assert_slot_is_empty(1U);
    assert_slot_is_empty(2U);
}

TEST_CASE("erase_all affects only the profiles namespace", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(1U));

    nvs_handle_t other = raw_open(TEST_OTHER_NAMESPACE, NVS_READWRITE);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_set_u32(other, "marker", 0x5A5A5A5AU));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, nvs_commit(other));
    nvs_close(other);

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_storage_erase_all());

    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot01"));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));
    TEST_ASSERT_TRUE(raw_key_exists(TEST_OTHER_NAMESPACE, "marker"));

    reload_from_nvs();
    assert_slot_is_empty(1U);
    TEST_ASSERT_FALSE(bench_profiles_has_selection());
}

TEST_CASE("validation failure changes neither RAM nor NVS", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(1U));

    uint8_t blob_before[256];
    uint8_t blob_after[256];
    const size_t length_before = raw_read_slot(1U, blob_before, sizeof(blob_before));
    bench_profile_t ram_before;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(1U, &ram_before));

    bench_profile_t invalid = make_test_profile_b();
    invalid.test_config.maximum_pwm_percent = 150.0f;
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(1U, &invalid));

    invalid = make_test_profile_b();
    invalid.part_number[0] = '\0';
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(1U, &invalid));

    invalid = make_test_profile_b();
    invalid.test_config.initial_pwm_percent = NAN;
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(1U, &invalid));

    const size_t length_after = raw_read_slot(1U, blob_after, sizeof(blob_after));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)length_before, (uint32_t)length_after);
    TEST_ASSERT_EQUAL_MEMORY(blob_before, blob_after, length_before);
    TEST_ASSERT_EQUAL_UINT8(1U, raw_read_u8(BENCH_PROFILES_NVS_KEY_SELECTED));

    bench_profile_t ram_after;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(1U, &ram_after));
    assert_profile_equal(&ram_before, &ram_after);
    TEST_ASSERT_EQUAL_UINT8(1U, bench_profiles_get_selected_slot());
}

TEST_CASE("zero maximum current persists", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    input.test_config = make_test_config(0.0f);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(9U, &input));

    reload_from_nvs();
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(9U, &loaded));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_CONFIGURED, loaded.slot_state);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, loaded.test_config.maximum_current_a);
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_config_validate(&loaded.test_config));
}

TEST_CASE("clearing selected slot removes profile and selection after reload", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(2U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(1U));

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_clear(1U));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot01"));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));

    reload_from_nvs();
    assert_slot_is_empty(1U);
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
    bench_profile_t other;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(2U, &other));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_CONFIGURED, other.slot_state);
}

TEST_CASE("disabling selected slot persists DISABLED and no selection", TEST_TAG)
{
    reset_components();

    bench_profile_t input = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(1U, &input));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(1U));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set_enabled(1U, false));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));

    reload_from_nvs();
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(1U, &loaded));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_DISABLED, loaded.slot_state);
    TEST_ASSERT_EQUAL_UINT32(1U, loaded.revision);
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
}

TEST_CASE("replacing selected slot with DISABLED persists both changes", TEST_TAG)
{
    reset_components();

    bench_profile_t first = make_test_profile_a();
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(3U, &first));
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_select(3U));

    bench_profile_t second = make_test_profile_b();
    second.slot_state = BENCH_PROFILE_SLOT_DISABLED;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_set(3U, &second));
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());

    reload_from_nvs();
    bench_profile_t loaded;
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_get(3U, &loaded));
    TEST_ASSERT_EQUAL(BENCH_PROFILE_SLOT_DISABLED, loaded.slot_state);
    TEST_ASSERT_EQUAL_UINT32(2U, loaded.revision);
    TEST_ASSERT_EQUAL_STRING("TEST-PN-002", loaded.part_number);
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));
}

TEST_CASE("erase_all succeeds when the namespace does not exist", TEST_TAG)
{
    reset_components();

    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_storage_erase_all());
    TEST_ASSERT_EQUAL_HEX32(ESP_OK, bench_profiles_storage_erase_all());

    nvs_handle_t handle = 0;
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_NVS_NOT_FOUND,
                            nvs_open_from_partition(TEST_PARTITION_LABEL, BENCH_PROFILES_NVS_NAMESPACE,
                                                    NVS_READONLY, &handle));
}

TEST_CASE("failed validation on an empty store creates no profile or selection", TEST_TAG)
{
    reset_components();

    bench_profile_t invalid = make_test_profile_a();
    invalid.test_config.maximum_current_a = -1.0f;
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_ARG, bench_profiles_set(1U, &invalid));

    assert_slot_is_empty(1U);
    TEST_ASSERT_EQUAL_UINT8(0U, bench_profiles_get_selected_slot());
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, "slot01"));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SELECTED));
    TEST_ASSERT_FALSE(raw_key_exists(BENCH_PROFILES_NVS_NAMESPACE, BENCH_PROFILES_NVS_KEY_SCHEMA));
}
