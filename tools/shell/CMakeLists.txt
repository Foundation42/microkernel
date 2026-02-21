# tools/shell/CMakeLists.txt — WASM interactive shell

find_program(CARGO cargo)
if(NOT CARGO)
    message(STATUS "mk-shell: skipped (cargo not found)")
    return()
endif()

# ── Build Rust WASM module via cargo ──────────────────────────────────

set(SHELL_WASM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/shell_wasm/src/lib.rs")
set(SHELL_WASM_TOML "${CMAKE_CURRENT_SOURCE_DIR}/shell_wasm/Cargo.toml")
set(SHELL_WASM_OUT
    "${CMAKE_CURRENT_SOURCE_DIR}/shell_wasm/target/wasm32-unknown-unknown/release/mk_shell.wasm")
set(SHELL_WASM_DEST "${CMAKE_CURRENT_BINARY_DIR}/shell.wasm")

add_custom_command(
    OUTPUT ${SHELL_WASM_OUT}
    COMMAND ${CARGO} build --target wasm32-unknown-unknown --release
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shell_wasm"
    DEPENDS ${SHELL_WASM_SRC} ${SHELL_WASM_TOML}
    COMMENT "Building shell WASM module (Rust)")

add_custom_command(
    OUTPUT ${SHELL_WASM_DEST}
    COMMAND ${CMAKE_COMMAND} -E copy ${SHELL_WASM_OUT} ${SHELL_WASM_DEST}
    DEPENDS ${SHELL_WASM_OUT}
    COMMENT "Copying shell.wasm to build directory")

add_custom_target(shell_wasm ALL DEPENDS ${SHELL_WASM_DEST})

# ── Build C driver ────────────────────────────────────────────────────

add_executable(mk-shell main.c)
target_link_libraries(mk-shell PRIVATE microkernel)
target_include_directories(mk-shell PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
add_dependencies(mk-shell shell_wasm)
