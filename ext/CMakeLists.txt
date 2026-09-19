add_library(klib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/klib/kstring.c
)
target_include_directories(klib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/klib
)

add_subdirectory(msgpack_c)
add_subdirectory(libuv)
add_subdirectory(libgit2)
add_subdirectory(utf8proc)
add_subdirectory(openssl)

if(ENABLE_TESTS)
    add_subdirectory(unity)
endif()
