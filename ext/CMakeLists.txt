add_library(klib INTERFACE)
target_include_directories(klib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/klib
)

add_subdirectory(msgpack_c)
add_subdirectory(libuv)
add_subdirectory(libgit2)
add_subdirectory(utf8proc)

if(ENABLE_TESTS)
    add_subdirectory(unity)
endif()
