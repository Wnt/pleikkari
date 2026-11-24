# The native macOS client and its modules (CHIAKI_ENABLE_MACOS_NATIVE):
#   host       SessionHost, AudioOutput, registration loader (links chiaki-lib)
#   video      VideoToolbox -> Metal pipeline (standalone project, also builds here)
#   dualsense  IOKit HID DualSense driver (standalone project, also builds here)

# The video pipeline uses CAMetalDisplayLink (macOS 14). The top-level project forces
# 13.0 for the lib; everything below this directory targets 14.0.
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)

# Objective-C++ is enabled here, so every directory below sees it (a language enabled in
# one subdirectory's project() is not visible in its siblings).
enable_language(OBJC)
enable_language(OBJCXX)

add_subdirectory(host)
add_subdirectory(video)
add_subdirectory(dualsense)
