idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES microkernel microkernel_hal
    PRIV_REQUIRES vfs esp_wifi esp_netif nvs_flash esp_event
)
