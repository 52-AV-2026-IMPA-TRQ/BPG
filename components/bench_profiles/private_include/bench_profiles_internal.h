#pragma once

#include <stdint.h>

#include "bench_profiles.h"

/* Component-private helpers shared by bench_profiles.c and bench_profiles_storage.c. */

/* Fills *profile with the canonical EMPTY profile for slot_number (1..10). */
void bench_profiles_internal_make_empty(uint8_t slot_number, bench_profile_t *profile);
