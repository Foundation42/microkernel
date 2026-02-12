add_library(microkernel STATIC
    message.c
    mailbox.c
    actor.c
    scheduler.c
    runtime.c
)

target_include_directories(microkernel PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(microkernel PUBLIC Threads::Threads)
