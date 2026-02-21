# Select fiber implementation based on target architecture
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    set(FIBER_SRCS "fiber_xtensa.c" "fiber_xtensa.S")
    set(FIBER_REQUIRES xtensa)
elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
    set(FIBER_SRCS "fiber_riscv.c" "fiber_riscv.S")
    set(FIBER_REQUIRES "")
endif()

idf_component_register(
    SRCS
        "timer_esp32.c"
        "mk_socket_tls_esp.c"
        ${FIBER_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../../src"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
    PRIV_REQUIRES esp_timer vfs esp-tls ${FIBER_REQUIRES}
)
