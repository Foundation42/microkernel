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
