cmake_minimum_required(VERSION 3.20)

project(plk_dualsense LANGUAGES CXX)

if(NOT APPLE)
	message(FATAL_ERROR "macos/dualsense is a macOS-only IOKit driver")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
	# IOHIDDeviceRegisterInputReportWithTimeStampCallback needs 10.15.
	set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
endif()

set(PLK_DS_WARNINGS -Wall -Wextra -Wshadow -Wno-unused-parameter)

# Protocol, writer and haptics pacing: no IOKit, unit tested without hardware.
add_library(plk_dualsense_core STATIC
	src/Protocol.cpp
	src/EffectsWriter.cpp
	src/HapticsSender.cpp)
target_include_directories(plk_dualsense_core PUBLIC include PRIVATE src)
target_compile_options(plk_dualsense_core PRIVATE ${PLK_DS_WARNINGS})

# The driver: IOKit device matching, input thread, output sink.
add_library(plk_dualsense STATIC src/DualSenseHID.cpp)
target_include_directories(plk_dualsense PUBLIC include PRIVATE src)
target_compile_options(plk_dualsense PRIVATE ${PLK_DS_WARNINGS})
target_link_libraries(plk_dualsense PUBLIC plk_dualsense_core "-framework IOKit" "-framework CoreFoundation")

add_executable(plk-dualsense-probe tools/probe.cpp)
target_compile_options(plk-dualsense-probe PRIVATE ${PLK_DS_WARNINGS})
target_link_libraries(plk-dualsense-probe PRIVATE plk_dualsense)

include(CTest)
if(BUILD_TESTING)
	add_executable(plk-dualsense-tests
		tests/test_main.cpp
		tests/test_crc.cpp
		tests/test_output.cpp
		tests/test_input.cpp
		tests/test_writer.cpp
		tests/test_haptics.cpp)
	target_include_directories(plk-dualsense-tests PRIVATE src tests)
	target_compile_options(plk-dualsense-tests PRIVATE ${PLK_DS_WARNINGS})
	target_compile_definitions(plk-dualsense-tests PRIVATE
		PLK_DS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
	target_link_libraries(plk-dualsense-tests PRIVATE plk_dualsense_core)
	add_test(NAME plk-dualsense-tests COMMAND plk-dualsense-tests)
endif()
