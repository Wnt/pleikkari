
cmake_minimum_required(VERSION 3.2)

set(CMAKE_CXX_STANDARD 14)

add_library(chiaki-jni SHARED
        src/main/cpp/chiaki-jni.c
        src/main/cpp/log.h
        src/main/cpp/log.c
        src/main/cpp/video-decoder.h
        src/main/cpp/video-decoder.c
        src/main/cpp/video-decoder-operating-rate.h
        src/main/cpp/video-decoder-operating-rate.c
        src/main/cpp/video-decoder-codec-header.h
        src/main/cpp/video-decoder-codec-header.c
        src/main/cpp/video-presenter.h
        src/main/cpp/video-presenter-age.h
        src/main/cpp/video-presenter-age.c
        src/main/cpp/video-presenter-recovery.h
        src/main/cpp/video-presenter-recovery.c
        src/main/cpp/video-presenter-histogram.h
        src/main/cpp/video-presenter-histogram.c
        src/main/cpp/video-presenter-cadence.h
        src/main/cpp/video-presenter-cadence.c
        src/main/cpp/video-presenter-dejitter.h
        src/main/cpp/video-presenter-dejitter.c
        src/main/cpp/video-presenter-timing.h
        src/main/cpp/video-presenter-timing.c
        src/main/cpp/video-presenter.c
        src/main/cpp/video-frame-latency.h
        src/main/cpp/video-frame-latency.c
        src/main/cpp/latency-probe.h
        src/main/cpp/latency-probe.c
        src/main/cpp/latency-trace.h
        src/main/cpp/latency-trace.c
        src/main/cpp/audio-decoder.h
        src/main/cpp/audio-decoder.c
        src/main/cpp/audio-output.h
        src/main/cpp/audio-output.cpp
        src/main/cpp/circular-fifo.hpp)

if(CHIAKI_ANDROID_EGL_RENDERER)
    target_sources(chiaki-jni PRIVATE
            src/main/cpp/egl-renderer.cpp
            src/main/cpp/egl-renderer.h)
    target_compile_definitions(chiaki-jni PRIVATE CHIAKI_ANDROID_EGL_RENDERER=1)
endif()
target_link_libraries(chiaki-jni chiaki-lib)

find_library(ANDROID_LIB_LOG log)
find_library(ANDROID_LIB_MEDIANDK mediandk)
find_library(ANDROID_LIB_ANDROID android)
if(CHIAKI_ANDROID_EGL_RENDERER)
    find_library(ANDROID_LIB_EGL EGL)
    find_library(ANDROID_LIB_GLESV3 GLESv3)
    target_link_libraries(chiaki-jni "${ANDROID_LIB_EGL}" "${ANDROID_LIB_GLESV3}")
endif()
target_link_libraries(chiaki-jni "${ANDROID_LIB_LOG}" "${ANDROID_LIB_MEDIANDK}" "${ANDROID_LIB_ANDROID}")

find_package (oboe REQUIRED CONFIG)
target_link_libraries(chiaki-jni oboe::oboe)

# PLE-603: the VR environment renderer (plain GLES 3.0, no VrApi) as its own small library
# so it compiles in every gate and can be loaded on demand. PLE-602's VrApi library links
# it and calls the C API in vr-environment.h; the JNI file serves the debug preview.
add_library(pleikkari-vr-environment SHARED
        src/main/cpp/vr-environment.h
        src/main/cpp/vr-environment.cpp
        src/main/cpp/vr-environment-jni.cpp)
set_target_properties(pleikkari-vr-environment PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
find_library(ANDROID_LIB_EGL_ENV EGL)
find_library(ANDROID_LIB_GLESV3_ENV GLESv3)
target_link_libraries(pleikkari-vr-environment "${ANDROID_LIB_LOG}" "${ANDROID_LIB_EGL_ENV}" "${ANDROID_LIB_GLESV3_ENV}")

# PLE-524: adb-shell sanitizer stress tests (android/native-stress/README.md). Built only when
# -PchiakiNativeStress=thread|address|hwaddress sets CHIAKI_ANDROID_NATIVE_STRESS; never packaged
# into the APK, and chiaki-jni itself is left uninstrumented (use -PchiakiSanitizer for that).
set(CHIAKI_ANDROID_NATIVE_STRESS "" CACHE STRING "Sanitizer for the native stress tests: thread, address or hwaddress (empty: not built)")
set(CHIAKI_ANDROID_NATIVE_STRESS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/native-stress" CACHE PATH "Where the stress tests and their runtime libraries are collected, per ABI")
set(CHIAKI_ANDROID_NATIVE_STRESS_AUDIO_OUTPUT_SOURCE "" CACHE FILEPATH "audio-output.cpp to stress instead of the tree's own (the harness self-test)")
if(CHIAKI_ANDROID_NATIVE_STRESS)
    set(stress_sanitizer "${CHIAKI_ANDROID_NATIVE_STRESS}")
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(stress_arch aarch64)
        set(stress_triple aarch64-linux-android)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(stress_arch x86_64)
        set(stress_triple x86_64-linux-android)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(stress_arch arm)
        set(stress_triple arm-linux-androideabi)
    elseif(ANDROID_ABI STREQUAL "x86")
        set(stress_arch i686)
        set(stress_triple i686-linux-android)
    else()
        message(FATAL_ERROR "native stress: unknown ABI ${ANDROID_ABI}")
    endif()
    if(stress_sanitizer STREQUAL "thread")
        set(stress_runtime tsan)
        set(stress_supported aarch64 x86_64)
    elseif(stress_sanitizer STREQUAL "address")
        set(stress_runtime asan)
        set(stress_supported aarch64 x86_64 arm i686)
    elseif(stress_sanitizer STREQUAL "hwaddress")
        set(stress_runtime hwasan)
        set(stress_supported aarch64)
    else()
        message(FATAL_ERROR "CHIAKI_ANDROID_NATIVE_STRESS must be thread, address or hwaddress, not '${stress_sanitizer}'")
    endif()
    list(FIND stress_supported "${stress_arch}" stress_arch_index) # IN_LIST needs policy CMP0057
    if(stress_arch_index EQUAL -1)
        message(FATAL_ERROR "native stress: the NDK has no ${stress_sanitizer} runtime for ${ANDROID_ABI}; build with -PchiakiAbiFilters=<one of ${stress_supported}>")
    endif()

    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE stress_clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(stress_runtime_lib "${stress_clang_resource_dir}/lib/linux/libclang_rt.${stress_runtime}-${stress_arch}-android.so")
    set(stress_cxx_shared "${CMAKE_SYSROOT}/usr/lib/${stress_triple}/libc++_shared.so")
    set(stress_push_libs "${stress_runtime_lib}" "${stress_cxx_shared}")
    if(stress_sanitizer STREQUAL "hwaddress")
        # Android 14+ ships the HWASan runtime; run.sh sets LD_HWASAN=1 so the linker uses it
        # (and the system's HWASan libraries). The NDK copy crashes in its pre-init constructor there.
        set(stress_push_libs "${stress_cxx_shared}")
    endif()
    foreach(stress_file "${stress_runtime_lib}" "${stress_cxx_shared}")
        if(NOT EXISTS "${stress_file}")
            message(FATAL_ERROR "native stress: ${stress_file} not found")
        endif()
    endforeach()

    # Built into this configuration's own binary dir, then collected into the shared output dir
    # on every build, so that dir always holds what the last build configured, never a binary
    # another -PchiakiNativeStress* combination left behind.
    set(stress_stage "${CMAKE_CURRENT_BINARY_DIR}/native-stress")
    set(stress_out "${CHIAKI_ANDROID_NATIVE_STRESS_OUTPUT_DIR}/${ANDROID_ABI}")
    add_executable(chiaki-audio-output-stress ../native-stress/audio-output-stress.cpp)
    set_target_properties(chiaki-audio-output-stress PROPERTIES
            CXX_STANDARD 17
            RUNTIME_OUTPUT_DIRECTORY "${stress_stage}")
    # The harness #includes audio-output.cpp, so its directory is on the include path either way.
    target_include_directories(chiaki-audio-output-stress PRIVATE src/main/cpp)
    if(CHIAKI_ANDROID_NATIVE_STRESS_AUDIO_OUTPUT_SOURCE)
        target_compile_definitions(chiaki-audio-output-stress PRIVATE
                "CHIAKI_STRESS_AUDIO_OUTPUT_SOURCE=\"${CHIAKI_ANDROID_NATIVE_STRESS_AUDIO_OUTPUT_SOURCE}\"")
    endif()
    target_compile_options(chiaki-audio-output-stress PRIVATE
            -fsanitize=${stress_sanitizer} -fno-omit-frame-pointer -g)
    target_link_options(chiaki-audio-output-stress PRIVATE -fsanitize=${stress_sanitizer})
    target_link_libraries(chiaki-audio-output-stress chiaki-lib oboe::oboe "${ANDROID_LIB_LOG}")
    # run.sh reads these back, so it never runs a build under the wrong name.
    file(WRITE "${stress_stage}/SANITIZER" "${stress_sanitizer}\n")
    file(WRITE "${stress_stage}/SOURCE" "${CHIAKI_ANDROID_NATIVE_STRESS_AUDIO_OUTPUT_SOURCE}\n")
    add_custom_target(chiaki-native-stress-collect ALL
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${stress_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${stress_out}"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:chiaki-audio-output-stress>" "${stress_stage}/SANITIZER" "${stress_stage}/SOURCE"
                    ${stress_push_libs} "$<TARGET_FILE:oboe::oboe>" "${stress_out}/"
            DEPENDS chiaki-audio-output-stress
            VERBATIM)
endif()

# PLE-602: separate JNI library; the normal decoder/audio library has no VrApi dependency.
if(CHIAKI_GO_VR AND ANDROID_ABI STREQUAL "arm64-v8a")
    add_library(vrapi SHARED IMPORTED)
    set_target_properties(vrapi PROPERTIES IMPORTED_LOCATION
        "${CHIAKI_GO_VR_SDK}/VrApi/Libs/Android/arm64-v8a/Release/libvrapi.so")
    # PLE-702: vr-screen-placement.c is plain C, unit-tested in chiaki-unit.
    # PLE-715: vr-frame-pacing.c too.
    # PLE-722: vr-ui-panel.c (the VR UI's panel geometry) is plain C too, tested the same way.
    add_library(pleikkari-vr SHARED src/vr/cpp/vr-cinema.cpp src/vr/cpp/vr-ui-layers.cpp
        src/main/cpp/vr-screen-placement.c src/main/cpp/vr-frame-pacing.c src/main/cpp/vr-ui-panel.c)
    set_target_properties(pleikkari-vr PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # PLE-603: the environment renderer draws the room and the curved screen into the eye buffers.
    target_include_directories(pleikkari-vr PRIVATE "${CHIAKI_GO_VR_SDK}/VrApi/Include" src/main/cpp)
    target_link_libraries(pleikkari-vr vrapi pleikkari-vr-environment android log EGL GLESv3)
endif()
