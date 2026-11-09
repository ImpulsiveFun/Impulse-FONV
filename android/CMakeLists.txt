cmake_minimum_required(VERSION 3.22.1)
project(fnvprobe C)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The probe requires Android ARM64")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static codec libraries" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "No upstream codec test executables" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "No codec documentation install" FORCE)
add_subdirectory(../third_party/libogg-1.3.6 codec-ogg)
set(OGG_LIBRARY ogg CACHE STRING "Bundled Ogg target" FORCE)
set(OGG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libogg-1.3.6/include" CACHE PATH "Bundled Ogg headers" FORCE)
add_subdirectory(../third_party/libvorbis-1.3.7 codec-vorbis)
foreach(codec IN ITEMS ogg vorbis vorbisenc vorbisfile)
    target_compile_options(${codec} PRIVATE -O2 -ffp-contract=off -fvisibility=hidden)
endforeach()
file(TO_CMAKE_PATH "${FNV_GENERATED}" FNV_GENERATED)
file(GLOB lifted CONFIGURE_DEPENDS "${FNV_GENERATED}/tu_*.c")
if(NOT lifted)
    message(FATAL_ERROR "No translated code found. Run prepare.py first.")
endif()
list(APPEND lifted "${FNV_GENERATED}/table.c" "${FNV_GENERATED}/stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/rt_dispatch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/rt_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/gheap.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/game_checks.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvse_script.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/shims_sync.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/shims_text.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/shims_files.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/shims_win.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/com.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/dsound.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/android_audio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/android_video.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/android_media.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/mmio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/android_vorbis.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/android_graphics.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/android_texture.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/d3d9.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/d3dx9.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/sm2msl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/dinput.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/input.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/host/shims_kernel32.c")
set(staged_sources)
foreach(source IN LISTS lifted)
    get_filename_component(name "${source}" NAME)
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/lifted/${name}")
    configure_file("${source}" "${staged}" COPYONLY)
    list(APPEND staged_sources "${staged}")
endforeach()
add_library(fnvprobe SHARED probe.c startup.c ${staged_sources})
target_include_directories(fnvprobe PRIVATE ../recomp ../recomp/host "${FNV_GENERATED}")
target_compile_features(fnvprobe PRIVATE c_std_11)
target_link_libraries(fnvprobe PRIVATE m dl aaudio log EGL GLESv3 android vorbisfile vorbisenc)
target_link_options(fnvprobe PRIVATE -Wl,-Bsymbolic-functions)

# Lifted p-code has many temporary values; basic optimization is essential.
target_compile_options(fnvprobe PRIVATE -O3 -mtune=cortex-x4 -ffp-contract=off -fno-fast-math)
if(FNV_NATIVE_TLS)
    target_compile_options(fnvprobe PRIVATE -fno-emulated-tls)
endif()
if(FNV_FIXED_MEMORY)
    target_compile_definitions(fnvprobe PRIVATE RT_FIXED_MEM)
endif()
