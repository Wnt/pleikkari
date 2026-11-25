// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-host-probe: a headless SessionHost client.
//
//   plk-host-probe --list [--plist PATH]
//       Registered consoles from the Qt app's settings: nickname and console type only.
//
//   plk-host-probe --path ADDRESS
//       How a stream to ADDRESS would leave this machine and what the AWDL guard would do
//       about it. Sends nothing and changes nothing.
//
//   plk-host-probe --connect NICKNAME ADDRESS [options]
//       A real LAN session with no video output: counts frames, bytes and losses, and
//       optionally writes every video sample to a PLKVDMP1 dump.
//       --seconds N        stop after N seconds (default 10; 0 = until the session ends or Ctrl-C)
//       --dump FILE        write video samples to FILE (PLKVDMP1)
//       --resolution R     360p | 540p | 720p | 1080p (default 1080p)
//       --fps F            30 | 60 (default 60)
//       --bitrate KBPS     default: the resolution preset's
//       --codec C          h264 | h265 | h265_hdr (default h265)
//       --no-dualsense     do not enable DualSense features
//       --no-reorder       disable the library's 16 ms video packet reordering wait
//       --audio            play the game audio through AudioOutput (default output device!)
//       --pin PIN          login PIN, if the console asks for one
//       --quiet            library log: warnings and errors only
//       --plist PATH       settings plist (default ~/Library/Preferences/com.chiaki.Chiaki.plist)
//
// Never prints anything from a registration besides nickname and console type.

#include "AudioOutput.h"
#include "AwdlGuard.h"
#include "Registration.h"
#include "SessionHost.h"
#include "VideoDump.h"

#include <chiaki/time.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_interrupted{false};

void onSignal(int)
{
	g_interrupted.store(true);
}

void usage()
{
	fprintf(stderr,
		"usage: plk-host-probe --list [--plist PATH]\n"
		"       plk-host-probe --path ADDRESS\n"
		"       plk-host-probe --connect NICKNAME ADDRESS [--seconds N] [--dump FILE]\n"
		"                      [--resolution 360p|540p|720p|1080p] [--fps 30|60] [--bitrate KBPS]\n"
		"                      [--codec h264|h265|h265_hdr] [--no-dualsense] [--no-reorder]\n"
		"                      [--audio] [--pin PIN] [--quiet] [--plist PATH]\n");
}

/** The way a stream to address leaves the machine, as AwdlGuard::acquire() sees it. */
int showPath(const std::string &address)
{
	const std::string out = plk::AwdlGuard::outgoingInterface(address);
	const std::string physical = plk::AwdlGuard::physicalInterface(out);
	const bool wifi = !physical.empty() && plk::AwdlGuard::isWifi(physical);
	const int awdl = plk::AwdlGuard::awdlIsUp();
	printf("outgoing interface: %s\n", out.empty() ? "unknown" : out.c_str());
	if(physical.empty())
		printf("hardware interface: none found\n");
	else
		printf("hardware interface: %s (%s)%s\n", physical.c_str(), wifi ? "Wi-Fi" : "not Wi-Fi",
				physical == out ? "" : ", which carries the tunnel");
	printf("awdl0:              %s\n", awdl < 0 ? "no such interface" : awdl ? "up" : "down");
	printf("a stream would:     %s\n", wifi && awdl >= 0 ? "keep awdl0 down while it lasts" : "leave AWDL alone");
	return 0;
}

/** Does this Annex-B access unit contain an IDR picture? */
bool containsIdr(const uint8_t *buf, size_t size, ChiakiCodec codec)
{
	const bool hevc = chiaki_codec_is_h265(codec);
	for(size_t i = 0; i + 3 < size; i++)
	{
		if(buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 1)
			continue;
		const uint8_t h = buf[i + 3];
		if(hevc)
		{
			const unsigned type = (h >> 1) & 0x3f;
			if(type == 19 || type == 20) // IDR_W_RADL, IDR_N_LP
				return true;
		}
		else if((h & 0x1f) == 5)
			return true;
		i += 2;
	}
	return false;
}

class ProbeVideoSink : public plk::VideoSink
{
public:
	ProbeVideoSink(ChiakiCodec codec, plk::AsyncVideoDumpWriter *dump) : codec_(codec), dump_(dump) {}

	bool onVideoSample(uint8_t *buf, size_t buf_size, uint16_t frame_index, uint64_t frame_ready_time_us,
			int32_t frames_lost, bool recovered) override
	{
		if(dump_)
			dump_->write(buf, buf_size, frame_index, frame_ready_time_us, frames_lost, recovered);
		if(frame_ready_time_us == 0)
		{
			config_samples.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if(first_frame_us.load(std::memory_order_relaxed) == 0)
			first_frame_us.store(frame_ready_time_us, std::memory_order_relaxed);
		frames.fetch_add(1, std::memory_order_relaxed);
		bytes.fetch_add(buf_size, std::memory_order_relaxed);
		if(frames_lost > 0)
			lost.fetch_add((uint64_t)frames_lost, std::memory_order_relaxed);
		if(recovered)
			recovered_frames.fetch_add(1, std::memory_order_relaxed);
		if(containsIdr(buf, buf_size, codec_))
			idr_frames.fetch_add(1, std::memory_order_relaxed);
		if(last_ready_us_)
		{
			const uint64_t gap = frame_ready_time_us - last_ready_us_;
			gap_sum_us.fetch_add(gap, std::memory_order_relaxed);
			gap_count.fetch_add(1, std::memory_order_relaxed);
			if(gap > gap_max_us.load(std::memory_order_relaxed))
				gap_max_us.store(gap, std::memory_order_relaxed);
			if(gap > 25000)
				gaps_over_25ms.fetch_add(1, std::memory_order_relaxed);
		}
		last_ready_us_ = frame_ready_time_us;
		return true;
	}

	std::atomic<uint64_t> frames{0}, config_samples{0}, bytes{0}, lost{0}, recovered_frames{0}, idr_frames{0};
	std::atomic<uint64_t> first_frame_us{0}, gap_sum_us{0}, gap_count{0}, gap_max_us{0}, gaps_over_25ms{0};

private:
	const ChiakiCodec codec_;
	plk::AsyncVideoDumpWriter *const dump_;
	uint64_t last_ready_us_ = 0; // takion thread only
};

class CountingPcmSink : public plk::PcmSink
{
public:
	void onAudioFormat(uint32_t ch, uint32_t r) override
	{
		channels.store(ch);
		rate.store(r);
	}
	void onPcm(const int16_t *, size_t n) override { frames.fetch_add(n, std::memory_order_relaxed); }

	std::atomic<uint32_t> channels{0}, rate{0};
	std::atomic<uint64_t> frames{0};
};

class ProbeDelegate : public plk::SessionDelegate
{
public:
	explicit ProbeDelegate(bool quiet) : quiet_(quiet) {}

	void onConnected(const ChiakiConnectedEvent &info) override
	{
		connected_us.store(chiaki_time_now_monotonic_us());
		printf("connected: %s %s:%u, mtu %u, rtt %.2f ms%s\n", info.relay ? "relay" : "lan", info.peer_host,
			(unsigned)info.peer_port, (unsigned)info.mtu_in, info.rtt_us / 1000.0, info.measured ? "" : " (not measured)");
		fflush(stdout);
	}
	void onQuit(ChiakiQuitReason reason, const char *reason_str) override
	{
		printf("quit: %s%s%s\n", chiaki_quit_reason_string(reason), reason_str ? ": " : "", reason_str ? reason_str : "");
		fflush(stdout);
	}
	void onLoginPinRequest(bool incorrect) override
	{
		if(!pin.empty() && !incorrect && host)
		{
			printf("console asked for the login PIN; sending --pin\n");
			host->setLoginPin(pin);
		}
		else
		{
			printf("console asked for the login PIN%s; stopping\n", incorrect ? " again (incorrect)" : "");
			if(host)
				host->stop();
		}
		fflush(stdout);
	}
	void onStreamStats(const ChiakiStreamStatsEvent &s) override
	{
		if(quiet_)
			return;
		printf("stats: %llu frames/%llu ms, lost total %llu, jitter %.2f ms, takion pkts %llu, units missing %llu, "
			"fec recovered %llu, unrecoverable %llu, silence %llu ms\n",
			(unsigned long long)s.stream_frames, (unsigned long long)s.interval_ms,
			(unsigned long long)s.video_frames_lost, s.video_packet_jitter_us / 1000.0,
			(unsigned long long)s.takion_packets_received, (unsigned long long)s.takion_partial_frame_units_missing,
			(unsigned long long)s.fec_recovered_packets, (unsigned long long)s.unrecoverable_packets,
			(unsigned long long)s.takion_max_receive_gap_ms);
		fflush(stdout);
	}
	void onRumble(const ChiakiRumbleEvent &) override { rumble.fetch_add(1, std::memory_order_relaxed); }
	void onTriggerEffects(const ChiakiTriggerEffectsEvent &) override { triggers.fetch_add(1, std::memory_order_relaxed); }
	void onHapticsFrame(const uint8_t *, size_t size) override
	{
		haptics_frames.fetch_add(1, std::memory_order_relaxed);
		haptics_bytes.fetch_add(size, std::memory_order_relaxed);
	}
	void onVideoFecFailure(const ChiakiVideoFecFailureEvent &) override { fec_failures.fetch_add(1, std::memory_order_relaxed); }
	void onCantDisplay(bool cant) override
	{
		printf("console: stream %s\n", cant ? "cannot be displayed (protected content)" : "can be displayed again");
		fflush(stdout);
	}

	plk::SessionHost *host = nullptr;
	std::string pin;
	std::atomic<uint64_t> connected_us{0};
	std::atomic<uint64_t> rumble{0}, triggers{0}, haptics_frames{0}, haptics_bytes{0}, fec_failures{0};

private:
	const bool quiet_;
};

int listConsoles(const std::string &plist)
{
	std::vector<plk::RegisteredConsole> consoles;
	std::string error;
	if(!plk::loadRegisteredConsoles(plist, consoles, &error))
	{
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if(consoles.empty())
		printf("no registered consoles\n");
	for(const auto &c : consoles)
		printf("%s\t%s%s\n", c.nickname.c_str(), c.consoleTypeName(), c.usable() ? "" : "\t(incomplete registration)");
	return 0;
}

bool parseResolution(const std::string &s, ChiakiVideoResolutionPreset &out)
{
	if(s == "360p") out = CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
	else if(s == "540p") out = CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
	else if(s == "720p") out = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
	else if(s == "1080p") out = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	else return false;
	return true;
}

bool parseCodec(const std::string &s, ChiakiCodec &out)
{
	if(s == "h264") out = CHIAKI_CODEC_H264;
	else if(s == "h265") out = CHIAKI_CODEC_H265;
	else if(s == "h265_hdr" || s == "h265hdr") out = CHIAKI_CODEC_H265_HDR;
	else return false;
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	std::string plist = plk::qtSettingsPlistPath();
	bool list = false, connect = false, quiet = false, play_audio = false;
	std::string nickname, address, dump_path, pin;
	double seconds = 10.0;
	plk::StreamSettings settings;
	settings.suspend_awdl = false; // a probe changes nothing on the machine
	settings.stream_diagnostics = true;

	for(int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next = [&](const char *name) -> std::string {
			if(i + 1 >= argc)
			{
				fprintf(stderr, "%s needs a value\n", name);
				exit(2);
			}
			return argv[++i];
		};
		if(a == "--list") list = true;
		else if(a == "--path") return showPath(next("--path"));
		else if(a == "--connect")
		{
			connect = true;
			nickname = next("--connect");
			if(i + 1 >= argc)
			{
				usage();
				return 2;
			}
			address = argv[++i];
		}
		else if(a == "--seconds") seconds = atof(next("--seconds").c_str());
		else if(a == "--dump") dump_path = next("--dump");
		else if(a == "--plist") plist = next("--plist");
		else if(a == "--resolution")
		{
			if(!parseResolution(next("--resolution"), settings.resolution))
			{
				usage();
				return 2;
			}
		}
		else if(a == "--fps")
		{
			int fps = atoi(next("--fps").c_str());
			if(fps != 30 && fps != 60)
			{
				usage();
				return 2;
			}
			settings.fps = fps == 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
		}
		else if(a == "--bitrate") settings.bitrate_kbps = (unsigned)atoi(next("--bitrate").c_str());
		else if(a == "--codec")
		{
			if(!parseCodec(next("--codec"), settings.codec))
			{
				usage();
				return 2;
			}
		}
		else if(a == "--no-dualsense") settings.enable_dualsense = false;
		else if(a == "--no-reorder") settings.disable_video_packet_reordering = true;
		else if(a == "--audio") play_audio = true;
		else if(a == "--pin") pin = next("--pin");
		else if(a == "--quiet") quiet = true;
		else
		{
			usage();
			return 2;
		}
	}

	if(list == connect)
	{
		usage();
		return 2;
	}
	if(list)
		return listConsoles(plist);

	std::vector<plk::RegisteredConsole> consoles;
	std::string error;
	if(!plk::loadRegisteredConsoles(plist, consoles, &error))
	{
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	const plk::RegisteredConsole *console = plk::findConsole(consoles, nickname);
	if(!console)
	{
		fprintf(stderr, "no registered console named \"%s\" (see --list)\n", nickname.c_str());
		return 1;
	}
	if(!console->usable())
	{
		fprintf(stderr, "the registration of \"%s\" is incomplete\n", nickname.c_str());
		return 1;
	}
	if(quiet)
		settings.log_mask = CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR;
	const ChiakiCodec codec = console->isPS5() ? settings.codec : CHIAKI_CODEC_H264;

	std::unique_ptr<plk::AsyncVideoDumpWriter> dump;
	if(!dump_path.empty())
	{
		dump.reset(new plk::AsyncVideoDumpWriter);
		if(!dump->open(dump_path, &error))
		{
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
	}

	ProbeVideoSink video(codec, dump.get());
	CountingPcmSink counting_pcm;
	std::unique_ptr<plk::AudioOutput> audio_out;
	plk::PcmSink *pcm = &counting_pcm;
	if(play_audio)
	{
		audio_out.reset(new plk::AudioOutput());
		pcm = audio_out.get();
	}
	ProbeDelegate delegate(quiet);
	delegate.pin = pin;
	std::unique_ptr<plk::SessionHost> host(new plk::SessionHost(&delegate, &video, pcm));
	delegate.host = host.get();

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	plk::ConnectTarget target = plk::ConnectTarget::fromRegistration(*console, address);
	printf("connecting to %s (%s) at %s\n", console->nickname.c_str(), console->consoleTypeName(), address.c_str());
	fflush(stdout);
	const uint64_t start_us = chiaki_time_now_monotonic_us();
	ChiakiErrorCode err = host->start(target, settings);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "session start failed: %s\n", chiaki_error_string(err));
		return 1;
	}

	uint64_t prev_frames = 0, prev_bytes = 0;
	const uint64_t deadline_us = seconds > 0 ? start_us + (uint64_t)(seconds * 1e6) : UINT64_MAX;
	while(!g_interrupted.load())
	{
		if(host->waitForQuit(std::chrono::milliseconds(1000)))
			break;
		if(chiaki_time_now_monotonic_us() >= deadline_us)
			break;
		const uint64_t f = video.frames.load(), b = video.bytes.load();
		if(!quiet && host->state() == plk::SessionHost::State::Streaming)
		{
			printf("video: %llu frames/s, %.2f Mbit/s, lost %llu, recovered %llu\n", (unsigned long long)(f - prev_frames),
				(b - prev_bytes) * 8 / 1e6, (unsigned long long)video.lost.load(), (unsigned long long)video.recovered_frames.load());
			fflush(stdout);
		}
		prev_frames = f;
		prev_bytes = b;
	}
	const uint64_t end_us = chiaki_time_now_monotonic_us();
	host->stop();
	host->shutdown();
	const ChiakiQuitReason reason = host->quitReason();
	bool dump_ok = true;
	if(dump)
		dump_ok = dump->close();

	const uint64_t frames = video.frames.load();
	const uint64_t connected_us = delegate.connected_us.load();
	const double streamed_s = connected_us ? (end_us - connected_us) / 1e6 : 0.0;
	printf("\nsummary\n");
	printf("  quit reason          %s\n", chiaki_quit_reason_string(reason));
	if(connected_us)
		printf("  start -> connected   %.1f ms\n", (connected_us - start_us) / 1000.0);
	if(video.first_frame_us.load())
		printf("  start -> first frame %.1f ms\n", (video.first_frame_us.load() - start_us) / 1000.0);
	printf("  video frames         %llu (%llu IDR, %llu config samples)\n", (unsigned long long)frames,
		(unsigned long long)video.idr_frames.load(), (unsigned long long)video.config_samples.load());
	if(streamed_s > 0)
		printf("  average              %.2f fps, %.2f Mbit/s\n", frames / streamed_s, video.bytes.load() * 8 / 1e6 / streamed_s);
	printf("  frames lost          %llu (reported with delivered frames), recovered refs %llu, FEC failures %llu\n",
		(unsigned long long)video.lost.load(), (unsigned long long)video.recovered_frames.load(),
		(unsigned long long)delegate.fec_failures.load());
	if(video.gap_count.load())
		printf("  frame-ready interval avg %.2f ms, max %.2f ms, %llu over 25 ms\n",
			video.gap_sum_us.load() / 1000.0 / video.gap_count.load(), video.gap_max_us.load() / 1000.0,
			(unsigned long long)video.gaps_over_25ms.load());
	if(audio_out)
	{
		plk::AudioOutputStats a = audio_out->stats();
		printf("  audio                %llu frames in, %u ch @ %u Hz, underruns %llu, overflows %llu, backlog cuts %llu\n",
			(unsigned long long)a.pcm_frames_in, a.channels, a.rate, (unsigned long long)a.playout.underruns,
			(unsigned long long)a.overflow_events, (unsigned long long)a.playout.hard_drops);
		printf("  audio latency        ring avg %.2f ms (target margin %.2f ms), device %.2f ms, drift %.0f ppm\n",
			a.playout.avg_fill_ms, a.playout.margin_target_ms, a.latency.total_ms, a.playout.drift_ppm);
	}
	else
		printf("  audio                %llu frames, %u ch @ %u Hz (not played)\n", (unsigned long long)counting_pcm.frames.load(),
			counting_pcm.channels.load(), counting_pcm.rate.load());
	printf("  haptics              %llu frames, %llu bytes; rumble events %llu, trigger effects %llu\n",
		(unsigned long long)delegate.haptics_frames.load(), (unsigned long long)delegate.haptics_bytes.load(),
		(unsigned long long)delegate.rumble.load(), (unsigned long long)delegate.triggers.load());
	if(dump)
		printf("  dump                 %s%s, %llu samples dropped\n", dump_path.c_str(), dump_ok ? "" : " (WRITE ERROR)",
			(unsigned long long)dump->dropped());

	return chiaki_quit_reason_is_error(reason) && reason != CHIAKI_QUIT_REASON_NONE ? 1 : 0;
}
