// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_AWDLGUARD_H
#define CHIAKI_AWDLGUARD_H

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class QProcess;

/**
 * Keeps Apple Wireless Direct Link down while a stream runs over Wi-Fi (macOS): straight
 * over it, or inside a VPN tunnel that does.
 *
 * AWDL (AirDrop, AirPlay, Handoff) shares the Wi-Fi radio: while it is up the radio leaves the
 * access point's channel for up to 100 ms every 0.5 to 2 s. Measured on a stream: ping to the
 * console 4 ms, with every sixth one between 30 and 95 ms, and a 95 ms stop in the picture each
 * time, after which six frames arrive at once.
 *
 * Taking awdl0 down needs root. The first time, the system's password dialog is shown once and a
 * sudoers rule is installed that allows exactly "ifconfig awdl0 down" and "ifconfig awdl0 up"
 * without a password (/private/etc/sudoers.d/chiaki-awdl; delete the file to undo). After that
 * the interface is taken down when a Wi-Fi stream starts, taken down again whenever macOS
 * raises it, and brought back up when the stream ends.
 *
 * One object serves the whole process and counts the streams that use it: a stream that is
 * retried several times a second while the console still answers "in use" must not show the
 * dialog once per attempt or switch the interface up and down each time.
 */
class AwdlGuard : public QObject
{
	Q_OBJECT

	public:
		static AwdlGuard *Instance();

		/**
		 * A stream starts.
		 * @param host the console's address or name, as the session connects to it
		 * @param on_declined called if the password dialog is dismissed: the caller switches
		 *                    the setting off, so that the dialog is not shown at every stream
		 * @return true when the stream leaves over Wi-Fi, as it is or inside a tunnel, and
		 *         Release() has to follow
		 */
		bool Acquire(const QString &host, std::function<void()> on_declined);
		/** A stream that Acquire() answered true for has ended. */
		void Release();

	private:
		explicit AwdlGuard(QObject *parent);
		void Tick();
		void TakeDown();
		void InstallRule();
		void BringUp();
		void Finished(bool taken_down);

		QTimer watch_timer;
		QTimer release_timer;
		QProcess *process = nullptr;
		std::function<void()> on_declined;
		int users = 0;
		bool brought_down = false;
		bool install_tried = false;
		bool gave_up = false;
		bool logged_down = false;
};

#endif // CHIAKI_AWDLGUARD_H
