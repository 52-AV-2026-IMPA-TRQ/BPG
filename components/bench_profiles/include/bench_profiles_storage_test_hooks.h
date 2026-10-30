#pragma once

/*
 * TEST ONLY. These declarations exist only when the build defines
 * BENCH_PROFILES_TEST_HOOKS (the temporary Unity application does; the
 * production firmware never does), so production cannot redirect storage.
 */
#ifdef BENCH_PROFILES_TEST_HOOKS

#include "esp_err.h"

/* Byte offset of the little-endian u16 schema_version inside a profile blob. */
#define BENCH_PROFILES_BLOB_VERSION_OFFSET 4U
/* Offset of the first text byte; the blob ends with a 4-byte CRC32 after the texts. */
#define BENCH_PROFILES_BLOB_TEXT_OFFSET 50U

/*
 * Redirects storage to another, already initialized NVS partition label.
 * NULL restores the production label "nvs". Labels longer than 15 characters
 * and the production label "nvs" itself are rejected.
 */
esp_err_t bench_profiles_storage_test_set_partition_label(const char *label);

#endif
