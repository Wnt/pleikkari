# Native macOS session host: SessionHost, AudioOutput, the registration loader, the
# PLKVDMP1 dump writer, plk-host-probe and the unit tests. No Qt, SDL or FFmpeg.

set(PLK_HOST_CXX_PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(plk-host STATIC
	AudioOutput.cpp
	AudioOutput.h
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
# memset_s(), which the compiler may not optimise away, for wiping registration keys.
target_compile_definitions(plk-host PRIVATE __STDC_WANT_LIB_EXT1__=1)
target_link_libraries(plk-host PUBLIC
	chiaki-lib
	"-framework AudioToolbox"
	"-framework CoreAudio"
	"-framework CoreFoundation")

add_executable(plk-host-probe tools/plk-host-probe.cpp)
set_target_properties(plk-host-probe PROPERTIES ${PLK_HOST_CXX_PROPERTIES})
target_compile_options(plk-host-probe PRIVATE -Wall -Wextra)
target_link_libraries(plk-host-probe PRIVATE plk-host)

