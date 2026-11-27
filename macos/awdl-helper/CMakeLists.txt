# The AWDL helper: a launch daemon inside Pleikkari.app that keeps awdl0 down while a stream
# holds it (AwdlHelperProtocol.h). macos/app copies it into the bundle and signs it.
#
# PLK_TEAM_ID is the team of the certificate the app is signed with. With it, the helper
# listens only to Pleikkari signed by that team, and the app talks only to a helper signed by
# it. Without it (an ad hoc build) neither side can tell who the other is, and macOS does not
# register the daemon of an ad hoc app anyway.
set(PLK_TEAM_ID "" CACHE STRING "team identifier of the signing certificate (empty: ad hoc build)")

add_executable(plk-awdl-helper main.c AwdlHelperProtocol.h)
set_target_properties(plk-awdl-helper PROPERTIES
	OUTPUT_NAME fi.madekivi.pleikkari.awdl-helper
	C_STANDARD 11)
target_compile_options(plk-awdl-helper PRIVATE -Wall -Wextra -fblocks)
if(PLK_TEAM_ID)
	target_compile_definitions(plk-awdl-helper PRIVATE
		"PLK_AWDL_CLIENT_REQUIREMENT=\"identifier \\\"fi.madekivi.pleikkari.mac\\\" and anchor apple generic and certificate leaf[subject.OU] = \\\"${PLK_TEAM_ID}\\\"\"")
endif()
