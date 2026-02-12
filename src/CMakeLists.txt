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
)

target_include_directories(microkernel PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(microkernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(microkernel PUBLIC Threads::Threads)
