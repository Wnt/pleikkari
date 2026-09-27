
add_library(munit "${CMAKE_CURRENT_SOURCE_DIR}/munit/munit.c")
target_include_directories(munit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/munit")

# MinGW's tmpfile() fails in some Windows/MSYS environments. MUnit uses it
# only to buffer test stderr, so disable that optional capture on Windows.
if(WIN32)
	target_compile_definitions(munit PRIVATE MUNIT_NO_BUFFER)
endif()

set(CHIAKI_UNIT_SOURCES
				main.c
				http.c
				rpcrypt.c
				gkcrypt.c
				takion.c
				senkusha.c
				feedbacksender.c
				fake_console.c
				fake_console.h
				linkwatchdog.c
				frameloss.c
				videoreceiver.c
				seqnum.c
				keystate.c
				reorderqueue.c
				fec.c
				test_log.c
				test_log.h
				bitstream.c
				video_presenter.c
				video_frame_latency.c
				latency_probe.c
				video_decoder_operating_rate.c
				video_decoder_codec_header.c
				vr_screen_placement.c
				vr_frame_pacing.c
				vr_ui_panel.c
				networkstats.c
				session_mtu_fallback.c
				../android/app/src/main/cpp/video-decoder-operating-rate.c
				../android/app/src/main/cpp/video-decoder-codec-header.c
				../android/app/src/main/cpp/video-presenter-age.c
				../android/app/src/main/cpp/video-presenter-recovery.c
				../android/app/src/main/cpp/video-presenter-histogram.c
				../android/app/src/main/cpp/video-presenter-cadence.c
				../android/app/src/main/cpp/video-presenter-dejitter.c
				../android/app/src/main/cpp/video-presenter-timing.c
				../android/app/src/main/cpp/vr-screen-placement.c
				../android/app/src/main/cpp/vr-frame-pacing.c
				../android/app/src/main/cpp/vr-ui-panel.c
				../android/app/src/main/cpp/video-frame-latency.c
				../android/app/src/main/cpp/latency-probe.c
				regist.c)

if(NOT CHIAKI_LIB_ENABLE_MBEDTLS)
	list(APPEND CHIAKI_UNIT_SOURCES aia.c)
endif()

if(CHIAKI_ENABLE_FFMPEG_DECODER)
	list(APPEND CHIAKI_UNIT_SOURCES ffmpegdecoder.c)
endif()

add_executable(chiaki-unit ${CHIAKI_UNIT_SOURCES})
add_test(NAME unit COMMAND chiaki-unit)

target_include_directories(chiaki-unit PRIVATE "${CMAKE_BINARY_DIR}/lib/protobuf")

target_link_libraries(chiaki-unit chiaki-lib munit)

# PLE-722: vr-ui-panel.c's trigonometry.
if(UNIX)
	target_link_libraries(chiaki-unit m)
endif()

if(CHIAKI_ENABLE_FFMPEG_DECODER)
	target_link_libraries(chiaki-unit FFMPEG::avcodec FFMPEG::avutil)
endif()

# PLE-766: compile and link the Go's GLSL shaders through the host's Mesa, so a
# precision or link error fails here instead of on the Go. Skips (77) without Mesa.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
	add_test(NAME vr-shaders COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/vr_shaders_check.py")
	set_tests_properties(vr-shaders PROPERTIES SKIP_RETURN_CODE 77)
endif()
