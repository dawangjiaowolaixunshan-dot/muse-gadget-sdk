idf_component_register(
    SRCS "vmess.c" "vproxy.c"
    INCLUDE_DIRS "."
    REQUIRES lwip mbedtls nvs_flash esp_timer
)

# Every TCP connection the firmware makes resolves and connects through these
# two; vproxy.c passes them through untouched unless a VMess node is set.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lwip_getaddrinfo"
    "-Wl,--wrap=lwip_connect"
    "-Wl,-u,__wrap_lwip_getaddrinfo"
    "-Wl,-u,__wrap_lwip_connect")
