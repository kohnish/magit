configure_file(vim9_fuzzy_env.h.in ${PROJECT_BINARY_DIR}/include/vim9_fuzzy_env.h)

set(src
    magit_server.c
    msgpack_handler.c
    util.c
    git_handler.c
    # search_helper.c
    # timer.c
)

if(NOT WIN32)
    # list(APPEND src
    #     yank.c
    #     grep.c
    # )
endif()

set(lib
    klib
    libutf8proc
    libuv
    libgit2
    libmsgpack-c
    # ToDo: git2 deps, import to the project
    ssl
    crypto
    z
    pcre
)

if(WIN32)
    list(APPEND lib
                gdi32
                iphlpapi
                psapi
                rpcrt4
                userenv
                ws2_32
                wsock32
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND lib
        dl
        pthread
        )
endif()

set(inc
    ${PROJECT_BINARY_DIR}/include
    )

add_executable(${proj_name}
    ${src}
)

set(compile_opt
    # -Wall
    # -Wextra
    # -Werror
    -Wcast-align
    -Werror=vla
)

target_compile_options(${proj_name} PRIVATE
    ${compile_opt}
)

target_include_directories(${proj_name} SYSTEM PRIVATE
    ${inc}
)

target_link_libraries(${proj_name}
    ${lib}
)

install(TARGETS ${proj_name} RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/bin/)

if(ENABLE_TESTS)
    set(test_executable_name test-${proj_name})
    list(APPEND src
        test_magit_server.c
    )
    list(REMOVE_ITEM src
        magit_server.c
    )
    list(APPEND lib
        libunity
    )
    add_executable(${test_executable_name}
        ${src}
    )
    target_compile_options(${test_executable_name} PRIVATE
        ${compile_opt}
    )
#     target_include_directories(${test_executable_name} SYSTEM PRIVATE
#         ${inc}
#     )
    target_link_libraries(${test_executable_name}
        ${lib}
    )
endif()
