cmake_minimum_required(VERSION 3.24)

# Pick the full Xcode SDK explicitly: on some machines the default `xcrun`
# resolution lands on a Command Line Tools SDK that xcodebuild cannot use.
if(APPLE AND NOT CMAKE_OSX_SYSROOT)
	execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
		OUTPUT_VARIABLE PLK_SDK OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	if(PLK_SDK)
		set(CMAKE_OSX_SYSROOT "${PLK_SDK}" CACHE PATH "macOS SDK")
	endif()
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS (CAMetalDisplayLink)")

project(plk_video LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fobjc-arc")
set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -fobjc-arc")

# Embed the Metal source as a string; it is compiled at runtime with
# newLibraryWithSource, so the build needs no offline Metal toolchain.
set(PLK_SHADER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/Shaders.metal")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PLK_SHADER_FILE}")
file(READ "${PLK_SHADER_FILE}" PLK_SHADER_SOURCE)
configure_file(src/ShaderSource.inc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ShaderSource.inc" @ONLY)

add_library(plkvideo STATIC
	include/plk/FrameEnhancer.h
	include/plk/VideoPipeline.h
	src/AnnexB.cpp
	src/AnnexB.h
	src/RollingStats.h
	src/ToneMap.cpp
	src/ToneMap.h
	src/VideoPipeline.mm)
target_include_directories(plkvideo
	PUBLIC include
	PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(plkvideo PUBLIC
	"-framework Foundation"
	"-framework CoreFoundation"
	"-framework CoreGraphics"
	"-framework CoreMedia"
	"-framework CoreVideo"
	"-framework VideoToolbox"
	"-framework Metal"
	"-framework MetalFX"
	"-framework QuartzCore")

add_executable(plk-video-replay
	tools/replay/main.mm
	tools/replay/Dump.cpp
	tools/replay/Dump.h)
target_include_directories(plk-video-replay PRIVATE src tools/replay)
target_link_libraries(plk-video-replay PRIVATE plkvideo "-framework AppKit")

enable_testing()
add_executable(plk-video-tests tests/test_annexb.cpp tools/replay/Dump.cpp)
target_include_directories(plk-video-tests PRIVATE src tools/replay)
target_link_libraries(plk-video-tests PRIVATE plkvideo)
add_test(NAME annexb COMMAND plk-video-tests)
add_executable(plk-video-tonemap-tests tests/test_tonemap.cpp)
target_include_directories(plk-video-tonemap-tests PRIVATE src)
target_link_libraries(plk-video-tonemap-tests PRIVATE plkvideo)
add_test(NAME tonemap COMMAND plk-video-tonemap-tests)

# The enhancer path, end to end: encodes its own stream, renders offscreen. Needs a GPU
# and the hardware H.264 encoder and reports itself skipped (77) without them.
add_executable(plk-video-enhancer-test tests/test_enhancer.mm)
target_link_libraries(plk-video-enhancer-test PRIVATE plkvideo)
add_test(NAME enhancer COMMAND plk-video-enhancer-test)
set_tests_properties(enhancer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# The same for an HDR picture: an HEVC Main 10 stream in PQ, the enhancer's SDR rendition
# of it, and the change taken back to PQ. Needs the hardware HEVC encoder; exits 77 without.
add_executable(plk-video-enhancer-hdr-test tests/test_enhancer_hdr.mm)
target_link_libraries(plk-video-enhancer-hdr-test PRIVATE plkvideo)
add_test(NAME enhancer-hdr COMMAND plk-video-enhancer-hdr-test)
set_tests_properties(enhancer-hdr PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
