# Pleikkari.app: the native macOS PS5 Remote Play client. AppKit + the three modules:
#   plk-host (session, audio, registration), plkvideo (VideoToolbox -> Metal),
#   plk_dualsense (IOKit HID). No Qt, SDL or libplacebo.

set(PLK_APP_VERSION "1.0.0")

# Everything but main.mm and the window: unit tested in plk-app-tests.
add_library(plk-app-core STATIC
	src/Controller.cpp
	src/Controller.h
	src/FrameTelemetry.cpp
	src/FrameTelemetry.h
	src/Log.cpp
	src/Log.h
	src/Options.cpp
	src/Options.h
	src/PadMapper.cpp
	src/PadMapper.h
	src/Replay.cpp
	src/Replay.h
	src/Runner.cpp
	src/Runner.h
	src/StreamSession.cpp
	src/StreamSession.h
	src/UsbHaptics.cpp
	src/UsbHaptics.h
	src/VideoPath.cpp
	src/VideoPath.h
	"${CMAKE_CURRENT_SOURCE_DIR}/../video/tools/replay/Dump.cpp")
set_target_properties(plk-app-core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(plk-app-core PUBLIC src PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../video/tools/replay")
target_compile_options(plk-app-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(plk-app-core PUBLIC plk-host plkvideo plk_dualsense)

set(PLK_APP_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
configure_file(bundle/Info.plist.in "${PLK_APP_PLIST}" @ONLY)

add_executable(Pleikkari MACOSX_BUNDLE
	src/main.mm
	src/StreamWindow.h
	src/StreamWindow.mm)
set_target_properties(Pleikkari PROPERTIES
	OUTPUT_NAME Pleikkari
	MACOSX_BUNDLE_INFO_PLIST "${PLK_APP_PLIST}"
	CXX_STANDARD 20
	OBJCXX_STANDARD 20
	XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
target_compile_options(Pleikkari PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(Pleikkari PRIVATE plk-app-core "-framework AppKit" "-framework QuartzCore" "-framework Metal")
# Ad-hoc signature, so the bundle has a stable identity for the system's privacy prompts
# (local network) and for Game Mode.
add_custom_command(TARGET Pleikkari POST_BUILD
	COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:Pleikkari>"
	COMMENT "Signing Pleikkari.app (ad hoc)"
	VERBATIM)

# The host module's test harness (PLK_TEST, CHECK, REQUIRE).
add_executable(plk-app-tests
	"${CMAKE_CURRENT_SOURCE_DIR}/../host/tests/main.cpp"
	tests/ControllerTests.cpp
	tests/LogTests.cpp
	tests/OptionsTests.cpp
	tests/PadMapperTests.cpp
	tests/TelemetryTests.cpp)
set_target_properties(plk-app-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(plk-app-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(plk-app-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host/tests")
target_link_libraries(plk-app-tests PRIVATE plk-app-core)
add_test(NAME plk-app-tests COMMAND plk-app-tests WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
