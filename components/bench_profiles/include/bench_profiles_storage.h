#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "bench_profiles.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NVS persistence for generator profiles. Domain rules live in bench_profiles;
 * this module only serializes, stores and restores.
 *
 * Layout inside the NVS partition ("nvs" in production):
 *   namespace "bpg_profiles"
 *     "slot01".."slot10"  blob, one serialized profile per CONFIGURED/DISABLED slot
 *                         (EMPTY slots have no key)
 *     "selected"          u8, selected slot 1..10 (missing key = no selection)
 *     "schema"            u32, store schema version (currently 1)
 *
 * Blob format, schema 1 (all integers little-endian, floats IEEE-754 binary32,
 * no structure padding):
 *   off  size  field
 *     0     4  magic 0x42504750
 *     4     2  schema_version (1)
 *     6     2  payload_length = bytes between this prefix (8) and the CRC
 *     8     1  slot_number (1..10)
 *     9     1  slot_state (CONFIGURED=1, DISABLED=2; EMPTY is never stored)
 *    10     4  revision
 *    14     1  part_number length
 *    15     1  designation length
 *    16     1  description length
 *    17     4  pwm_frequency_hz
 *    21     1  pwm_resolution_bits
 *    22     4  initial_pwm_percent
 *    26     4  maximum_pwm_percent
 *    30     4  pwm_increment_percent
 *    34     4  ramp_period_ms
 *    38     4  contactor_settle_ms
 *    42     4  maximum_test_time_ms
 *    46     4  maximum_current_a
 *    50     n  part_number, designation, description bytes (no NUL, in that order)
 *   50+n    4  CRC32 (esp_rom_crc32_le, seed 0) of every preceding byte
 *
 * Thread safety: these functions do not serialize callers. bench_profiles calls
 * them under its operation mutex; other callers must do the same.
 */

#define BENCH_PROFILES_NVS_NAMESPACE "bpg_profiles"
#define BENCH_PROFILES_NVS_KEY_SELECTED "selected"
#define BENCH_PROFILES_NVS_KEY_SCHEMA "schema"
#define BENCH_PROFILES_STORAGE_SCHEMA_VERSION 1U

/*
 * Checks that the NVS partition is usable (nvs_flash_init already succeeded).
 * Does not write anything. Returns ESP_ERR_NVS_NOT_INITIALIZED otherwise.
 */
esp_err_t bench_profiles_storage_init(void);

/*
 * Loads all slots. profiles[] is fully overwritten (slot_number 1..10);
 * slots without data, or with corrupt/incompatible data, are EMPTY.
 * Corrupt slots are logged and skipped without touching other slots or NVS.
 * An unknown store schema loads nothing (all EMPTY, selection 0) and later
 * writes fail with ESP_ERR_INVALID_VERSION until bench_profiles_storage_erase_all().
 * An invalid persisted selection (out of range, corrupt, or not CONFIGURED)
 * yields selected_slot 0 and is repaired in NVS (best effort).
 * Returns ESP_OK in all of those cases; only real NVS access errors are returned.
 */
esp_err_t bench_profiles_storage_load(
    bench_profile_t profiles[BENCH_PROFILE_SLOT_COUNT],
    uint8_t *selected_slot
);

/*
 * Transactional writes: each function uses one NVS handle and exactly one
 * nvs_commit, so slot and selection change together or not at all.
 * If update_selection is false the selection key is left untouched;
 * otherwise selected_slot (0 removes it, 1..10 stores it) is applied.
 */
esp_err_t bench_profiles_storage_save_slot_and_selection(
    const bench_profile_t *profile,
    bool update_selection,
    uint8_t selected_slot
);

/* Removes the slot key (a missing key is not an error) and optionally updates the selection. */
esp_err_t bench_profiles_storage_clear_slot_and_selection(
    uint8_t slot_number,
    bool update_selection,
    uint8_t selected_slot
);

/* Persists a CONFIGURED or DISABLED profile (profile->slot_number selects the key). */
esp_err_t bench_profiles_storage_save_slot(const bench_profile_t *profile);

/* Removes the persisted slot (a missing key is not an error). */
esp_err_t bench_profiles_storage_clear_slot(uint8_t slot_number);

/* 0 removes the persisted selection; 1..10 stores it. */
esp_err_t bench_profiles_storage_save_selection(uint8_t selected_slot);

/*
 * Erases every key of the "bpg_profiles" namespace only (one nvs_erase_all on
 * that namespace's handle, one commit). A namespace that does not exist yet
 * returns ESP_OK. Other namespaces (e.g. Wi-Fi) and the partition itself are
 * untouched. Never called automatically. RAM state is not changed; call
 * bench_profiles_init() after.
 */
esp_err_t bench_profiles_storage_erase_all(void);

#ifdef __cplusplus
}
#endif
