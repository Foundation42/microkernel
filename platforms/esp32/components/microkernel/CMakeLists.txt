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
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../../include"
    PRIV_INCLUDE_DIRS
        "${MK_SRC_DIR}"
    REQUIRES microkernel_hal
)
