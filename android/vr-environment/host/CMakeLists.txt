# PLE-603: off-headset render of the VR environments through Mesa (llvmpipe) on the
# build host. Same source file as the APK's native library; only the entry point differs.
cmake_minimum_required(VERSION 3.16)
project(pleikkari-vr-environment-host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

add_executable(vr-environment-render
        render.cpp
        ../../app/src/main/cpp/vr-environment.cpp)
target_include_directories(vr-environment-render PRIVATE ../../app/src/main/cpp)
target_compile_options(vr-environment-render PRIVATE -Wall -Wextra)
target_link_libraries(vr-environment-render PkgConfig::EGL PkgConfig::GLES PkgConfig::ZLIB)

# PLE-711: default-off room orientation A/B arm.
option(PLEIKKARI_VR_ORIENTATION_CUE "Faint floor cue in plain and void rooms" OFF)
target_compile_definitions(vr-environment-render PRIVATE
        PLEIKKARI_VR_ORIENTATION_CUE=$<BOOL:${PLEIKKARI_VR_ORIENTATION_CUE}>)
