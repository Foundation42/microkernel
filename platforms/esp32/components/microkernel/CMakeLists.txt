# Reference source files from the main repository via relative paths
set(MK_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../src")

idf_component_register(
    SRCS
        "${MK_SRC_DIR}/message.c"
        "${MK_SRC_DIR}/mailbox.c"
        "${MK_SRC_DIR}/actor.c"
        "${MK_SRC_DIR}/scheduler.c"
        "${MK_SRC_DIR}/runtime.c"
        "${MK_SRC_DIR}/wire.c"
        "${MK_SRC_DIR}/transport_tcp.c"
        "${MK_SRC_DIR}/mk_socket_tcp.c"
        "${MK_SRC_DIR}/name_registry.c"
        "${MK_SRC_DIR}/log_actor.c"
        "${MK_SRC_DIR}/http_conn.c"
        "${MK_SRC_DIR}/url_parse.c"
        "${MK_SRC_DIR}/sha1.c"
        "${MK_SRC_DIR}/base64.c"
        "${MK_SRC_DIR}/ws_frame.c"
        "${MK_SRC_DIR}/supervision.c"
        "${MK_SRC_DIR}/wasm_actor.c"
        "${MK_SRC_DIR}/ns_actor.c"
        "${MK_SRC_DIR}/node_identity.c"
        "${MK_SRC_DIR}/caps_actor.c"
        "${MK_SRC_DIR}/cf_proxy.c"
        "${MK_SRC_DIR}/local_kv.c"
        "${MK_SRC_DIR}/state_persist.c"
        "${MK_SRC_DIR}/gpio_actor.c"
        "${MK_SRC_DIR}/i2c_actor.c"
        "${MK_SRC_DIR}/pwm_actor.c"
        "${MK_SRC_DIR}/led_actor.c"
        "${MK_SRC_DIR}/display_actor.c"
        "${MK_SRC_DIR}/console_actor.c"
        "${MK_SRC_DIR}/text_render.c"
        "${MK_SRC_DIR}/dashboard_actor.c"
        "${MK_SRC_DIR}/shell_actor.c"
        "${MK_SRC_DIR}/mk_readline.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
    PRIV_INCLUDE_DIRS
        "${MK_SRC_DIR}"
    REQUIRES microkernel_hal wamr
    PRIV_REQUIRES esp_timer heap esp_netif soc
)

# ESP32 has ~280 KB heap — shrink embedded pools to fit
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    HTTP_READ_BUF_SIZE=4096
    MAX_HTTP_CONNS=4
    MAX_HTTP_LISTENERS=2
    MAX_TIMERS=8
    MAX_FD_WATCHES=8
    NAME_REGISTRY_SIZE=16
    MAX_SUPERVISOR_CHILDREN=8
    MAX_RESTART_HISTORY=16
    HAVE_MBEDTLS=1
    HAVE_WASM=1
)
