add_library(microkernel STATIC
    message.c
    mailbox.c
    actor.c
    scheduler.c
    runtime.c
    wire.c
    transport_unix.c
    transport_tcp.c
    transport_udp.c
    timer_linux.c
    name_registry.c
    log_actor.c
    mk_socket_tcp.c
    url_parse.c
    sha1.c
    base64.c
    ws_frame.c
    http_conn.c
    supervision.c
    ns_actor.c
    node_identity.c
    caps_actor.c
    cf_proxy.c
    local_kv.c
    state_persist.c
    gpio_actor.c
    gpio_hal_linux.c
    i2c_actor.c
    i2c_hal_linux.c
    pwm_actor.c
    pwm_hal_linux.c
    led_actor.c
    led_hal_linux.c
)

target_include_directories(microkernel PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(microkernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

option(CF_PROXY_DEBUG "Enable cf_proxy debug logging" OFF)
if(CF_PROXY_DEBUG)
    target_compile_definitions(microkernel PRIVATE CF_PROXY_DEBUG=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(microkernel PUBLIC Threads::Threads)

find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_sources(microkernel PRIVATE mk_socket_tls.c)
    target_link_libraries(microkernel PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(microkernel PUBLIC HAVE_OPENSSL=1)
endif()

# Phase 12: WASM actor support via WAMR
option(ENABLE_WASM "Enable WASM actor support via WAMR" ON)
if(ENABLE_WASM)
    # Auto-init submodule if directory exists but is empty
    if(EXISTS "${PROJECT_SOURCE_DIR}/.gitmodules"
       AND NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/wamr/CMakeLists.txt")
        execute_process(
            COMMAND git submodule update --init third_party/wamr
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    endif()

    if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/wamr/CMakeLists.txt")
        set(WAMR_ROOT_DIR "${PROJECT_SOURCE_DIR}/third_party/wamr")
        set(WAMR_BUILD_PLATFORM "linux")

        # Auto-detect target architecture
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set(WAMR_BUILD_TARGET "X86_64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(WAMR_BUILD_TARGET "AARCH64")
        else()
            set(WAMR_BUILD_TARGET "X86_64")
        endif()

        set(WAMR_BUILD_INTERP 1)
        set(WAMR_BUILD_AOT 1)
        set(WAMR_BUILD_FAST_INTERP 1)
        set(WAMR_BUILD_LIBC_BUILTIN 1)
        set(WAMR_BUILD_LIBC_WASI 0)
        include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

        add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
        target_compile_options(vmlib PRIVATE -w)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_compile_options(vmlib PRIVATE -fno-sanitize=address,undefined)
            target_link_options(vmlib PRIVATE -fno-sanitize=address,undefined)
        endif()

        target_sources(microkernel PRIVATE wasm_actor.c)
        target_link_libraries(microkernel PUBLIC vmlib m)
        target_compile_definitions(microkernel PUBLIC HAVE_WASM=1 HAVE_UCONTEXT=1)
        target_include_directories(microkernel PRIVATE
            ${WAMR_ROOT_DIR}/core/iwasm/include)
        set(HAVE_WASM TRUE CACHE INTERNAL "")
        message(STATUS "WASM actor support: enabled (WAMR, target=${WAMR_BUILD_TARGET})")
    else()
        set(HAVE_WASM FALSE CACHE INTERNAL "")
        message(STATUS "WASM actor support: disabled (WAMR not found)")
    endif()
else()
    set(HAVE_WASM FALSE CACHE INTERNAL "")
    message(STATUS "WASM actor support: disabled (ENABLE_WASM=OFF)")
endif()
