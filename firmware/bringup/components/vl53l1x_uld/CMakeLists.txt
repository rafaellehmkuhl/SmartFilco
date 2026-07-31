idf_component_register(
    SRCS "src/tof.c"
         "core/src/VL53L1X_api.c"
         "core/src/VL53L1X_calibration.c"
         "core/src/vl53l1_platform.c"
    INCLUDE_DIRS "include" "core/include"
    REQUIRES driver esp_common freertos log)
