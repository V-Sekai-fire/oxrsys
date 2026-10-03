# SPDX-License-Identifier: MPL-2.0
#
# oxrsys_pyrowave: PyroWave's C API (MIT), linked statically with the Granite subset its
# checkout_granite.sh pins, for the Windows and Linux runtime and the Android client.

include(FetchContent)

FetchContent_Declare(
    pyrowave
    GIT_REPOSITORY https://github.com/V-Sekai-fire/pyrowave.git
    GIT_TAG 89f7e47d4abbf650c91fae766728af866c5e32a0
    SOURCE_SUBDIR none
)
FetchContent_Declare(
    granite
    GIT_REPOSITORY https://github.com/Themaister/Granite.git
    GIT_TAG 1b2d1801d2910fb09ebcded2f0bb3a3a781103b5
    GIT_SUBMODULES third_party/volk third_party/khronos/vulkan-headers
    SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(pyrowave granite)
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "null" CACHE STRING "" FORCE)
add_subdirectory(${granite_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/granite-build EXCLUDE_FROM_ALL)

add_library(oxrsys_pyrowave STATIC
    ${pyrowave_SOURCE_DIR}/pyrowave_encoder.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_decoder.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_common.cpp
    ${pyrowave_SOURCE_DIR}/pyrowave_c.cpp
    ${granite_SOURCE_DIR}/video/scaler.cpp
)
target_include_directories(oxrsys_pyrowave
    PUBLIC ${pyrowave_SOURCE_DIR}
    PRIVATE ${pyrowave_SOURCE_DIR}/shaders ${granite_SOURCE_DIR}/video
)
target_link_libraries(oxrsys_pyrowave PUBLIC granite-vulkan granite-math)
# Upstream's default PYROWAVE_FP32_MATH: FP32 arithmetic, reduced-range storage.
target_compile_definitions(oxrsys_pyrowave PRIVATE PYROWAVE_PRECISION=1)
set_target_properties(oxrsys_pyrowave PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(oxrsys_pyrowave PRIVATE /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005)
endif()
