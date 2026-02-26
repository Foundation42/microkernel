# Select fiber implementation based on target architecture
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    set(FIBER_SRCS "fiber_xtensa.c" "fiber_xtensa.S")
    set(FIBER_REQUIRES xtensa)
elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
    set(FIBER_SRCS "fiber_riscv.c" "fiber_riscv.S")
    set(FIBER_REQUIRES "")
endif()

# Select display HAL based on board choice
if(CONFIG_MK_BOARD_LCD43B)
    set(DISPLAY_SRC "display_lcd43.c")
    set(DISPLAY_REQUIRES "")
else()
    set(DISPLAY_SRC "display_esp32.c")
    set(DISPLAY_REQUIRES "")
endif()

idf_component_register(
    SRCS
        "timer_esp32.c"
        "mk_socket_tls_esp.c"
        "gpio_esp32.c"
        "i2c_esp32.c"
        "pwm_esp32.c"
        "led_esp32.c"
        "midi_esp32.c"
        ${DISPLAY_SRC}
        ${FIBER_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../../src"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
    PRIV_REQUIRES esp_timer vfs esp-tls driver led_strip esp_lcd ${FIBER_REQUIRES}
)
