idf_component_register(
    SRCS
        "timer_esp32.c"
        "mk_socket_tls_esp.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../../src"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
    PRIV_REQUIRES esp_timer vfs esp-tls
)
