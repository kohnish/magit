set(lib_name libuv)
if(BUILD_SHARED_LIBS)
    pkg_check_modules(UV ${lib_name} REQUIRED)
    if("${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "MSYS")
        message("Msys has no proper pkgconfig for ${lib_name}")
    else()
        add_library(${lib_name} SHARED IMPORTED GLOBAL)
        set_target_properties(${lib_name} PROPERTIES IMPORTED_LOCATION ${pkgcfg_lib_UV_uv})
        if("${UV_INCLUDE_DIRS}" STREQUAL "")
            message("No include dir for ${lib_name}")
        else()
            set_target_properties(${lib_name} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${UV_INCLUDE_DIRS})
        endif()
    endif()
else()
    set(inst_prefix "${PROJECT_BINARY_DIR}/ext/${lib_name}")
    ExternalProject_Add(${lib_name}_build
        URL https://github.com/libuv/libuv/archive/refs/tags/v1.52.1.tar.gz
        CMAKE_ARGS ${COMPILE_OPT} -DCMAKE_INSTALL_PREFIX=${inst_prefix} -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=${inst_prefix}/lib -DLIBUV_BUILD_TESTS=OFF
        URL_HASH SHA256=478baf2599bfbc882c355288c9cb6f92e0e7dda435fa04031fa5b607cf3f414c
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
