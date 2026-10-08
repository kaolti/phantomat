cmake_minimum_required(VERSION 3.27)

project(spatialoverview
    DESCRIPTION "spatial overview plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

# The plugin is the top-level sources only (scripts/ and tests/ hold separate tools).
file(GLOB SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(spatialoverview SHARED ${SRC})

set(VERSION_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VERSION_HEADER "${VERSION_HEADER_DIR}/PluginVersion.hpp")
add_custom_target(spatialoverview-version ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${VERSION_HEADER_DIR}"
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-plugin-version.sh" "${VERSION_HEADER}" "${CMAKE_CURRENT_SOURCE_DIR}"
    BYPRODUCTS "${VERSION_HEADER}"
    VERBATIM
)
add_dependencies(spatialoverview spatialoverview-version)
target_include_directories(spatialoverview PRIVATE "${VERSION_HEADER_DIR}")

find_package(PkgConfig REQUIRED)
pkg_search_module(lua REQUIRED IMPORTED_TARGET "lua5.4>=5.4" "lua>=5.4")
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    hyprgraphics
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(spatialoverview PRIVATE rt PkgConfig::deps PkgConfig::lua)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(spatialoverview PRIVATE -fno-gnu-unique)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(DOBBY_INCLUDE "" CACHE PATH "Directory containing dobby.h")
    set(DOBBY_LIBRARY "" CACHE FILEPATH "Path to the PIC libdobby.a archive")
    if(NOT EXISTS "${DOBBY_INCLUDE}/dobby.h" OR NOT EXISTS "${DOBBY_LIBRARY}" OR NOT DOBBY_LIBRARY MATCHES "\\.a$")
        message(FATAL_ERROR "ARM64 builds require DOBBY_INCLUDE and DOBBY_LIBRARY pointing to dobby.h and a PIC libdobby.a archive. See docs/arm64.md.")
    endif()
    add_library(phantomat-dobby STATIC IMPORTED)
    set_target_properties(phantomat-dobby PROPERTIES
        IMPORTED_LOCATION "${DOBBY_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DOBBY_INCLUDE}"
    )
    target_link_libraries(spatialoverview PRIVATE phantomat-dobby)
    target_link_options(spatialoverview PRIVATE "LINKER:--exclude-libs,ALL")
endif()

install(TARGETS spatialoverview)
