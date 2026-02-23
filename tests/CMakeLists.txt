function(add_microkernel_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE microkernel)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_microkernel_test(test_message)
add_microkernel_test(test_mailbox)
add_microkernel_test(test_scheduler)
add_microkernel_test(test_runtime)
add_microkernel_test(test_pingpong)
add_microkernel_test(test_wire)
add_microkernel_test(test_transport_unix)
add_microkernel_test(test_multinode)
add_microkernel_test(test_timer)
add_microkernel_test(test_fd_watcher)
add_microkernel_test(test_name_registry)
add_microkernel_test(test_log_actor)
add_microkernel_test(test_wire_net)
add_microkernel_test(test_transport_tcp)
add_microkernel_test(test_multinode_tcp)
add_microkernel_test(test_transport_udp)
add_microkernel_test(test_mk_socket)
add_microkernel_test(test_url_parse)
add_microkernel_test(test_http)
add_microkernel_test(test_sse)
add_microkernel_test(test_websocket)
add_microkernel_test(test_http_runtime)
add_microkernel_test(test_http_server)
add_microkernel_test(test_sse_server)
add_microkernel_test(test_ws_server)
add_microkernel_test(test_supervision)
add_microkernel_test(test_distributed_registry)
add_microkernel_test(test_ns_actor)
add_microkernel_test(test_ns_remote)
add_microkernel_test(test_caps_actor)
add_microkernel_test(test_cf_proxy)
add_microkernel_test(test_local_kv)
add_microkernel_test(test_gpio)
add_microkernel_test(test_i2c)
add_microkernel_test(test_pwm)
add_microkernel_test(test_led)
add_microkernel_test(test_display)
add_microkernel_test(test_display_text)
add_microkernel_test(test_dashboard)

# Phase 12: WASM actor test
if(HAVE_WASM)
    find_program(WASM_CC clang)
    if(WASM_CC)
        set(WASM_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        add_custom_command(
            OUTPUT ${WASM_OUT_DIR}/echo.wasm
            COMMAND ${WASM_CC} --target=wasm32-unknown-unknown -nostdlib -O2
                    -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined
                    -o ${WASM_OUT_DIR}/echo.wasm
                    ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/echo.c
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/echo.c
            COMMENT "Compiling echo.wasm")
        add_custom_command(
            OUTPUT ${WASM_OUT_DIR}/stateful.wasm
            COMMAND ${WASM_CC} --target=wasm32-unknown-unknown -nostdlib -O2
                    -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined
                    -o ${WASM_OUT_DIR}/stateful.wasm
                    ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/stateful.c
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/stateful.c
            COMMENT "Compiling stateful.wasm")
        add_custom_command(
            OUTPUT ${WASM_OUT_DIR}/counter_v1.wasm
            COMMAND ${WASM_CC} --target=wasm32-unknown-unknown -nostdlib -O2
                    -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined
                    -z stack-size=1024
                    -o ${WASM_OUT_DIR}/counter_v1.wasm
                    ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v1.c
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v1.c
            COMMENT "Compiling counter_v1.wasm (1KB aux stack)")
        add_custom_command(
            OUTPUT ${WASM_OUT_DIR}/counter_v2.wasm
            COMMAND ${WASM_CC} --target=wasm32-unknown-unknown -nostdlib -O2
                    -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined
                    -z stack-size=1024
                    -o ${WASM_OUT_DIR}/counter_v2.wasm
                    ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v2.c
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v2.c
            COMMENT "Compiling counter_v2.wasm (1KB aux stack)")
        find_program(WAT2WASM wat2wasm)
        if(WAT2WASM)
            add_custom_command(
                OUTPUT ${WASM_OUT_DIR}/counter_v1_lite.wasm
                COMMAND ${WAT2WASM}
                        -o ${WASM_OUT_DIR}/counter_v1_lite.wasm
                        ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v1_lite.wat
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v1_lite.wat
                COMMENT "Assembling counter_v1_lite.wasm (WAT, zero linear memory)")
            add_custom_command(
                OUTPUT ${WASM_OUT_DIR}/counter_v2_lite.wasm
                COMMAND ${WAT2WASM}
                        -o ${WASM_OUT_DIR}/counter_v2_lite.wasm
                        ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v2_lite.wat
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wasm_modules/counter_v2_lite.wat
                COMMENT "Assembling counter_v2_lite.wasm (WAT, zero linear memory)")
        else()
            message(STATUS "wat2wasm not found — counter_*_lite.wasm skipped")
        endif()
        add_custom_target(wasm_test_modules ALL DEPENDS
            ${WASM_OUT_DIR}/echo.wasm
            ${WASM_OUT_DIR}/stateful.wasm
            ${WASM_OUT_DIR}/counter_v1.wasm
            ${WASM_OUT_DIR}/counter_v2.wasm
            ${WASM_OUT_DIR}/counter_v1_lite.wasm
            ${WASM_OUT_DIR}/counter_v2_lite.wasm)

        add_microkernel_test(test_wasm_actor)
        add_dependencies(test_wasm_actor wasm_test_modules)
        target_compile_definitions(test_wasm_actor PRIVATE
            WASM_MODULE_DIR="${WASM_OUT_DIR}")

        add_microkernel_test(test_state_persist)
        add_dependencies(test_state_persist wasm_test_modules)
        target_compile_definitions(test_state_persist PRIVATE
            WASM_MODULE_DIR="${WASM_OUT_DIR}")

        add_microkernel_test(test_hot_reload)
        add_dependencies(test_hot_reload wasm_test_modules)
        target_compile_definitions(test_hot_reload PRIVATE
            WASM_MODULE_DIR="${WASM_OUT_DIR}")
    else()
        message(STATUS "WASM test: skipped (clang not found)")
    endif()
endif()

# ── Real-world tests (require network) ──────────────────────────────
option(BUILD_REALWORLD_TESTS "Build tests requiring network" OFF)
if(BUILD_REALWORLD_TESTS)
    function(add_realworld_test name)
        add_executable(${name} ${name}.c)
        target_link_libraries(${name} PRIVATE microkernel)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/src
        )
    endfunction()

    add_realworld_test(test_http_realworld)
    add_realworld_test(test_ws_realworld)
    add_realworld_test(test_https_realworld)
endif()

# ── Benchmarks ──────────────────────────────────────────────────────
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
if(BUILD_BENCHMARKS)
    function(add_benchmark name)
        add_executable(${name} ${name}.c)
        target_link_libraries(${name} PRIVATE microkernel)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/src
        )
    endfunction()

    add_benchmark(bench_http)
    add_benchmark(bench_actor)
endif()
