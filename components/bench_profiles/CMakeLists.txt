idf_component_register(
    SRCS "bench_profiles.c" "bench_profiles_storage.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES bench_core nvs_flash
    PRIV_REQUIRES esp_rom log
)
