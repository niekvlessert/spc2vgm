cmake_minimum_required(VERSION 3.25)

project(spc2vgm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Match vcpkg's x64-windows-static triplet and avoid mixing LIBCMT with
    # MSVC's default dynamic runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(ZLIB REQUIRED)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/unarr/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing unarr submodule. Run: git submodule update --init --recursive")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_7Z ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_BZ2 OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_LZMA OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/unarr EXCLUDE_FROM_ALL)

set(BUILD_LIBAUDIO OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "" FORCE)
set(USE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(SNDEMU__ALL OFF CACHE BOOL "" FORCE)
set(SNDEMU__PRESET_MSX OFF CACHE BOOL "" FORCE)
set(SNDEMU_YMF262_ALL ON CACHE BOOL "" FORCE)
set(SNDEMU_YMF278B_ALL ON CACHE BOOL "" FORCE)
if(WIN32)
    # libvgm's bundled MSVC libiconv binary is 32-bit. Use the native Windows
    # character conversion backend for both 32-bit and 64-bit Windows builds.
    set(UTIL_CHARCNV_ICONV OFF CACHE BOOL "" FORCE)
    set(UTIL_CHARCNV_WINAPI ON CACHE BOOL "" FORCE)
endif()
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

add_executable(unarr_extract tools/unarr_extract.c)
target_link_libraries(unarr_extract PRIVATE unarr::unarr)

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

install(TARGETS spc2vgm spc_trace spc_render vgm_cmp unarr_extract RUNTIME DESTINATION bin)
install(PROGRAMS
    scripts/optimize_vgm_directory.sh
    scripts/package_vgmrips.py
    scripts/fetch_snesmusic.py
    DESTINATION scripts
)
install(FILES
    scripts/package_vgmrips.cmd
    scripts/fetch_snesmusic.cmd
    DESTINATION scripts
)
install(FILES README.md INFO_FOR_AI_AND_DEV.md THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES external/vgmtools/LICENSE DESTINATION licenses RENAME vgmtools-GPL-2.0.txt)
install(FILES extern/unarr/COPYING DESTINATION licenses RENAME unarr-LGPL-3.0.txt)
