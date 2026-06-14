cmake_minimum_required(VERSION 3.16)

project(spc2vgm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)

set(BUILD_LIBAUDIO OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "" FORCE)
set(USE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(SNDEMU__ALL OFF CACHE BOOL "" FORCE)
set(SNDEMU__PRESET_MSX OFF CACHE BOOL "" FORCE)
set(SNDEMU_YMF262_ALL ON CACHE BOOL "" FORCE)
set(SNDEMU_YMF278B_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(external/libvgm EXCLUDE_FROM_ALL)

set(SNES_SPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/smwcentral-spc-player/src/snes_spc")

if(NOT EXISTS "${SNES_SPC_DIR}/SNES_SPC.cpp")
    message(FATAL_ERROR
        "Missing smwcentral-spc-player submodule. Run: git submodule update --init --recursive")
endif()

add_library(snes_spc STATIC
    "${SNES_SPC_DIR}/SNES_SPC.cpp"
    "${SNES_SPC_DIR}/SNES_SPC_misc.cpp"
    "${SNES_SPC_DIR}/SNES_SPC_state.cpp"
    "${SNES_SPC_DIR}/SPC_DSP.cpp"
    "${SNES_SPC_DIR}/SPC_Filter.cpp"
)

target_include_directories(snes_spc PUBLIC "${SNES_SPC_DIR}")

if(MSVC)
    target_compile_options(snes_spc PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/spc_trace_hook.h")
else()
    target_compile_options(snes_spc PRIVATE "-include${CMAKE_CURRENT_SOURCE_DIR}/src/spc_trace_hook.h")
endif()

add_executable(spc2vgm src/spc2vgm.cpp)
target_link_libraries(spc2vgm PRIVATE snes_spc vgm-player vgm-emu vgm-utils)

add_executable(spc_trace tools/spc_trace.cpp)
target_link_libraries(spc_trace PRIVATE snes_spc)

add_executable(spc_render tools/spc_render.cpp)
target_link_libraries(spc_render PRIVATE snes_spc)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/vgmtools/vgm_cmp.c")
    message(FATAL_ERROR
        "Missing vgmtools submodule. Run: git submodule update --init --recursive")
endif()

add_executable(vgm_cmp
    external/vgmtools/vgm_cmp.c
    external/vgmtools/chip_cmp.c
)
target_link_libraries(vgm_cmp PRIVATE ZLIB::ZLIB)
if(MSVC)
    target_compile_definitions(vgm_cmp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS spc2vgm spc_trace spc_render vgm_cmp RUNTIME DESTINATION bin)
install(PROGRAMS
    scripts/optimize_vgm_directory.sh
    scripts/package_vgmrips.py
    DESTINATION scripts
)
install(FILES README.md INFO_FOR_AI_AND_DEV.md THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES external/vgmtools/LICENSE DESTINATION licenses RENAME vgmtools-GPL-2.0.txt)
