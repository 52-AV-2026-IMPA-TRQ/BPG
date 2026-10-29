idf_component_register(
    SRCS "bench_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES bench_core
    PRIV_REQUIRES bench_simulator esp_timer
)
