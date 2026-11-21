cmake_minimum_required(VERSION 3.22)
project(pdispnative C)

# The protocol library is the same C code the PC side builds (../../../../../proto), used here through JNI.
set(PROTO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../proto)
add_library(pdispnative SHARED
    session.c
    ${PROTO}/proto.c ${PROTO}/fec.c ${PROTO}/video.c ${PROTO}/vrecv.c ${PROTO}/timesync.c ${PROTO}/net.c ${PROTO}/usbmux.c)
target_include_directories(pdispnative PRIVATE ${PROTO})
target_link_options(pdispnative PRIVATE -Wl,-z,max-page-size=16384)
target_compile_options(pdispnative PRIVATE -std=gnu11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE)
target_link_libraries(pdispnative android mediandk log)
find_package(Threads)
