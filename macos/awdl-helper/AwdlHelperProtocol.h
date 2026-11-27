// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef PLK_AWDL_HELPER_PROTOCOL_H
#define PLK_AWDL_HELPER_PROTOCOL_H

/*
 * What Pleikkari and its AWDL helper say to each other, over XPC.
 *
 * The helper is a launch daemon inside the app bundle that macOS manages (SMAppService):
 * the user allows it once in System Settings, under Login Items, and switches it off there.
 * It runs as root and can do exactly one thing: keep awdl0 down for as long as a client
 * holds it.
 *
 * A request is a dictionary with PLK_AWDL_KEY_OP; every request gets a reply with
 * PLK_AWDL_KEY_OK (bool), PLK_AWDL_KEY_UP (int64: 1 up, 0 down, -1 no such interface) and,
 * when something failed, PLK_AWDL_KEY_ERRNO (int64).
 *
 *   hold     awdl0 is taken down and kept down: the helper takes it down again within a
 *            second whenever macOS raises it
 *   release  this client no longer holds it; with the last hold gone, awdl0 comes back up
 *   status   changes nothing
 *
 * A hold ends with its connection. A client that crashes or is killed therefore cannot leave
 * AWDL down: the helper brings it back up when the connection goes away.
 */

#define PLK_AWDL_HELPER_SERVICE "fi.madekivi.pleikkari.awdl-helper"
#define PLK_AWDL_HELPER_PLIST "fi.madekivi.pleikkari.awdl-helper.plist"

#define PLK_AWDL_KEY_OP "op"
#define PLK_AWDL_OP_HOLD "hold"
#define PLK_AWDL_OP_RELEASE "release"
#define PLK_AWDL_OP_STATUS "status"

#define PLK_AWDL_KEY_OK "ok"
#define PLK_AWDL_KEY_UP "up"
#define PLK_AWDL_KEY_ERRNO "errno"

#endif // PLK_AWDL_HELPER_PROTOCOL_H
