# Pleikkari.app: the native macOS PS5 Remote Play client. AppKit + the three modules:
#   plk-host (session, audio, registration), plkvideo (VideoToolbox -> Metal),
#   plk_dualsense (IOKit HID). No Qt, SDL or libplacebo.

set(PLK_APP_VERSION "1.0.0")
set(PLK_SIGN_IDENTITY "-" CACHE STRING "codesign identity for Pleikkari.app and what is in it (- is ad hoc)")

# Everything but main.mm and the window: unit tested in plk-app-tests.
add_library(plk-app-core STATIC
	src/CameraWarp.cpp
	src/CameraWarp.h
	src/Controller.cpp
	src/Controller.h
	src/FrameAnalysis.cpp
	src/FrameAnalysis.h
	src/FrameHud.cpp
	src/FrameHud.h
	src/FrameTelemetry.cpp
	src/FrameTelemetry.h
	src/GamepadMapper.cpp
	src/GamepadMapper.h
	src/GamepadRumble.cpp
	src/GamepadRumble.h
	src/Gamepads.h
	src/Gamepads.mm
	src/Log.cpp
	src/Log.h
	src/NeuralEnhancer.cpp
	src/NeuralEnhancer.h
	src/Options.cpp
	src/Options.h
	src/PadMapper.cpp
	src/PadMapper.h
	src/PipelinePanel.cpp
	src/PipelinePanel.h
	src/Replay.cpp
	src/Replay.h
	src/Runner.cpp
	src/Runner.h
	src/SavedChoices.cpp
	src/SavedChoices.h
	src/StreamSession.cpp
	src/StreamSession.h
	src/UsbHaptics.cpp
	src/UsbHaptics.h
	src/VideoPath.cpp
	src/VideoPath.h
	"${CMAKE_CURRENT_SOURCE_DIR}/../video/tools/replay/Dump.cpp")
set_target_properties(plk-app-core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# The neural rendering engine's C header only: the engine itself (macos/nr, Swift) is not
# linked, the app opens it at run time when it is there.
target_include_directories(plk-app-core PUBLIC src PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../video/tools/replay"
	"${CMAKE_CURRENT_SOURCE_DIR}/../nr/Sources/CPlkNR/include")
target_compile_options(plk-app-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(src/Gamepads.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
set_target_properties(plk-app-core PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_link_libraries(plk-app-core PUBLIC plk-host plkvideo plk_dualsense "-framework GameController" "-framework CoreHaptics"
	"-framework Foundation" "-framework CoreGraphics" "-framework CoreText")

set(PLK_APP_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
configure_file(bundle/Info.plist.in "${PLK_APP_PLIST}" @ONLY)

add_executable(Pleikkari MACOSX_BUNDLE
	src/main.mm
	src/AnalysisView.h
	src/AnalysisView.mm
	src/PipelinePanelView.h
	src/PipelinePanelView.mm
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
# The neural rendering engine, when macos/nr/build.sh has built it: PLK_NR_DIST is that
# script's dist directory (libplknr.dylib and MLX's resource bundle with its Metal
# library). The library goes where the app looks for it and the bundle where MLX does;
# without PLK_NR_DIST the app is built exactly as before and has no neural rendering.
# Models are never copied into the app: they are NVIDIA's and stay where the user put them.
set(PLK_NR_DIST "" CACHE PATH "dist directory of macos/nr/build.sh (empty: no neural rendering in the bundle)")
if(PLK_NR_DIST)
	if(NOT EXISTS "${PLK_NR_DIST}/libplknr.dylib" OR NOT IS_DIRECTORY "${PLK_NR_DIST}/mlx-swift_Cmlx.bundle")
		message(FATAL_ERROR "PLK_NR_DIST=${PLK_NR_DIST} has no libplknr.dylib and mlx-swift_Cmlx.bundle: run macos/nr/build.sh")
	endif()
	add_custom_command(TARGET Pleikkari POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/Frameworks"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PLK_NR_DIST}/libplknr.dylib"
			"$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/Frameworks/libplknr.dylib"
		COMMAND ${CMAKE_COMMAND} -E copy_directory "${PLK_NR_DIST}/mlx-swift_Cmlx.bundle"
			"$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/Resources/mlx-swift_Cmlx.bundle"
		COMMAND codesign --force --sign "${PLK_SIGN_IDENTITY}" --timestamp=none "$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/Frameworks/libplknr.dylib"
		COMMENT "Copying the neural rendering engine into Pleikkari.app"
		VERBATIM)
endif()
# The AWDL helper goes into the bundle where its launchd plist says it is (BundleProgram),
# and the plist where SMAppService looks for a daemon's.
add_custom_command(TARGET Pleikkari POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/Library/LaunchDaemons"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../awdl-helper/fi.madekivi.pleikkari.awdl-helper.plist"
		"$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/Library/LaunchDaemons/fi.madekivi.pleikkari.awdl-helper.plist"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:plk-awdl-helper>"
		"$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/MacOS/fi.madekivi.pleikkari.awdl-helper"
	COMMAND codesign --force --sign "${PLK_SIGN_IDENTITY}" --timestamp=none --identifier fi.madekivi.pleikkari.awdl-helper
		"$<TARGET_BUNDLE_CONTENT_DIR:Pleikkari>/MacOS/fi.madekivi.pleikkari.awdl-helper"
	COMMENT "Copying the AWDL helper into Pleikkari.app"
	VERBATIM)
add_dependencies(Pleikkari plk-awdl-helper)
# Signed last, after the engine and the helper are in place: they are sealed with the rest.
# The signature gives the bundle a stable identity for the system's privacy prompts (local
# network) and for Game Mode. PLK_SIGN_IDENTITY is "-" (ad hoc) unless a certificate is named;
# with one (and its team in PLK_TEAM_ID) macOS registers the AWDL helper, and the helper and
# the app check each other's signature.
add_custom_command(TARGET Pleikkari POST_BUILD
	COMMAND codesign --force --sign "${PLK_SIGN_IDENTITY}" --timestamp=none "$<TARGET_BUNDLE_DIR:Pleikkari>"
	COMMENT "Signing Pleikkari.app (${PLK_SIGN_IDENTITY})"
	VERBATIM)

# The host module's test harness (PLK_TEST, CHECK, REQUIRE).
# What the gamepad support sees and whether the motors run, without a window or a stream.
add_executable(plk-gamepad-probe tools/GamepadProbe.mm)
set_target_properties(plk-gamepad-probe PROPERTIES CXX_STANDARD 20 OBJCXX_STANDARD 20)
target_compile_options(plk-gamepad-probe PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
target_include_directories(plk-gamepad-probe PRIVATE src)
target_link_libraries(plk-gamepad-probe PRIVATE plk-app-core)

# A --stick-sweep recording played through the camera warp again, with no console.
add_executable(plk-warp-eval tools/WarpEval.cpp)
set_target_properties(plk-warp-eval PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(plk-warp-eval PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(plk-warp-eval PRIVATE src)
target_link_libraries(plk-warp-eval PRIVATE plk-app-core)

# The analysis panel drawn into a PNG from made-up numbers: a look at it without a window.
add_executable(plk-analysis-render tools/AnalysisRender.mm src/AnalysisView.h src/AnalysisView.mm)
set_target_properties(plk-analysis-render PROPERTIES CXX_STANDARD 20 OBJCXX_STANDARD 20)
target_compile_options(plk-analysis-render PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
target_include_directories(plk-analysis-render PRIVATE src)
target_link_libraries(plk-analysis-render PRIVATE plk-app-core "-framework AppKit")

# The pipeline panel, the frame rate overlay and the analysis panel drawn into PNGs, and
# with --check the panel's mouse handling: what a click on each thing it drew sends.
add_executable(plk-panel-render tools/PanelRender.mm src/AnalysisView.h src/AnalysisView.mm src/PipelinePanelView.h
	src/PipelinePanelView.mm)
set_target_properties(plk-panel-render PROPERTIES CXX_STANDARD 20 OBJCXX_STANDARD 20)
target_compile_options(plk-panel-render PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
target_include_directories(plk-panel-render PRIVATE src)
target_link_libraries(plk-panel-render PRIVATE plk-app-core "-framework AppKit")
add_test(NAME plk-panel-view COMMAND plk-panel-render --check)

add_executable(plk-app-tests
	"${CMAKE_CURRENT_SOURCE_DIR}/../host/tests/main.cpp"
	tests/AnalysisTests.cpp
	tests/CameraWarpTests.cpp
	tests/ControllerTests.cpp
	tests/FrameHudTests.cpp
	tests/GamepadMapperTests.cpp
	tests/GamepadRumbleTests.cpp
	tests/LogTests.cpp
	tests/NeuralEnhancerTests.cpp
	tests/OptionsTests.cpp
	tests/PadMapperTests.cpp
	tests/PipelinePanelTests.cpp
	tests/SavedChoicesTests.cpp
	tests/TelemetryTests.cpp
	tests/VideoPathTests.cpp)
set_target_properties(plk-app-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(plk-app-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(plk-app-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host/tests")
target_link_libraries(plk-app-tests PRIVATE plk-app-core)
add_test(NAME plk-app-tests COMMAND plk-app-tests WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
