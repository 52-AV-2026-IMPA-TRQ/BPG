#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "bench_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generator profile slots, held in RAM only (no persistence yet).
 *
 * Terminology:
 *  - slot_number: operator-facing position, 1..BENCH_PROFILE_SLOT_COUNT.
 *    Every public function uses it. 0 means "no slot" / "no selection".
 *  - slot index: internal array position, 0..BENCH_PROFILE_SLOT_COUNT-1
 *    (slot_number - 1). Never exposed by this API.
 */

#define BENCH_PROFILE_SLOT_COUNT 10
#define BENCH_PROFILE_PART_NUMBER_MAX_LENGTH 32
#define BENCH_PROFILE_DESIGNATION_MAX_LENGTH 48
#define BENCH_PROFILE_DESCRIPTION_MAX_LENGTH 96

#define BENCH_PROFILE_NO_SELECTION_SLOT 0U

typedef enum {
    BENCH_PROFILE_SLOT_EMPTY = 0,
    BENCH_PROFILE_SLOT_CONFIGURED,
    BENCH_PROFILE_SLOT_DISABLED
} bench_profile_slot_state_t;

typedef struct {
    uint8_t slot_number;
    bench_profile_slot_state_t slot_state;

    char part_number[BENCH_PROFILE_PART_NUMBER_MAX_LENGTH];
    char designation[BENCH_PROFILE_DESIGNATION_MAX_LENGTH];
    char description[BENCH_PROFILE_DESCRIPTION_MAX_LENGTH];

    uint32_t revision;

    bench_config_t test_config;
} bench_profile_t;

/*
 * Loads the profiles persisted in NVS (see bench_profiles_storage.h) into RAM.
 * Requires nvs_flash_init() to have completed. Starts from 10 EMPTY slots with
 * no selection and never selects anything automatically; missing, corrupt or
 * incompatible stored slots stay EMPTY (logged, NVS not modified). A normal
 * load does not write to NVS. Call it once from a single task before other
 * tasks use the API; calling it again reloads RAM from NVS (it does not erase
 * NVS). On failure the component stays uninitialized.
 *
 * Persistence: set, clear, set_enabled and select write to NVS first and
 * update RAM only if the write succeeded; on error RAM keeps its old content.
 * A slot change and the selection removal it implies are stored in a single
 * NVS commit, so they apply together or not at all. Mutations are serialized by an internal mutex
 * and may block while NVS is busy; do not call them from interrupt context.
 * All other stateful functions return ESP_ERR_INVALID_STATE before the first
 * successful init.
 */
esp_err_t bench_profiles_init(void);

size_t bench_profiles_get_count(void);

/* Copies slot content into *profile. ESP_ERR_INVALID_ARG on bad slot/NULL. */
esp_err_t bench_profiles_get(uint8_t slot_number, bench_profile_t *profile);

/*
 * Completely replaces a slot. Policy: profile->slot_number and
 * profile->revision are IGNORED and normalized: slot_number becomes the
 * destination slot and revision is managed here, never trusted from callers.
 * The rest of the profile is validated as in bench_profiles_validate().
 *
 * If profile->slot_state is EMPTY, this is exactly bench_profiles_clear():
 * every other field supplied by the caller (texts, revision, test_config) is
 * ignored, not validated and not stored. The slot is normalized to empty
 * texts, revision 0 and the default safe configuration.
 *
 * Returns ESP_ERR_INVALID_STATE before init (checked before validating the
 * profile contents).
 *
 * Revision: first non-EMPTY content in an EMPTY slot -> 1; replacing a
 * non-EMPTY slot -> previous + 1, saturating at UINT32_MAX.
 *
 * Selection is kept only if the slot was selected and the new profile is
 * CONFIGURED; otherwise it is removed.
 */
esp_err_t bench_profiles_set(uint8_t slot_number, const bench_profile_t *profile);

/* Resets the slot to EMPTY; removes the selection if it was this slot. */
esp_err_t bench_profiles_clear(uint8_t slot_number);

/*
 * true: DISABLED -> CONFIGURED; false: CONFIGURED -> DISABLED (removes the
 * selection if it was this slot). EMPTY slots return ESP_ERR_INVALID_STATE.
 * Content is unchanged, so revision is NOT incremented.
 */
esp_err_t bench_profiles_set_enabled(uint8_t slot_number, bool enabled);

/* Only CONFIGURED slots can be selected (else ESP_ERR_INVALID_STATE). */
esp_err_t bench_profiles_select(uint8_t slot_number);

/* Consistent snapshot of the selected profile; ESP_ERR_NOT_FOUND if none. */
esp_err_t bench_profiles_get_selected(bench_profile_t *profile);

/* Returns 0 when there is no selection, and also before bench_profiles_init(). */
uint8_t bench_profiles_get_selected_slot(void);

/* Returns false when there is no selection, and also before bench_profiles_init(). */
bool bench_profiles_has_selection(void);

/*
 * Pure validation (no shared state). Requires slot_number 1..10, a known
 * state, terminated strings, and:
 *  - CONFIGURED/DISABLED: non-empty part_number and designation and a config
 *    accepted by bench_config_validate();
 *  - EMPTY: empty texts, revision 0 and the default safe configuration.
 */
esp_err_t bench_profiles_validate(const bench_profile_t *profile);

#ifdef __cplusplus
}
#endif
