# Path to echo.wasm (compiled from test module source)
set(WASM_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../tests/wasm_modules/echo.c")
set(WASM_OUT "${CMAKE_CURRENT_BINARY_DIR}/echo.wasm")

# WiFi components only available on chips with WiFi (S3, C6, etc. — not P4)
set(WIFI_REQUIRES "")
if(CONFIG_ESP_WIFI_ENABLED)
    set(WIFI_REQUIRES esp_wifi esp_netif nvs_flash esp_event)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES microkernel microkernel_hal
    PRIV_REQUIRES vfs ${WIFI_REQUIRES}
    EMBED_FILES ${WASM_OUT}
)

# Custom commands are not scriptable — only emit during real build phase
find_program(WASM_CC clang)
if(WASM_CC)
    add_custom_command(
        OUTPUT ${WASM_OUT}
        COMMAND ${WASM_CC} --target=wasm32-unknown-unknown -nostdlib -O2
                -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined
                -o ${WASM_OUT} ${WASM_SRC}
        DEPENDS ${WASM_SRC}
        COMMENT "Compiling echo.wasm for ESP32 embedding"
    )
    add_custom_target(echo_wasm ALL DEPENDS ${WASM_OUT})
    add_dependencies(${COMPONENT_LIB} echo_wasm)
endif()
