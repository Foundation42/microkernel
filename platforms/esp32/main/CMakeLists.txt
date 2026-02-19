idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES microkernel microkernel_hal
    PRIV_REQUIRES vfs
)
