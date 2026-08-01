set(lib_name "libutf8proc")
if(BUILD_SHARED_LIBS)
    pkg_check_modules(LIBUTF8PROC ${lib_name} REQUIRED)
    if("${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "MSYS")
        message("Msys has no proper pkgconfig for ${lib_name}")
    else()
        add_library(${lib_name} SHARED IMPORTED GLOBAL)
        set_target_properties(${lib_name} PROPERTIES IMPORTED_LOCATION ${pkgcfg_lib_LIBUTF8PROC_utf8proc})
        message("INFO: Found ${pkgcfg_lib_LIBUTF8PROC_utf8proc}")
        if("${MSGPACK_INCLUDE_DIRS}" STREQUAL "")
            message("No include dir for ${lib_name}")
        else()
            set_target_properties(${lib_name} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${LIBUTF8PROC_INCLUDE_DIRS})
        endif()
    endif()
else()
    set(inst_prefix "${PROJECT_BINARY_DIR}/ext/${lib_name}")
    ExternalProject_Add(${lib_name}_build
        URL https://github.com/JuliaStrings/utf8proc/releases/download/v2.11.3/utf8proc-2.11.3.tar.gz
        CMAKE_ARGS ${COMPILE_OPT} -DCMAKE_INSTALL_PREFIX=${inst_prefix} -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=${inst_prefix}/lib
        URL_HASH SHA256=415189fd2c85cd6ee5ff26af500fa387de9ada1e3e316e93f7338551481d557d
        BUILD_BYPRODUCTS ${inst_prefix}/lib/${lib_name}.a
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    add_library(${lib_name} STATIC IMPORTED GLOBAL)
    add_dependencies(${lib_name} ${lib_name}_build)
    file(MAKE_DIRECTORY ${inst_prefix}/include)
    set_target_properties(${lib_name} PROPERTIES
        IMPORTED_LOCATION ${inst_prefix}/lib/${lib_name}.a
        INTERFACE_INCLUDE_DIRECTORIES ${inst_prefix}/include
    )
endif()
