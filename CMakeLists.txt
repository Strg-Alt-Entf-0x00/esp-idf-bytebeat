idf_component_register(
    SRCS "src/esp_bytebeat.c" 
         "src/bytebeat_vm.c"
    INCLUDE_DIRS "include"
    REQUIRES "freertos" "esp_common"
)
