// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <awdlguard.h>

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>

#include <SystemConfiguration/SystemConfiguration.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

Q_LOGGING_CATEGORY(awdlGuard, "chiaki.gui.awdl", QtInfoMsg)

static const char *const AWDL_IF = "awdl0";
static const char *const SUDOERS_FILE = "/private/etc/sudoers.d/chiaki-awdl";

// Name of the interface a packet to host would leave from. Nothing is sent: connecting a UDP
// socket only asks the routing table. A name that is not an address literal is not resolved
// here (that could block); the default route answers instead.
static QString OutgoingInterface(const QString &host)
{
	struct addrinfo hints = {};
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;
	struct addrinfo *res = nullptr;
	if(getaddrinfo(host.toUtf8().constData(), "9296", &hints, &res) != 0 || !res)
	{
		res = nullptr;
		if(getaddrinfo("1.1.1.1", "9296", &hints, &res) != 0 || !res)
			return QString();
	}
	QString name;
	int sock = socket(res->ai_family, SOCK_DGRAM, 0);
	if(sock >= 0)
	{
		struct sockaddr_storage local = {};
		socklen_t local_len = sizeof(local);
		if(::connect(sock, res->ai_addr, res->ai_addrlen) == 0
			&& getsockname(sock, (struct sockaddr *)&local, &local_len) == 0)
		{
			struct ifaddrs *ifs = nullptr;
			if(getifaddrs(&ifs) == 0)
			{
				for(struct ifaddrs *i = ifs; i && name.isEmpty(); i = i->ifa_next)
				{
					if(!i->ifa_addr || i->ifa_addr->sa_family != local.ss_family)
						continue;
					bool same = false;
					if(local.ss_family == AF_INET)
						same = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == ((struct sockaddr_in *)&local)->sin_addr.s_addr;
					else if(local.ss_family == AF_INET6)
						same = memcmp(&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, &((struct sockaddr_in6 *)&local)->sin6_addr, sizeof(struct in6_addr)) == 0;
					if(same)
						name = QString::fromLatin1(i->ifa_name);
				}
				freeifaddrs(ifs);
			}
		}
		close(sock);
	}
	freeaddrinfo(res);
	return name;
}

static bool IsWifi(const QString &ifname)
{
	bool wifi = false;
	CFArrayRef all = SCNetworkInterfaceCopyAll();
	if(!all)
		return false;
	CFStringRef wanted = ifname.toCFString();
	for(CFIndex i = 0; i < CFArrayGetCount(all) && !wifi; i++)
	{
		SCNetworkInterfaceRef iface = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
		CFStringRef bsd = SCNetworkInterfaceGetBSDName(iface);
		CFStringRef type = SCNetworkInterfaceGetInterfaceType(iface);
		if(bsd && type && CFEqual(bsd, wanted) && CFEqual(type, kSCNetworkInterfaceTypeIEEE80211))
			wifi = true;
	}
	CFRelease(wanted);
	CFRelease(all);
	return wifi;
}

// 1 up, 0 down, -1 no such interface
static int AwdlIsUp()
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock < 0)
		return -1;
	struct ifreq ifr = {};
	strlcpy(ifr.ifr_name, AWDL_IF, sizeof(ifr.ifr_name));
	int r = ioctl(sock, SIOCGIFFLAGS, &ifr);
	close(sock);
	if(r < 0)
		return -1;
	return (ifr.ifr_flags & IFF_UP) ? 1 : 0;
}

AwdlGuard *AwdlGuard::Instance()
{
	static AwdlGuard *instance = new AwdlGuard(QCoreApplication::instance());
	return instance;
}

AwdlGuard::AwdlGuard(QObject *parent) : QObject(parent)
{
	watch_timer.setInterval(1000);
	connect(&watch_timer, &QTimer::timeout, this, &AwdlGuard::Tick);
	// The interface comes back a moment after the last stream ended, so that a reconnect
	// does not switch it up and down.
	release_timer.setSingleShot(true);
	release_timer.setInterval(3000);
	connect(&release_timer, &QTimer::timeout, this, [this]() {
		watch_timer.stop();
		BringUp();
	});
	if(QCoreApplication::instance())
		connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &AwdlGuard::BringUp);
}

bool AwdlGuard::Acquire(const QString &host, std::function<void()> declined)
{
	const QString ifname = OutgoingInterface(host);
	if(ifname.isEmpty() || !IsWifi(ifname) || AwdlIsUp() < 0)
		return false;
	on_declined = std::move(declined);
	release_timer.stop();
	users++;
	// Said once per stream, not once per attempt of a stream that is being retried.
	if(!watch_timer.isActive() && !gave_up)
		qCInfo(awdlGuard) << "Stream runs over Wi-Fi (" << ifname << "): keeping" << AWDL_IF << "down while it lasts";
	if(!gave_up)
	{
		Tick();
		watch_timer.start();
	}
	return true;
}

void AwdlGuard::Release()
{
	if(users > 0 && --users == 0)
		release_timer.start();
}

void AwdlGuard::BringUp()
{
	watch_timer.stop();
	if(!brought_down)
		return;
	brought_down = false;
	logged_down = false;
	QProcess::startDetached("/usr/bin/sudo", {"-n", "/sbin/ifconfig", AWDL_IF, "up"});
	qCInfo(awdlGuard) << "Stream ended:" << AWDL_IF << "is brought back up";
}

void AwdlGuard::Tick()
{
	if(process || AwdlIsUp() != 1)
		return;
	TakeDown();
}

// A command that ran as root has ended.
void AwdlGuard::Finished(bool taken_down)
{
	process->deleteLater();
	process = nullptr;
	if(!taken_down)
		return;
	brought_down = true;
	if(!logged_down)
		qCInfo(awdlGuard) << AWDL_IF << "taken down";
	logged_down = true;
	// The stream may have ended while the password dialog was open.
	if(users == 0 && !release_timer.isActive())
		BringUp();
}

void AwdlGuard::TakeDown()
{
	process = new QProcess(this);
	connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
		const bool ok = status == QProcess::NormalExit && code == 0;
		Finished(ok);
		if(ok)
			return;
		// sudo -n fails when the rule is not there yet.
		if(!install_tried)
			InstallRule();
		else
		{
			gave_up = true;
			watch_timer.stop();
		}
	});
	process->start("/usr/bin/sudo", {"-n", "/sbin/ifconfig", AWDL_IF, "down"});
}

void AwdlGuard::InstallRule()
{
	install_tried = true;
	const struct passwd *pw = getpwuid(getuid());
	const QString user = pw ? QString::fromLocal8Bit(pw->pw_name) : QString();
	// The name goes into a sudoers line and a shell command: anything unusual is refused.
	static const QRegularExpression plain_name("^[A-Za-z_][A-Za-z0-9_-]*$");
	if(!plain_name.match(user).hasMatch())
	{
		qCWarning(awdlGuard) << "Cannot install the AWDL rule for this user name";
		gave_up = true;
		watch_timer.stop();
		return;
	}
	// The rule is checked with visudo before it is installed: a sudoers file with an error in
	// it would switch sudo off altogether. No double quote or backslash below, so the command
	// can sit in an AppleScript string as it is.
	const QString command = QString(
		"t=$(/usr/bin/mktemp /tmp/chiaki-awdl.XXXXXX)"
		" && /bin/echo '%1 ALL=(root) NOPASSWD: /sbin/ifconfig awdl0 down, /sbin/ifconfig awdl0 up' > $t"
		" && /usr/sbin/visudo -cf $t > /dev/null"
		" && /usr/bin/install -m 0440 -o root -g wheel $t %2"
		" && /bin/rm -f $t"
		" && /sbin/ifconfig awdl0 down").arg(user, SUDOERS_FILE);
	const QString prompt =
		"The stream runs over Wi-Fi, and AWDL (AirDrop, AirPlay, Handoff) takes the Wi-Fi radio "
		"off its channel for up to 100 ms at a time, which makes the picture stutter. "
		"Allow chiaki to switch AWDL off while a stream runs? This installs a rule that lets "
		"only the commands 'ifconfig awdl0 down' and 'ifconfig awdl0 up' run without a password.";
	const QString script = QString("do shell script \"%1\" with prompt \"%2\" with administrator privileges").arg(command, prompt);

	process = new QProcess(this);
	connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
		const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
		const bool ok = status == QProcess::NormalExit && code == 0;
		if(ok)
			qCInfo(awdlGuard) << "Rule installed in" << SUDOERS_FILE;
		Finished(ok);
		if(ok)
			return;
		gave_up = true;
		watch_timer.stop();
		if(err.contains("-128")) // userCanceledErr
		{
			qCInfo(awdlGuard) << "Password dialog dismissed: AWDL is left alone";
			if(on_declined)
				on_declined();
		}
		else
			qCWarning(awdlGuard) << "Installing the AWDL rule failed:" << err;
	});
	process->start("/usr/bin/osascript", {"-e", script});
}
