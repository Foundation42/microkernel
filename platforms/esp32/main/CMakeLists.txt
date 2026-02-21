# Path to echo.wasm (compiled from test module source)
set(WASM_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../tests/wasm_modules/echo.c")
set(WASM_OUT "${CMAKE_CURRENT_BINARY_DIR}/echo.wasm")

# Path to shell.wasm (compiled from Rust shell_wasm crate, small profile for ESP32)
set(SHELL_WASM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../tools/shell/shell_wasm")
set(SHELL_WASM_SRC "${SHELL_WASM_DIR}/src/lib.rs")
set(SHELL_WASM_TOML "${SHELL_WASM_DIR}/Cargo.toml")
set(SHELL_WASM_OUT "${CMAKE_CURRENT_BINARY_DIR}/shell.wasm")

# EMBED_FILES requires files to exist at configure time.
# Build them now if missing; add_custom_command handles incremental rebuilds.
find_program(WASM_CC clang)
if(WASM_CC AND NOT EXISTS ${WASM_OUT})
    message(STATUS "Building echo.wasm (first configure)...")
    execute_process(
        COMMAND ${WASM_CC} --target=wasm32-unknown-unknown -nostdlib -O2
                -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined
                -o ${WASM_OUT} ${WASM_SRC}
    )
endif()
# shell.wasm: copy from cargo target dir if pre-built, else build with cargo.
# The cargo target may already exist from a prior `cargo build` (Linux or manual).
find_program(CARGO cargo)
set(SHELL_WASM_CARGO_OUT "${SHELL_WASM_DIR}/target/wasm32-unknown-unknown/release/mk_shell.wasm")
if(NOT EXISTS ${SHELL_WASM_OUT} OR "${_shell_sz}" STREQUAL "0")
    # Check if output is missing or a stale 0-byte placeholder
    if(EXISTS ${SHELL_WASM_OUT})
        file(SIZE ${SHELL_WASM_OUT} _shell_sz)
    else()
        set(_shell_sz 0)
    endif()
    if(_shell_sz EQUAL 0)
        if(EXISTS ${SHELL_WASM_CARGO_OUT})
            message(STATUS "Copying pre-built shell.wasm from cargo target...")
            file(COPY_FILE ${SHELL_WASM_CARGO_OUT} ${SHELL_WASM_OUT})
        else()
            if(CARGO)
                message(STATUS "Building shell.wasm (first configure)...")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E env
                            "RUSTFLAGS=-C link-arg=-zstack-size=16384"
                            ${CARGO} build --target wasm32-unknown-unknown --release
                            --features small --manifest-path ${SHELL_WASM_TOML}
                    RESULT_VARIABLE _cargo_rc
                )
                if(_cargo_rc EQUAL 0 AND EXISTS ${SHELL_WASM_CARGO_OUT})
                    file(COPY_FILE ${SHELL_WASM_CARGO_OUT} ${SHELL_WASM_OUT})
                else()
                    message(WARNING "cargo build failed (rc=${_cargo_rc}). "
                            "Run manually: cd ${SHELL_WASM_DIR} && "
                            "RUSTFLAGS='-C link-arg=-zstack-size=16384' "
                            "cargo build --target wasm32-unknown-unknown --release --features small")
                endif()
            else()
                message(WARNING "cargo not found and no pre-built shell.wasm. "
                        "Run manually: cd ${SHELL_WASM_DIR} && "
                        "RUSTFLAGS='-C link-arg=-zstack-size=16384' "
                        "cargo build --target wasm32-unknown-unknown --release --features small")
            endif()
        endif()
    endif()
endif()

# Chips without WiFi radio — WiFi components don't exist for these targets
set(NO_WIFI_TARGETS esp32p4 esp32h2)
set(WIFI_REQUIRES "")
if(NOT IDF_TARGET IN_LIST NO_WIFI_TARGETS)
    set(WIFI_REQUIRES esp_wifi esp_netif nvs_flash esp_event)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES microkernel microkernel_hal
    PRIV_REQUIRES vfs driver spiffs ${WIFI_REQUIRES}
    EMBED_FILES ${WASM_OUT} ${SHELL_WASM_OUT}
)

# Incremental rebuilds when sources change
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

if(CARGO)
    add_custom_command(
        OUTPUT ${SHELL_WASM_OUT}
        COMMAND ${CMAKE_COMMAND} -E env
                "RUSTFLAGS=-C link-arg=-zstack-size=16384"
                ${CARGO} build --target wasm32-unknown-unknown --release
                --features small --manifest-path ${SHELL_WASM_TOML}
        COMMAND ${CMAKE_COMMAND} -E copy
                "${SHELL_WASM_DIR}/target/wasm32-unknown-unknown/release/mk_shell.wasm"
                ${SHELL_WASM_OUT}
        DEPENDS ${SHELL_WASM_SRC} ${SHELL_WASM_TOML}
        COMMENT "Building shell.wasm (Rust, small profile for ESP32)"
    )
    add_custom_target(shell_wasm ALL DEPENDS ${SHELL_WASM_OUT})
    add_dependencies(${COMPONENT_LIB} shell_wasm)
endif()
