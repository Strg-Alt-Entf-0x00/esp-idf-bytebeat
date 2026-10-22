idf_component_register(
    SRCS "main.cpp" "audio_manager.cpp" "bytebeat_engine.cpp" "serial_commands.cpp"
    INCLUDE_DIRS "."
    REQUIRES "esp-idf-bytebeat" "esp_codec_dev" "esp_timer" "esp_driver_i2c" "esp_driver_i2s" "esp_driver_gpio" "esp_ringbuf"
)
