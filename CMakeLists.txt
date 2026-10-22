idf_component_register(
    SRCS "src/esp_bytebeat.c" 
         "src/bytebeat_vm.c"
         "src/bytebeat_vm_pie.c"
         "src/native_bytebeat_bytecode.c"
         "src/native_bytebeat_vm.c"
    INCLUDE_DIRS "include" "src"
    REQUIRES "freertos" "esp_common" "esp-dsp"
)
