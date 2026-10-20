#pragma once

#include <stdint.h>
#include "bench_types.h"

typedef struct {
    bench_command_type_t type;
    bench_command_source_t source;
    uint32_t timestamp_ms;
} bench_command_t;
