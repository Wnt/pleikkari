// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// SessionHost lifecycles that need no console: never started, refused at start, refused
// by the peer, and stopped or destroyed while the session request hangs. The only peer is
// 127.0.0.1 (nothing listening, or a listener in this process that never answers).

#include "SessionHost.h"
#include "TestHarness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {

plk::ConnectTarget fakeTarget()
{
	plk::ConnectTarget t;
	t.host = "127.0.0.1";
	t.ps5 = true;
	for(size_t i = 0; i < t.regist_key.size(); i++)
		t.regist_key[i] = (char)('a' + i);
	for(size_t i = 0; i < t.morning.size(); i++)
		t.morning[i] = (uint8_t)(0x50 + i);
	return t;
}

plk::StreamSettings quietSettings()
{
	plk::StreamSettings s;
	// PLK_TEST_VERBOSE=1 shows the library's log of these sessions.
	s.log_mask = plktest::envFlag("PLK_TEST_VERBOSE") ? CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE : CHIAKI_LOG_ERROR;
	return s;
}

class RecordingDelegate : public plk::SessionDelegate
{
public:
	void onConnected(const ChiakiConnectedEvent &) override { connected++; }
	void onQuit(ChiakiQuitReason reason, const char *) override
	{
		quit_reason = reason;
		quit_on_main = pthread_equal(pthread_self(), main_thread) != 0;
		if(host)
			shutdown_from_callback = host->shutdown() ? 1 : 0;
		quits++;
	}
	void onLog(ChiakiLogLevel level, const char *message) override
	{
		logs++;
		if(plktest::envFlag("PLK_TEST_VERBOSE"))
			plk::SessionDelegate::onLog(level, message);
	}

	pthread_t main_thread = pthread_self();
	plk::SessionHost *host = nullptr; // set to try shutdown() from inside onQuit
	std::atomic<int> connected{0}, quits{0}, logs{0};
	std::atomic<int> shutdown_from_callback{-1};
	std::atomic<ChiakiQuitReason> quit_reason{CHIAKI_QUIT_REASON_NONE};
	std::atomic<bool> quit_on_main{false};
};

/** A TCP listener on 127.0.0.1:port that accepts and then never says anything. */
class SilentListener
{
public:
	bool start(uint16_t port)
	{
		fd_ = socket(AF_INET, SOCK_STREAM, 0);
		if(fd_ < 0)
			return false;
		int one = 1;
		setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if(bind(fd_, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd_, 4) != 0)
		{
			close(fd_);
			fd_ = -1;
			return false;
		}
		return true;
	}
	~SilentListener()
	{
		if(fd_ >= 0)
			close(fd_);
	}

private:
	int fd_ = -1; // connections complete in the backlog; nobody reads them
};

bool portIsFree(uint16_t port)
{
	SilentListener probe;
	return probe.start(port);
}

} // namespace

PLK_TEST(session_never_started)
{
	RecordingDelegate d;
	{
		plk::SessionHost host(&d, nullptr, nullptr);
		CHECK(host.state() == plk::SessionHost::State::Idle);
		ChiakiControllerState cs;
		chiaki_controller_state_set_idle(&cs);
		CHECK(!host.setControllerState(cs));
		CHECK(!host.requestKeyframe());
		CHECK(!host.setLoginPin("1234"));
		host.stop();
		CHECK(!host.waitForQuit(10ms));
		CHECK(host.shutdown());
		CHECK(host.shutdown());
	}
	CHECK(d.quits == 0);
}

PLK_TEST(session_empty_host)
{
	RecordingDelegate d;
	plk::SessionHost host(&d, nullptr, nullptr);
	plk::ConnectTarget t = fakeTarget();
	t.host.clear();
	CHECK(host.start(t, quietSettings()) == CHIAKI_ERR_PARSE_ADDR);
	CHECK(host.state() == plk::SessionHost::State::Idle);
	CHECK(host.shutdown());
	CHECK(d.quits == 0);
}

PLK_TEST(session_connection_refused)
{
	if(!portIsFree(9295))
		SKIP("something on this machine listens on 127.0.0.1:9295");
	RecordingDelegate d;
	plk::SessionHost host(&d, nullptr, nullptr);
	REQUIRE(host.start(fakeTarget(), quietSettings()) == CHIAKI_ERR_SUCCESS);
	CHECK(host.start(fakeTarget(), quietSettings()) != CHIAKI_ERR_SUCCESS); // one session at a time
	CHECK(host.waitForQuit(10s));
	CHECK(host.state() == plk::SessionHost::State::Quit);
	CHECK_MSG(chiaki_quit_reason_is_error(host.quitReason()), "quit reason %s", chiaki_quit_reason_string(host.quitReason()));
	plktest::note("quit reason with nothing listening: %s", chiaki_quit_reason_string(host.quitReason()));
	CHECK(d.quits == 1);
	CHECK(!d.quit_on_main); // raised on the library's session thread
	CHECK(d.connected == 0);
	CHECK(host.shutdown());
	CHECK(host.state() == plk::SessionHost::State::Idle);
	ChiakiControllerState cs;
	chiaki_controller_state_set_idle(&cs);
	CHECK(!host.setControllerState(cs));
	// and it can start again after a shutdown
	REQUIRE(host.start(fakeTarget(), quietSettings()) == CHIAKI_ERR_SUCCESS);
	CHECK(host.waitForQuit(10s));
	CHECK(host.shutdown());
	CHECK(d.quits == 2);
}

PLK_TEST(session_shutdown_inside_callback_is_refused)
{
	if(!portIsFree(9295))
		SKIP("something on this machine listens on 127.0.0.1:9295");
	RecordingDelegate d;
	plk::SessionHost host(&d, nullptr, nullptr);
	d.host = &host;
	REQUIRE(host.start(fakeTarget(), quietSettings()) == CHIAKI_ERR_SUCCESS);
	CHECK(host.waitForQuit(10s));
	CHECK(d.shutdown_from_callback == 0); // refused, and no deadlock
	CHECK(host.shutdown());
}

PLK_TEST(session_stop_while_connecting)
{
	SilentListener listener;
	if(!listener.start(9295))
		SKIP("cannot listen on 127.0.0.1:9295");
	RecordingDelegate d;
	plk::SessionHost host(&d, nullptr, nullptr);
	REQUIRE(host.start(fakeTarget(), quietSettings()) == CHIAKI_ERR_SUCCESS);
	std::this_thread::sleep_for(300ms);
	CHECK(host.state() == plk::SessionHost::State::Connecting);
	CHECK(!host.waitForQuit(1ms));
	// Before the stream: the state is kept for later, an IDR request has nowhere to go.
	ChiakiControllerState cs;
	chiaki_controller_state_set_idle(&cs);
	cs.buttons = CHIAKI_CONTROLLER_BUTTON_CROSS;
	CHECK(host.setControllerState(cs));
	CHECK(!host.requestKeyframe());
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(host.shutdown());
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	plktest::note("shutdown while the session request hung took %.1f ms", ms);
	CHECK_MSG(ms < 1000.0, "shutdown took %.0f ms", ms);
	CHECK(d.quits == 1);
	CHECK(d.connected == 0);
	CHECK(host.state() == plk::SessionHost::State::Idle);
}

PLK_TEST(session_destroy_while_connecting)
{
	SilentListener listener;
	if(!listener.start(9295))
		SKIP("cannot listen on 127.0.0.1:9295");
	RecordingDelegate d;
	auto host = std::make_unique<plk::SessionHost>(&d, nullptr, nullptr);
	REQUIRE(host->start(fakeTarget(), quietSettings()) == CHIAKI_ERR_SUCCESS);
	std::this_thread::sleep_for(100ms);
	const auto t0 = std::chrono::steady_clock::now();
	host.reset();
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	CHECK_MSG(ms < 1000.0, "destruction took %.0f ms", ms);
	CHECK(d.quits == 1);
}

PLK_TEST(session_stop_from_other_thread)
{
	SilentListener listener;
	if(!listener.start(9295))
		SKIP("cannot listen on 127.0.0.1:9295");
	RecordingDelegate d;
	plk::SessionHost host(&d, nullptr, nullptr);
	REQUIRE(host.start(fakeTarget(), quietSettings()) == CHIAKI_ERR_SUCCESS);
	std::thread stopper([&] {
		std::this_thread::sleep_for(100ms);
		host.stop();
		host.stop(); // idempotent
	});
	CHECK(host.waitForQuit(3s));
	stopper.join();
	CHECK(host.quitReason() == CHIAKI_QUIT_REASON_STOPPED);
	CHECK(host.shutdown());
}
