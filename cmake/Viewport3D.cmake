# Fetch and build bgfx for the experimental 3D viewport (src/viewport3d).
#
# bgfx is built from source through bgfx.cmake, which in turn fetches bx, bimg
# and bgfx at pinned commits. Only the libraries are built; the shaders are
# shipped precompiled in src/viewport3d/shaders, so no shader compiler is needed.
#
# To use a local checkout instead of downloading, pass:
#   -DFETCHCONTENT_SOURCE_DIR_BGFX_CMAKE=/path/to/bgfx.cmake
# and optionally -DBX_DIR=, -DBIMG_DIR=, -DBGFX_DIR= for the three libraries.

include(FetchContent)

set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
set(BGFX_WITH_WAYLAND OFF CACHE BOOL "" FORCE)
set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
set(BGFX_AMALGAMATED ON CACHE BOOL "" FORCE)
set(BX_AMALGAMATED ON CACHE BOOL "" FORCE)
set(BGFX_LIBRARY_TYPE "STATIC" CACHE STRING "" FORCE)

FetchContent_Declare(bgfx_cmake
    GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
    GIT_TAG 82f885c710b98a4b4f4df40cc234a3cb702f4846 # v1.168.9637-616
)

# bgfx is third-party code: build it without OpenTTD's warning flags, but with
# the same MSVC runtime library as the rest of the game.
get_directory_property(_viewport3d_saved_compile_options COMPILE_OPTIONS)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
if(MSVC)
    if(VCPKG_TARGET_TRIPLET MATCHES "-static" AND NOT VCPKG_TARGET_TRIPLET MATCHES "-md")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_compile_options(/MP)
endif()

FetchContent_MakeAvailable(bgfx_cmake)

set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_viewport3d_saved_compile_options}")
unset(CMAKE_MSVC_RUNTIME_LIBRARY)

target_link_libraries(openttd_lib bgfx bx)
target_compile_definitions(openttd_lib PUBLIC WITH_VIEWPORT3D)
message(STATUS "Viewport3D enabled -- -DWITH_VIEWPORT3D -- bgfx")
