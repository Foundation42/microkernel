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
)

target_include_directories(microkernel PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(microkernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(microkernel PUBLIC Threads::Threads)
