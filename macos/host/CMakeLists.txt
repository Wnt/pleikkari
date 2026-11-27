# Native macOS session host: SessionHost, AudioOutput, the registration loader, the
# PLKVDMP1 dump writer, plk-host-probe and the unit tests. No Qt, SDL or FFmpeg.

set(PLK_HOST_CXX_PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(plk-host STATIC
	AudioOutput.cpp
	AudioOutput.h
	AwdlGuard.cpp
	AwdlGuard.h
	AwdlHelperClient.h
	AwdlHelperClient.mm
	FrameRing.h
	Playout.cpp
	Playout.h
	Registration.cpp
	Registration.h
	SessionHost.cpp
	SessionHost.h
	Sinks.h
	VideoDump.cpp
	VideoDump.h)
set_target_properties(plk-host PROPERTIES ${PLK_HOST_CXX_PROPERTIES})
target_include_directories(plk-host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(plk-host PRIVATE -Wall -Wextra)
# The AWDL helper's client: SMAppService is Objective-C. The protocol header is the helper's.
set_source_files_properties(AwdlHelperClient.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
set_target_properties(plk-host PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
target_include_directories(plk-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../awdl-helper")
if(PLK_TEAM_ID)
	set_property(SOURCE AwdlHelperClient.mm APPEND PROPERTY COMPILE_DEFINITIONS
		"PLK_AWDL_HELPER_REQUIREMENT=\"identifier \\\"fi.madekivi.pleikkari.awdl-helper\\\" and anchor apple generic and certificate leaf[subject.OU] = \\\"${PLK_TEAM_ID}\\\"\"")
endif()
# memset_s(), which the compiler may not optimise away, for wiping registration keys.
target_compile_definitions(plk-host PRIVATE __STDC_WANT_LIB_EXT1__=1)
target_link_libraries(plk-host PUBLIC
	chiaki-lib
	"-framework AudioToolbox"
	"-framework CoreAudio"
	"-framework CoreFoundation"
	"-framework Foundation"
	"-framework ServiceManagement"
	"-framework SystemConfiguration")

add_executable(plk-host-probe tools/plk-host-probe.cpp)
set_target_properties(plk-host-probe PROPERTIES ${PLK_HOST_CXX_PROPERTIES})
target_compile_options(plk-host-probe PRIVATE -Wall -Wextra)
target_link_libraries(plk-host-probe PRIVATE plk-host)

add_executable(plk-host-tests
	tests/AudioTests.cpp
	tests/AwdlGuardTests.cpp
	tests/main.cpp
	tests/RegistrationTests.cpp
	tests/SessionHostTests.cpp
	tests/TestHarness.h
	tests/VideoDumpTests.cpp)
set_target_properties(plk-host-tests PROPERTIES ${PLK_HOST_CXX_PROPERTIES})
target_compile_options(plk-host-tests PRIVATE -Wall -Wextra)
target_compile_definitions(plk-host-tests PRIVATE PLK_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(plk-host-tests PRIVATE plk-host)
add_test(NAME plk-host-tests COMMAND plk-host-tests WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
