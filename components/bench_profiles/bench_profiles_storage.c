#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "bench_profiles_internal.h"
#include "bench_profiles_storage.h"
#include "bench_profiles_storage_test_hooks.h"

static const char *TAG = "bench_profiles_nvs";

#define BLOB_MAGIC 0x42504750UL
#define BLOB_PREFIX_SIZE 8U     /* magic + schema_version + payload_length */
#define BLOB_FIXED_SIZE 50U     /* bytes before the texts */
#define BLOB_CRC_SIZE 4U
#define BLOB_MIN_SIZE (BLOB_FIXED_SIZE + 2U + BLOB_CRC_SIZE) /* 1-byte part number and designation */
#define BLOB_MAX_TEXT_SIZE ((BENCH_PROFILE_PART_NUMBER_MAX_LENGTH - 1U) + \
                            (BENCH_PROFILE_DESIGNATION_MAX_LENGTH - 1U) + \
                            (BENCH_PROFILE_DESCRIPTION_MAX_LENGTH - 1U))
#define BLOB_MAX_SIZE (BLOB_FIXED_SIZE + BLOB_MAX_TEXT_SIZE + BLOB_CRC_SIZE)

#define SLOT_KEY_SIZE 8U
#define PARTITION_LABEL_SIZE 17U /* NVS partition labels are at most 16 characters */

static_assert(sizeof(float) == 4U, "float must be IEEE-754 binary32");
static_assert(BENCH_PROFILE_SLOT_COUNT >= 1 && BENCH_PROFILE_SLOT_COUNT <= 99,
              "slot keys use two decimal digits");
static_assert(BLOB_MAX_SIZE <= 0xFFFFU, "payload_length must fit in 16 bits");
static_assert(BENCH_PROFILE_DESCRIPTION_MAX_LENGTH - 1 <= 0xFF, "text lengths must fit in 8 bits");

static char s_partition_label[PARTITION_LABEL_SIZE] = NVS_DEFAULT_PART_NAME;

/* ---- little-endian serialization helpers ---- */

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t position;
    bool overflow;
} blob_writer_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t position;
    bool underflow;
} blob_reader_t;

static void put_u8(blob_writer_t *writer, uint8_t value)
{
    if (writer->position + 1U > writer->capacity) {
        writer->overflow = true;
        return;
    }
    writer->data[writer->position++] = value;
}

static void put_u16(blob_writer_t *writer, uint16_t value)
{
    put_u8(writer, (uint8_t)(value & 0xFFU));
    put_u8(writer, (uint8_t)(value >> 8));
}

static void put_u32(blob_writer_t *writer, uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        put_u8(writer, (uint8_t)((value >> shift) & 0xFFU));
    }
}

static void put_f32(blob_writer_t *writer, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(writer, bits);
}

static void put_bytes(blob_writer_t *writer, const char *bytes, size_t length)
{
    if (writer->position + length > writer->capacity) {
        writer->overflow = true;
        return;
    }
    memcpy(&writer->data[writer->position], bytes, length);
    writer->position += length;
}

static uint8_t get_u8(blob_reader_t *reader)
{
    if (reader->position + 1U > reader->length) {
        reader->underflow = true;
        return 0U;
    }
    return reader->data[reader->position++];
}

static uint16_t get_u16(blob_reader_t *reader)
{
    const uint16_t low = get_u8(reader);
    const uint16_t high = get_u8(reader);
    return (uint16_t)(low | (high << 8));
}

static uint32_t get_u32(blob_reader_t *reader)
{
    uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= (uint32_t)get_u8(reader) << shift;
    }
    return value;
}

static float get_f32(blob_reader_t *reader)
{
    const uint32_t bits = get_u32(reader);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void get_bytes(blob_reader_t *reader, char *destination, size_t length)
{
    if (reader->position + length > reader->length) {
        reader->underflow = true;
        return;
    }
    memcpy(destination, &reader->data[reader->position], length);
    reader->position += length;
}

static uint32_t blob_crc(const uint8_t *data, size_t length)
{
    return esp_rom_crc32_le(0U, data, (uint32_t)length);
}

/* ---- profile <-> blob ---- */

static esp_err_t serialize_profile(const bench_profile_t *profile, uint8_t *buffer,
                                   size_t capacity, size_t *serialized_length)
{
    const size_t part_length = strnlen(profile->part_number, sizeof(profile->part_number));
    const size_t designation_length = strnlen(profile->designation, sizeof(profile->designation));
    const size_t description_length = strnlen(profile->description, sizeof(profile->description));
    const size_t total_length = BLOB_FIXED_SIZE + part_length + designation_length +
                                description_length + BLOB_CRC_SIZE;

    if (total_length > capacity || total_length > BLOB_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    blob_writer_t writer = { .data = buffer, .capacity = capacity };
    put_u32(&writer, (uint32_t)BLOB_MAGIC);
    put_u16(&writer, (uint16_t)BENCH_PROFILES_STORAGE_SCHEMA_VERSION);
    put_u16(&writer, (uint16_t)(total_length - BLOB_PREFIX_SIZE - BLOB_CRC_SIZE));
    put_u8(&writer, profile->slot_number);
    put_u8(&writer, (uint8_t)profile->slot_state);
    put_u32(&writer, profile->revision);
    put_u8(&writer, (uint8_t)part_length);
    put_u8(&writer, (uint8_t)designation_length);
    put_u8(&writer, (uint8_t)description_length);

    const bench_config_t *config = &profile->test_config;
    put_u32(&writer, config->pwm_frequency_hz);
    put_u8(&writer, config->pwm_resolution_bits);
    put_f32(&writer, config->initial_pwm_percent);
    put_f32(&writer, config->maximum_pwm_percent);
    put_f32(&writer, config->pwm_increment_percent);
    put_u32(&writer, config->ramp_period_ms);
    put_u32(&writer, config->contactor_settle_ms);
    put_u32(&writer, config->maximum_test_time_ms);
    put_f32(&writer, config->maximum_current_a);

    put_bytes(&writer, profile->part_number, part_length);
    put_bytes(&writer, profile->designation, designation_length);
    put_bytes(&writer, profile->description, description_length);

    if (writer.overflow || writer.position != total_length - BLOB_CRC_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    put_u32(&writer, blob_crc(buffer, writer.position));
    if (writer.overflow || writer.position != total_length) {
        return ESP_ERR_INVALID_SIZE;
    }

    *serialized_length = total_length;
    return ESP_OK;
}

/*
 * Every length is checked before any copy. Error codes:
 *   ESP_ERR_INVALID_SIZE    truncated, extended or inconsistent lengths
 *   ESP_ERR_INVALID_ARG     bad magic, slot mismatch, bad state/text/config
 *   ESP_ERR_INVALID_VERSION unknown schema version
 *   ESP_ERR_INVALID_CRC     CRC mismatch
 */
static esp_err_t deserialize_profile(uint8_t expected_slot_number, const uint8_t *buffer,
                                     size_t length, bench_profile_t *profile)
{
    if (length < BLOB_MIN_SIZE || length > BLOB_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    blob_reader_t reader = { .data = buffer, .length = length };
    if (get_u32(&reader) != (uint32_t)BLOB_MAGIC) {
        return ESP_ERR_INVALID_ARG;
    }
    if (get_u16(&reader) != BENCH_PROFILES_STORAGE_SCHEMA_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    const uint16_t payload_length = get_u16(&reader);
    if ((size_t)payload_length + BLOB_PREFIX_SIZE + BLOB_CRC_SIZE != length) {
        return ESP_ERR_INVALID_SIZE;
    }

    blob_reader_t crc_reader = { .data = buffer + (length - BLOB_CRC_SIZE), .length = BLOB_CRC_SIZE };
    if (blob_crc(buffer, length - BLOB_CRC_SIZE) != get_u32(&crc_reader)) {
        return ESP_ERR_INVALID_CRC;
    }

    bench_profile_t decoded;
    bench_profiles_internal_make_empty(expected_slot_number, &decoded);

    const uint8_t slot_number = get_u8(&reader);
    const uint8_t slot_state = get_u8(&reader);
    decoded.revision = get_u32(&reader);
    const uint8_t part_length = get_u8(&reader);
    const uint8_t designation_length = get_u8(&reader);
    const uint8_t description_length = get_u8(&reader);

    bench_config_t *config = &decoded.test_config;
    config->pwm_frequency_hz = get_u32(&reader);
    config->pwm_resolution_bits = get_u8(&reader);
    config->initial_pwm_percent = get_f32(&reader);
    config->maximum_pwm_percent = get_f32(&reader);
    config->pwm_increment_percent = get_f32(&reader);
    config->ramp_period_ms = get_u32(&reader);
    config->contactor_settle_ms = get_u32(&reader);
    config->maximum_test_time_ms = get_u32(&reader);
    config->maximum_current_a = get_f32(&reader);

    if (reader.underflow || reader.position != BLOB_FIXED_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (slot_number != expected_slot_number) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot_state != (uint8_t)BENCH_PROFILE_SLOT_CONFIGURED &&
        slot_state != (uint8_t)BENCH_PROFILE_SLOT_DISABLED) {
        return ESP_ERR_INVALID_ARG;
    }
    if (part_length >= BENCH_PROFILE_PART_NUMBER_MAX_LENGTH ||
        designation_length >= BENCH_PROFILE_DESIGNATION_MAX_LENGTH ||
        description_length >= BENCH_PROFILE_DESCRIPTION_MAX_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (BLOB_FIXED_SIZE + (size_t)part_length + designation_length + description_length +
            BLOB_CRC_SIZE != length) {
        return ESP_ERR_INVALID_SIZE;
    }

    decoded.slot_state = (bench_profile_slot_state_t)slot_state;
    get_bytes(&reader, decoded.part_number, part_length);
    get_bytes(&reader, decoded.designation, designation_length);
    get_bytes(&reader, decoded.description, description_length);
    if (reader.underflow || reader.position != length - BLOB_CRC_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Texts are stored without NUL; an embedded NUL would shorten them silently. */
    if (strlen(decoded.part_number) != part_length ||
        strlen(decoded.designation) != designation_length ||
        strlen(decoded.description) != description_length) {
        return ESP_ERR_INVALID_ARG;
    }

    if (bench_profiles_validate(&decoded) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    *profile = decoded;
    return ESP_OK;
}

/* ---- NVS access ---- */

static bool slot_number_is_valid(uint8_t slot_number)
{
    return slot_number >= 1U && slot_number <= BENCH_PROFILE_SLOT_COUNT;
}

static void format_slot_key(uint8_t slot_number, char key[SLOT_KEY_SIZE])
{
    snprintf(key, SLOT_KEY_SIZE, "slot%02u", (unsigned)slot_number);
}

static esp_err_t open_namespace(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open_from_partition(s_partition_label, BENCH_PROFILES_NVS_NAMESPACE, mode, handle);
}

/*
 * Opens read/write after checking the store schema. A missing schema key is
 * written (committed together with the caller's change); an unknown one is
 * never overwritten.
 */
static esp_err_t open_for_write(nvs_handle_t *handle)
{
    esp_err_t result = open_namespace(NVS_READWRITE, handle);
    if (result != ESP_OK) {
        return result;
    }

    uint32_t schema_version = 0U;
    result = nvs_get_u32(*handle, BENCH_PROFILES_NVS_KEY_SCHEMA, &schema_version);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = nvs_set_u32(*handle, BENCH_PROFILES_NVS_KEY_SCHEMA, BENCH_PROFILES_STORAGE_SCHEMA_VERSION);
    } else if (result == ESP_ERR_NVS_TYPE_MISMATCH ||
               (result == ESP_OK && schema_version != BENCH_PROFILES_STORAGE_SCHEMA_VERSION)) {
        result = ESP_ERR_INVALID_VERSION;
    }

    if (result != ESP_OK) {
        nvs_close(*handle);
    }
    return result;
}

static esp_err_t commit_and_close(nvs_handle_t handle, esp_err_t operation_result)
{
    esp_err_t result = operation_result;
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t bench_profiles_storage_init(void)
{
    nvs_handle_t handle;
    const esp_err_t result = open_namespace(NVS_READONLY, &handle);

    if (result == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }
    /* No namespace yet simply means nothing was ever saved. */
    return (result == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : result;
}

/* 0 erases the selection key; 1..10 stores it, replacing a key of the wrong type. */
static esp_err_t apply_selection(nvs_handle_t handle, uint8_t selected_slot)
{
    esp_err_t result;

    if (selected_slot == 0U) {
        result = nvs_erase_key(handle, BENCH_PROFILES_NVS_KEY_SELECTED);
        return (result == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : result;
    }

    result = nvs_set_u8(handle, BENCH_PROFILES_NVS_KEY_SELECTED, selected_slot);
    if (result == ESP_ERR_NVS_TYPE_MISMATCH) {
        result = nvs_erase_key(handle, BENCH_PROFILES_NVS_KEY_SELECTED);
        if (result == ESP_OK) {
            result = nvs_set_u8(handle, BENCH_PROFILES_NVS_KEY_SELECTED, selected_slot);
        }
    }
    return result;
}

esp_err_t bench_profiles_storage_save_slot_and_selection(const bench_profile_t *profile,
                                                         bool update_selection,
                                                         uint8_t selected_slot)
{
    if (profile == NULL || !slot_number_is_valid(profile->slot_number) ||
        (update_selection && selected_slot != 0U && !slot_number_is_valid(selected_slot)) ||
        profile->slot_state == BENCH_PROFILE_SLOT_EMPTY ||
        bench_profiles_validate(profile) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t blob[BLOB_MAX_SIZE];
    size_t blob_length = 0U;
    esp_err_t result = serialize_profile(profile, blob, sizeof(blob), &blob_length);
    if (result != ESP_OK) {
        return result;
    }

    char key[SLOT_KEY_SIZE];
    format_slot_key(profile->slot_number, key);

    nvs_handle_t handle;
    result = open_for_write(&handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_blob(handle, key, blob, blob_length);
    if (result == ESP_OK && update_selection) {
        result = apply_selection(handle, selected_slot);
    }
    return commit_and_close(handle, result);
}

esp_err_t bench_profiles_storage_save_slot(const bench_profile_t *profile)
{
    return bench_profiles_storage_save_slot_and_selection(profile, false, BENCH_PROFILE_NO_SELECTION_SLOT);
}

esp_err_t bench_profiles_storage_clear_slot_and_selection(uint8_t slot_number,
                                                          bool update_selection,
                                                          uint8_t selected_slot)
{
    if (!slot_number_is_valid(slot_number) ||
        (update_selection && selected_slot != 0U && !slot_number_is_valid(selected_slot))) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[SLOT_KEY_SIZE];
    format_slot_key(slot_number, key);

    nvs_handle_t handle;
    esp_err_t result = open_for_write(&handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_erase_key(handle, key);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK && update_selection) {
        result = apply_selection(handle, selected_slot);
    }
    return commit_and_close(handle, result);
}

esp_err_t bench_profiles_storage_clear_slot(uint8_t slot_number)
{
    return bench_profiles_storage_clear_slot_and_selection(slot_number, false,
                                                           BENCH_PROFILE_NO_SELECTION_SLOT);
}

esp_err_t bench_profiles_storage_save_selection(uint8_t selected_slot)
{
    if (selected_slot != 0U && !slot_number_is_valid(selected_slot)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = open_for_write(&handle);
    if (result != ESP_OK) {
        return result;
    }
    return commit_and_close(handle, apply_selection(handle, selected_slot));
}

esp_err_t bench_profiles_storage_erase_all(void)
{
    nvs_handle_t handle;
    /* Read-only open does not create the namespace: a missing one is already empty. */
    esp_err_t result = open_namespace(NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    nvs_close(handle);

    result = open_namespace(NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    /* nvs_erase_all only affects the namespace of this handle. */
    return commit_and_close(handle, nvs_erase_all(handle));
}

static void load_all_empty(bench_profile_t profiles[BENCH_PROFILE_SLOT_COUNT], uint8_t *selected_slot)
{
    for (size_t slot_index = 0U; slot_index < BENCH_PROFILE_SLOT_COUNT; slot_index++) {
        bench_profiles_internal_make_empty((uint8_t)(slot_index + 1U), &profiles[slot_index]);
    }
    *selected_slot = BENCH_PROFILE_NO_SELECTION_SLOT;
}

esp_err_t bench_profiles_storage_load(bench_profile_t profiles[BENCH_PROFILE_SLOT_COUNT],
                                      uint8_t *selected_slot)
{
    if (profiles == NULL || selected_slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    load_all_empty(profiles, selected_slot);

    nvs_handle_t handle;
    esp_err_t result = open_namespace(NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored profiles; all %d slots empty", BENCH_PROFILE_SLOT_COUNT);
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    uint32_t schema_version = BENCH_PROFILES_STORAGE_SCHEMA_VERSION;
    result = nvs_get_u32(handle, BENCH_PROFILES_NVS_KEY_SCHEMA, &schema_version);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
        schema_version = BENCH_PROFILES_STORAGE_SCHEMA_VERSION;
    } else if (result == ESP_ERR_NVS_TYPE_MISMATCH) {
        result = ESP_OK;
        schema_version = 0U;
    }
    if (result != ESP_OK) {
        nvs_close(handle);
        return result;
    }
    if (schema_version != BENCH_PROFILES_STORAGE_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "unsupported store schema %u; stored profiles ignored and left untouched",
                 (unsigned)schema_version);
        nvs_close(handle);
        return ESP_OK;
    }

    unsigned configured_count = 0U;
    unsigned disabled_count = 0U;
    unsigned ignored_count = 0U;

    for (uint8_t slot_number = 1U; slot_number <= BENCH_PROFILE_SLOT_COUNT; slot_number++) {
        char key[SLOT_KEY_SIZE];
        format_slot_key(slot_number, key);

        size_t stored_length = 0U;
        result = nvs_get_blob(handle, key, NULL, &stored_length);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (result == ESP_ERR_NVS_TYPE_MISMATCH) {
            ESP_LOGW(TAG, "slot %u: unexpected stored type; slot left empty", (unsigned)slot_number);
            ignored_count++;
            continue;
        }
        if (result != ESP_OK) {
            nvs_close(handle);
            return result;
        }
        if (stored_length < BLOB_MIN_SIZE || stored_length > BLOB_MAX_SIZE) {
            ESP_LOGW(TAG, "slot %u: invalid stored size %u; slot left empty",
                     (unsigned)slot_number, (unsigned)stored_length);
            ignored_count++;
            continue;
        }

        uint8_t blob[BLOB_MAX_SIZE];
        size_t read_length = stored_length;
        result = nvs_get_blob(handle, key, blob, &read_length);
        if (result != ESP_OK) {
            nvs_close(handle);
            return result;
        }

        bench_profile_t decoded;
        result = deserialize_profile(slot_number, blob, read_length, &decoded);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "slot %u: stored profile rejected (%s); slot left empty",
                     (unsigned)slot_number, esp_err_to_name(result));
            ignored_count++;
            continue;
        }

        profiles[slot_number - 1U] = decoded;
        if (decoded.slot_state == BENCH_PROFILE_SLOT_CONFIGURED) {
            configured_count++;
        } else {
            disabled_count++;
        }
    }

    bool repair_selection = false;
    uint8_t stored_selection = 0U;
    result = nvs_get_u8(handle, BENCH_PROFILES_NVS_KEY_SELECTED, &stored_selection);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    } else if (result == ESP_ERR_NVS_TYPE_MISMATCH) {
        ESP_LOGW(TAG, "stored selection has an unexpected type; selection cleared");
        repair_selection = true;
        result = ESP_OK;
    } else if (result == ESP_OK) {
        if (slot_number_is_valid(stored_selection) &&
            profiles[stored_selection - 1U].slot_state == BENCH_PROFILE_SLOT_CONFIGURED) {
            *selected_slot = stored_selection;
        } else {
            ESP_LOGW(TAG, "stored selection %u is not a configured slot; selection cleared",
                     (unsigned)stored_selection);
            repair_selection = true;
        }
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        return result;
    }

    /* A stale selection must not resurface if that slot is configured later. */
    if (repair_selection) {
        const esp_err_t repair_result = bench_profiles_storage_save_selection(0U);
        if (repair_result != ESP_OK) {
            ESP_LOGW(TAG, "could not clear stored selection (%s)", esp_err_to_name(repair_result));
        }
    }

    ESP_LOGI(TAG, "loaded %u configured, %u disabled, %u ignored; selected slot %u",
             configured_count, disabled_count, ignored_count, (unsigned)*selected_slot);
    return ESP_OK;
}

#ifdef BENCH_PROFILES_TEST_HOOKS
esp_err_t bench_profiles_storage_test_set_partition_label(const char *label)
{
    if (label == NULL) {
        strcpy(s_partition_label, NVS_DEFAULT_PART_NAME);
        return ESP_OK;
    }
    if (strlen(label) == 0U || strlen(label) >= sizeof(s_partition_label) ||
        strcmp(label, NVS_DEFAULT_PART_NAME) == 0) {
        /* Tests must never target the production partition explicitly. */
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(s_partition_label, label);
    return ESP_OK;
}
#endif
