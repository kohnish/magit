set(lib_name libunity)
set(inst_prefix "${PROJECT_BINARY_DIR}/ext/${lib_name}")
ExternalProject_Add(${lib_name}_build
    URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.7.0.tar.gz
    CMAKE_ARGS ${COMPILE_OPT} -DCMAKE_INSTALL_PREFIX=${inst_prefix} -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=${inst_prefix}/lib
    URL_HASH SHA256=e84eb301ca7967831e68b1728f911e87fa2d345d8ddb64f897bc2f2ee24a321c
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
