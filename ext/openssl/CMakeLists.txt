set(lib_name "openssl")
if(BUILD_SHARED_LIBS)
    pkg_check_modules(OPENSSL ${lib_name} REQUIRED)
    if("${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "MSYS")
        message("Msys has no proper pkgconfig for ${lib_name}")
    else()
        add_library(ssl SHARED IMPORTED GLOBAL)
        add_library(crypto SHARED IMPORTED GLOBAL)
        set_target_properties(ssl PROPERTIES IMPORTED_LOCATION ${pkgcfg_lib_OPENSSL_ssl})
        set_target_properties(crypto PROPERTIES IMPORTED_LOCATION ${pkgcfg_lib_OPENSSL_crypto})
        if("${OPENSSL_INCLUDE_DIRS}" STREQUAL "")
            message("No include dir for ${lib_name}")
        else()
            set_target_properties(ssl PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIRS})
        endif()
    endif()
else()
    set(inst_prefix "${PROJECT_BINARY_DIR}/ext/${lib_name}")
    ExternalProject_Add(${lib_name}_build
        URL https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz
        BUILD_IN_SOURCE 1
        CONFIGURE_COMMAND ./config --prefix=${inst_prefix} --libdir=lib --openssldir=${inst_prefix} no-shared no-weak-ssl-ciphers
        BUILD_COMMAND make -j8
        INSTALL_COMMAND make install_sw
        URL_HASH SHA256=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
        BUILD_BYPRODUCTS "${inst_prefix}/lib/libssl.a" "${inst_prefix}/lib/libcrypto.a"
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )

    make_directory(${inst_prefix}/include)
    add_library(ssl STATIC IMPORTED GLOBAL)
    set_target_properties(ssl PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${inst_prefix}
        IMPORTED_LOCATION ${inst_prefix}/lib/libssl.a
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${inst_prefix}/include
        INTERFACE_INCLUDE_DIRECTORIES ${inst_prefix}/include
    )
    add_dependencies(ssl openssl_build)
    if(APPLE)
        target_link_libraries(ssl INTERFACE "-framework Security")
        target_link_libraries(ssl INTERFACE "-framework CoreFoundation")
        target_link_libraries(ssl INTERFACE "iconv")
    endif()

    add_library(crypto STATIC IMPORTED GLOBAL)
    set_target_properties(crypto PROPERTIES
        IMPORTED_LOCATION ${inst_prefix}/lib/libcrypto.a
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${inst_prefix}/include
        INTERFACE_INCLUDE_DIRECTORIES ${inst_prefix}/include
        )
    add_dependencies(crypto openssl_build)
endif()
