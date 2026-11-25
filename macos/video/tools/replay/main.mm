// plk-video-replay: replays a recorded video stream through VideoPipeline into
// a small window and prints latency statistics.
//
// Runs as an accessory app (no Dock icon, never activates, never takes focus);
// the window is borderless, click-through and sits in a screen corner.
// Run with --help for flags.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "AnnexB.h"
#include "Dump.h"
#include "plk/VideoPipeline.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mach/mach_time.h>
#include <mutex>
#include <pthread.h>
#include <random>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

using namespace plk;

namespace {

struct Unit {
	bool config = false;
	std::vector<uint8_t> data;
	uint16_t frameIndex = 0;
	bool recovered = false;
	bool keyframe = false;
	int32_t framesLost = 0;
	double offsetUs = 0;  // recorded cadence: time of this frame relative to the first
};

struct Phase {
	video::PacingMode pacing;
	video::Scaler scaler;
};

struct Options {
	std::string input;
	std::string codec = "auto";
	double fps = 60.0;
	bool recordedCadence = true;  // for dumps; raw streams always use --fps
	bool cadenceSet = false;
	long frames = 0;
	double duration = 10.0;
	bool loop = false;
	video::PacingMode pacing = video::PacingMode::Immediate;
	video::Scaler scaler = video::Scaler::Bilinear;
	std::vector<Phase> phases;
	double phaseSeconds = 3.0;
	double warmup = 0.5;
	int winW = 640, winH = 360;
	bool back = false;
	int offW = 0, offH = 0;
	uint32_t latchUs = 0;
	int drawables = 3;
	bool syncDecode = false;
	bool compressed = false;
	bool hdrLinear = false;
	float hdrBrightness = 1.0f;
	video::HdrToneMap hdrToneMap = video::HdrToneMap::App;
	float hdrHeadroom = 0.0f;
	long invalidateAt = -1;
	std::vector<std::pair<int, int>> readback;
	bool json = false;
	bool allowLong = false;
	std::string writeDump;
	std::string cadenceModel = "measured";
	float refreshHz = 0;
};

void usage()
{
	fprintf(stderr,
		"usage: plk-video-replay [flags] INPUT\n"
		"  INPUT                 .h265/.hevc/.265, .h264/.264 (raw Annex-B) or .plkv (PLKVDMP1 dump)\n"
		"  --codec hevc|h264     override codec detection\n"
		"  --fps N               fixed playback rate (default 60; raw streams always use it)\n"
		"  --cadence recorded|fixed  dumps: replay frame_ready_us deltas (default) or --fps\n"
		"  --frames N            stop after N frames\n"
		"  --duration S          stop after S seconds (default 10; capped at 15 without --allow-long)\n"
		"  --loop                restart the input at its end\n"
		"  --pacing immediate|immediate-vsync|displaylink   (default immediate)\n"
		"  --scaler bilinear|bicubic|lanczos|metalfx   (default bilinear)\n"
		"  --phases P:S,P:S,...  run pacing:scaler phases back to back, stats per phase\n"
		"  --phase-seconds S     length of each phase (default 3)\n"
		"  --warmup S            discard stats for the first S seconds of each phase (default 0.5)\n"
		"  --latch-us N          displaylink: wait up to targetTimestamp-N us for a late frame\n"
		"  --refresh-hz N        displaylink frame rate (default: screen maximum)\n"
		"  --drawables 2|3       CAMetalLayer.maximumDrawableCount (default 3)\n"
		"  --sync-decode         synchronous VTDecompressionSessionDecodeFrame\n"
		"  --compressed-output   request lossless-compressed decoder output (&8v0, 8-bit only)\n"
		"  --hdr-output pq|linear  HDR drawable: PQ code values (default) or extended-linear half float\n"
		"  --hdr-brightness PCT  linear light of an HDR picture times PCT/100 (default 100)\n"
		"  --hdr-tone-map app|system  who fits an HDR picture to the display (default app: the shader)\n"
		"  --hdr-headroom H      app tone mapping: the display's headroom, SDR white = 1 (default: none,\n"
		"                        nothing is rolled off)\n"
		"  --size WxH            window size in points (default 640x360)\n"
		"  --back                order the window behind other windows\n"
		"  --offscreen WxH       render offscreen at WxH pixels (GPU timing; no presentation)\n"
		"  --readback X,Y        offscreen: print the rendered pixel at X,Y at the end (repeatable)\n"
		"  --invalidate-at N     invalidate the decoder session before frame N (recovery test)\n"
		"  --json                also print each phase's stats as one JSON line\n"
		"  --write-dump OUT      convert INPUT to a PLKVDMP1 dump with a synthetic cadence and exit\n"
		"  --cadence-model measured|fixed  --write-dump cadence (default: measured PS5 jitter)\n");
}

bool parseSize(const char *s, int *w, int *h) { return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0; }

bool endsWith(const std::string &s, const char *suf)
{
	size_t n = strlen(suf);
	return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}

bool readFile(const std::string &path, std::vector<uint8_t> &out)
{
	std::ifstream f(path, std::ios::binary);
	if(!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

double monotonicUs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

double machUs()
{
	static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1000.0;
}

void waitUntilUs(double us)
{
	static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
	mach_wait_until((uint64_t)(us * 1000.0 * tb.denom / tb.numer));
}

bool loadUnits(const Options &o, std::vector<Unit> &units, std::string &desc)
{
	if(endsWith(o.input, ".plkv"))
	{
		std::vector<replay::DumpRecord> recs;
		std::string err;
		if(!replay::readDump(o.input, recs, &err))
		{
			fprintf(stderr, "error: %s\n", err.c_str());
			return false;
		}
		if(!err.empty())
			fprintf(stderr, "warning: %s (using %zu complete records)\n", err.c_str(), recs.size());
		uint64_t first = 0;
		size_t configs = 0, recovered = 0;
		int64_t lost = 0;
		for(auto &r : recs)
		{
			Unit u;
			u.config = r.isConfig();
			u.frameIndex = r.frameIndex;
			u.recovered = r.recovered();
			u.framesLost = r.framesLost;
			u.data = std::move(r.payload);
			if(!u.config)
			{
				if(!first)
					first = r.frameReadyUs;
				u.offsetUs = r.frameReadyUs >= first ? (double)(r.frameReadyUs - first) : 0;
				lost += r.framesLost;
				recovered += u.recovered;
			}
			else
				configs++;
			units.push_back(std::move(u));
		}
		char b[256];
		snprintf(b, sizeof b, "dump: %zu records (%zu config, %zu frames, %zu recovered, %lld lost)", units.size(), configs,
				units.size() - configs, recovered, (long long)lost);
		desc = b;
		return true;
	}

	std::vector<uint8_t> raw;
	if(!readFile(o.input, raw))
	{
		fprintf(stderr, "error: cannot read %s\n", o.input.c_str());
		return false;
	}
	video::annexb::Codec codec;
	if(o.codec == "hevc") codec = video::annexb::Codec::HEVC;
	else if(o.codec == "h264") codec = video::annexb::Codec::H264;
	else if(endsWith(o.input, ".h264") || endsWith(o.input, ".264") || endsWith(o.input, ".avc")) codec = video::annexb::Codec::H264;
	else if(endsWith(o.input, ".h265") || endsWith(o.input, ".265") || endsWith(o.input, ".hevc")) codec = video::annexb::Codec::HEVC;
	else if(!video::annexb::detectCodec(raw.data(), raw.size(), &codec))
	{
		fprintf(stderr, "error: cannot detect codec of %s (use --codec)\n", o.input.c_str());
		return false;
	}
	auto aus = video::annexb::splitAccessUnits(codec, raw.data(), raw.size());
	uint16_t idx = 1;
	size_t frames = 0, keys = 0, configs = 0;
	for(auto &au : aus)
	{
		if(!au.config.empty())
		{
			Unit c;
			c.config = true;
			c.data = std::move(au.config);
			units.push_back(std::move(c));
			configs++;
		}
		if(au.frame.empty())
			continue;
		Unit u;
		u.data = std::move(au.frame);
		u.frameIndex = idx++;
		u.keyframe = au.keyframe;
		u.offsetUs = frames * 1e6 / o.fps;
		units.push_back(std::move(u));
		frames++;
		keys += au.keyframe;
	}
	char b[256];
	snprintf(b, sizeof b, "raw %s: %zu bytes, %zu access units (%zu keyframes), %zu config samples",
			codec == video::annexb::Codec::HEVC ? "HEVC" : "H.264", raw.size(), frames, keys, configs);
	desc = b;
	return true;
}

// Synthetic arrival cadence resembling the measured PS5 stream: 73 % of
// intervals 13-20 ms, the rest alternating about 10 ms / 23 ms.
int writeSyntheticDump(const Options &o, const std::vector<Unit> &units)
{
	std::mt19937 rng(12345);
	std::uniform_real_distribution<double> uni(0.0, 1.0);
	std::vector<replay::DumpRecord> recs;
	double t = 1e9;  // arbitrary CLOCK_MONOTONIC origin
	bool shortNext = true;
	bool first = true;
	for(const auto &u : units)
	{
		replay::DumpRecord r;
		r.frameIndex = u.frameIndex;
		r.payload = u.data;
		if(u.config)
		{
			r.flags = replay::kDumpFlagConfig;
			recs.push_back(std::move(r));
			continue;
		}
		if(!first)
		{
			double dt;
			if(o.cadenceModel == "fixed")
				dt = 1e6 / o.fps;
			else if(uni(rng) < 0.73)
				dt = 13000.0 + uni(rng) * 7000.0;
			else
			{
				dt = (shortNext ? 10000.0 : 23000.0) + (uni(rng) - 0.5) * 1000.0;
				shortNext = !shortNext;
			}
			t += dt;
		}
		first = false;
		r.frameReadyUs = (uint64_t)t;
		recs.push_back(std::move(r));
	}
	std::string err;
	if(!replay::writeDump(o.writeDump, recs, &err))
	{
		fprintf(stderr, "error: %s\n", err.c_str());
		return 1;
	}
	fprintf(stderr, "wrote %zu records to %s\n", recs.size(), o.writeDump.c_str());
	return 0;
}

void printDist(const char *name, const video::Distribution &d)
{
	if(!d.count)
		return;
	printf("  %-22s %7llu %8.3f %8.3f %8.3f %8.3f %8.3f\n", name, (unsigned long long)d.count, d.mean, d.p50, d.p95, d.p99, d.max);
}

void jsonDist(std::string &s, const char *name, const video::Distribution &d)
{
	char b[256];
	snprintf(b, sizeof b, "\"%s\":{\"n\":%llu,\"mean\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},", name,
			(unsigned long long)d.count, d.mean, d.p50, d.p95, d.p99, d.max);
	s += b;
}

void printStats(const video::Stats &s, const std::string &label, bool json)
{
	printf("== %s ==\n", label.c_str());
	printf("  stream: %s %dx%d %d-bit %s, decoder output %s, hardware decoder %s\n", s.codec.c_str(), s.width, s.height,
			s.bitDepth, s.hdr ? "HDR" : "SDR", s.pixelFormat.c_str(), s.hardwareDecoder ? "yes" : "NO");
	printf("  colour: %s%s%s\n", s.colorDescription.c_str(), s.hdr ? "; EDR metadata " : "", s.hdr ? s.hdrMetadata.c_str() : "");
	printf("  render: pacing %s, scaler %s, target %dx%d\n", s.pacing.c_str(), s.scaler.c_str(), s.targetWidth, s.targetHeight);
	printf("  frames: submitted %llu decoded %llu rendered %llu presented %llu | superseded %llu not-presented %llu "
	       "skipped-awaiting-idr %llu\n",
			(unsigned long long)s.framesSubmitted, (unsigned long long)s.framesDecoded, (unsigned long long)s.framesRendered,
			(unsigned long long)s.framesPresented, (unsigned long long)s.framesSuperseded,
			(unsigned long long)s.framesNotPresented, (unsigned long long)s.framesSkippedAwaitingIdr);
	printf("  events: configs %llu changes %llu sessions %llu recoveries %llu decode-errors %llu idr-requests %llu\n",
			(unsigned long long)s.configsSubmitted, (unsigned long long)s.configChanges, (unsigned long long)s.sessionsCreated,
			(unsigned long long)s.sessionRecoveries, (unsigned long long)s.decodeErrors, (unsigned long long)s.idrRequests);
	if(s.displayLinkTicks)
		printf("  displaylink: ticks %llu without-new-frame %llu latch-waits %llu latch-hits %llu\n",
				(unsigned long long)s.displayLinkTicks, (unsigned long long)s.ticksWithoutNewFrame,
				(unsigned long long)s.latchWaits, (unsigned long long)s.latchHits);
	printf("  %-22s %7s %8s %8s %8s %8s %8s   (ms)\n", "", "n", "mean", "p50", "p95", "p99", "max");
	printDist("submitFrame call", s.submitCallMs);
	printDist("decode", s.decodeMs);
	printDist("decoded->render", s.queueMs);
	printDist("render cpu", s.renderCpuMs);
	printDist("render gpu", s.renderGpuMs);
	printDist("submit->presented", s.submitToPresentedMs);
	printDist("ready->presented", s.readyToPresentedMs);
	printDist("decoded->presented", s.decodedToPresentedMs);
	printDist("submit interval", s.submitIntervalMs);
	printDist("present interval", s.presentIntervalMs);
	printDist("link cb->target pres", s.linkLeadMs);
	printDist("link cb->target ts", s.linkDeadlineMs);
	printDist("presented-target", s.presentVsTargetMs);
	if(json)
	{
		std::string j = "{";
		char b[1024];
		snprintf(b, sizeof b,
				"\"label\":\"%s\",\"codec\":\"%s\",\"width\":%d,\"height\":%d,\"bits\":%d,\"hdr\":%s,\"pixfmt\":\"%s\","
				"\"pacing\":\"%s\",\"scaler\":\"%s\",\"target\":\"%dx%d\",\"submitted\":%llu,\"decoded\":%llu,"
				"\"rendered\":%llu,\"presented\":%llu,\"superseded\":%llu,\"not_presented\":%llu,\"skipped_idr\":%llu,"
				"\"config_changes\":%llu,\"recoveries\":%llu,\"decode_errors\":%llu,\"idr_requests\":%llu,"
				"\"ticks\":%llu,\"ticks_no_frame\":%llu,",
				label.c_str(), s.codec.c_str(), s.width, s.height, s.bitDepth, s.hdr ? "true" : "false", s.pixelFormat.c_str(),
				s.pacing.c_str(), s.scaler.c_str(), s.targetWidth, s.targetHeight, (unsigned long long)s.framesSubmitted,
				(unsigned long long)s.framesDecoded, (unsigned long long)s.framesRendered, (unsigned long long)s.framesPresented,
				(unsigned long long)s.framesSuperseded, (unsigned long long)s.framesNotPresented,
				(unsigned long long)s.framesSkippedAwaitingIdr, (unsigned long long)s.configChanges,
				(unsigned long long)s.sessionRecoveries, (unsigned long long)s.decodeErrors, (unsigned long long)s.idrRequests,
				(unsigned long long)s.displayLinkTicks, (unsigned long long)s.ticksWithoutNewFrame);
		j += b;
		jsonDist(j, "submit_call", s.submitCallMs);
		jsonDist(j, "decode", s.decodeMs);
		jsonDist(j, "queue", s.queueMs);
		jsonDist(j, "render_cpu", s.renderCpuMs);
		jsonDist(j, "render_gpu", s.renderGpuMs);
		jsonDist(j, "submit_to_presented", s.submitToPresentedMs);
		jsonDist(j, "ready_to_presented", s.readyToPresentedMs);
		jsonDist(j, "present_interval", s.presentIntervalMs);
		jsonDist(j, "submit_interval", s.submitIntervalMs);
		jsonDist(j, "presented_minus_target", s.presentVsTargetMs);
		j.back() = '}';
		printf("JSON %s\n", j.c_str());
	}
	fflush(stdout);
}

void printScreenInfo(const char *when)
{
	NSScreen *s = NSScreen.mainScreen;
	printf("screen (%s): %s, %ld Hz max, EDR current %.2f potential %.2f reference %.2f\n", when,
			s.localizedName.UTF8String, (long)s.maximumFramesPerSecond, s.maximumExtendedDynamicRangeColorComponentValue,
			s.maximumPotentialExtendedDynamicRangeColorComponentValue, s.maximumReferenceExtendedDynamicRangeColorComponentValue);
	fflush(stdout);
}

}  // namespace

@interface PLKReplayApp : NSObject <NSApplicationDelegate>
@end

@implementation PLKReplayApp
{
	Options _o;
	std::vector<Unit> _units;
	NSWindow *_window;
	CAMetalLayer *_layer;
	std::unique_ptr<video::VideoPipeline> _pipeline;
	std::thread _feeder;
	std::atomic<bool> _stop;
	std::atomic<bool> _feederDone;
	std::atomic<long> _framesFed;
	std::atomic<int> _errorsPrinted;
	NSTimer *_timer;
	double _startUs;
	size_t _phase;
	double _phaseStartUs;
	bool _warmupDone;
	std::vector<Phase> _phases;
	id<NSObject> _activity;
}

- (instancetype)initWithOptions:(const Options &)o units:(std::vector<Unit> &&)units
{
	if((self = [super init]))
	{
		_o = o;
		_units = std::move(units);
		_stop = false;
		_feederDone = false;
		_framesFed = 0;
		_errorsPrinted = 0;
		_phases = o.phases;
		if(_phases.empty())
			_phases.push_back({o.pacing, o.scaler});
	}
	return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	// An accessory app that never activates is a prime App Nap candidate:
	// timers coalesce and threads are clamped, which wrecks latency numbers.
	// The streaming client needs the same activity while a stream runs.
	_activity = [NSProcessInfo.processInfo
		beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
		                  reason:@"video latency measurement"];
	NSScreen *screen = NSScreen.mainScreen;
	printScreenInfo("before");
	NSRect vf = screen.visibleFrame;
	NSRect frame = NSMakeRect(vf.origin.x + 8, vf.origin.y + 8, _o.winW, _o.winH);
	_window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless
	                                        backing:NSBackingStoreBuffered defer:NO];
	_window.ignoresMouseEvents = YES;
	_window.hasShadow = NO;
	_window.releasedWhenClosed = NO;
	_window.collectionBehavior = NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
	NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, _o.winW, _o.winH)];
	_layer = [CAMetalLayer layer];
	view.layer = _layer;
	view.wantsLayer = YES;
	_window.contentView = view;
	CGFloat scale = _window.backingScaleFactor;
	_layer.contentsScale = scale;
	_layer.frame = view.bounds;
	_layer.drawableSize = CGSizeMake(_o.winW * scale, _o.winH * scale);

	video::PipelineConfig cfg;
	cfg.codec = _o.codec == "hevc" ? video::Codec::HEVC : _o.codec == "h264" ? video::Codec::H264 : video::Codec::Auto;
	cfg.pacing = _phases[0].pacing;
	cfg.scaler = _phases[0].scaler;
	cfg.displayRefreshHz = _o.refreshHz > 0 ? _o.refreshHz : (float)screen.maximumFramesPerSecond;
	cfg.latchMarginUs = _o.latchUs;
	cfg.maxDrawables = _o.drawables;
	cfg.asyncDecode = !_o.syncDecode;
	cfg.compressedDecoderOutput = _o.compressed;
	cfg.hdrOutput = _o.hdrLinear ? video::HdrOutput::ExtendedLinear : video::HdrOutput::PQ;
	cfg.hdrBrightness = _o.hdrBrightness;
	cfg.hdrToneMap = _o.hdrToneMap;
	cfg.displayHeadroom = _o.hdrHeadroom;
	cfg.offscreenWidth = _o.offW;
	cfg.offscreenHeight = _o.offH;
	PLKReplayApp *__weak weakSelf = self;
	std::atomic<int> *printed = &_errorsPrinted;
	auto onError = [printed](const video::Error &e) {
		if(printed->fetch_add(1) < 8)
			fprintf(stderr, "pipeline error: kind %d status %d idr %d: %s\n", (int)e.kind, (int)e.status, e.requestIdr,
					e.message.c_str());
	};
	(void)weakSelf;
	std::string err;
	_pipeline = video::VideoPipeline::create((__bridge void *)_layer, cfg, onError, &err);
	if(!_pipeline)
	{
		fprintf(stderr, "error: %s\n", err.c_str());
		exit(1);
	}
	if(_o.back)
		[_window orderBack:nil];
	else
		[_window orderFrontRegardless];

	_startUs = machUs();
	_phase = 0;
	_phaseStartUs = _startUs;
	_warmupDone = false;
	_feeder = std::thread([self] { [self feed]; });
	_timer = [NSTimer scheduledTimerWithTimeInterval:0.02 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}

- (void)feed
{
	pthread_setname_np("plk.replay.feeder");
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	double base = machUs() + 20000.0;  // first frame 20 ms from now
	double loopOffset = 0;
	double lastOffset = 0;
	long fed = 0;
	bool useRecorded = endsWith(_o.input, ".plkv") && _o.recordedCadence;
	double interval = 1e6 / _o.fps;
	double fixedT = 0;
	double lateSum = 0, lateMax = 0, rebases = 0;
	for(;;)
	{
		for(size_t i = 0; i < _units.size(); i++)
		{
			if(_stop.load())
				goto done;
			const Unit &u = _units[i];
			if(u.config)
			{
				_pipeline->submitConfig(u.data.data(), u.data.size());
				continue;
			}
			double off;
			if(useRecorded)
			{
				// Cap pauses in the recording (menus, reconnects) at 250 ms.
				double d = u.offsetUs - lastOffset;
				if(d > 250000.0 || d < 0)
					loopOffset += (d < 0 ? 0 : d) - interval, d = interval;
				off = u.offsetUs + loopOffset;
				lastOffset = u.offsetUs;
			}
			else
			{
				off = fixedT;
				fixedT += interval;
			}
			// A stall (first session creation, a slow submit) must not turn
			// into a catch-up burst: re-base the schedule instead.
			if(machUs() > base + off + 30000.0)
			{
				base = machUs() - off;
				rebases++;
			}
			waitUntilUs(base + off);
			double late = machUs() - (base + off);
			lateSum += late;
			lateMax = std::max(lateMax, late);
			if(_o.invalidateAt >= 0 && fed == _o.invalidateAt)
			{
				fprintf(stderr, "invalidating decoder session before frame %ld\n", fed);
				_pipeline->debugInvalidateSession();
			}
			_pipeline->submitFrame(u.data.data(), u.data.size(), u.frameIndex, (uint64_t)monotonicUs(), u.recovered,
					u.framesLost);
			fed++;
			_framesFed.store(fed);
			if(_o.frames > 0 && fed >= _o.frames)
				goto done;
		}
		if(!_o.loop)
			break;
		if(useRecorded)
		{
			loopOffset += lastOffset + interval;
			lastOffset = 0;
		}
	}
done:
	if(fed)
		fprintf(stderr, "feeder: %ld frames, wake-up lateness mean %.2f ms max %.2f ms, %.0f schedule re-bases\n", fed,
				lateSum / fed / 1000.0, lateMax / 1000.0, rebases);
	_feederDone.store(true);
}

- (void)tick:(NSTimer *)t
{
	double now = machUs();
	double total = _o.phases.empty() ? _o.duration : _o.phaseSeconds * _phases.size();
	bool phaseEnd = !_o.phases.empty() && now - _phaseStartUs >= _o.phaseSeconds * 1e6;
	bool allEnd = _feederDone.load() || now - _startUs >= total * 1e6;
	if(!_warmupDone && now - _phaseStartUs >= _o.warmup * 1e6 && !allEnd)
	{
		_pipeline->resetStats();
		_warmupDone = true;
	}
	if(phaseEnd || allEnd)
	{
		if(allEnd)
		{
			_stop.store(true);
			if(_feeder.joinable())
				_feeder.join();
			usleep(150000);  // let the last frames reach the display
		}
		char label[256];
		snprintf(label, sizeof label, "phase %zu/%zu: pacing=%s scaler=%s, %.1f s (after %.1f s warm-up), %ld frames fed",
				_phase + 1, _phases.size(), video::toString(_phases[_phase].pacing), video::toString(_phases[_phase].scaler),
				(now - _phaseStartUs) / 1e6 - _o.warmup, _o.warmup, _framesFed.load());
		printScreenInfo("during");
		printf("window: %s\n", (_window.occlusionState & NSWindowOcclusionStateVisible) ? "visible" : "OCCLUDED");
		if(!_o.offW)
		{
			CFStringRef csName = _layer.colorspace ? CGColorSpaceCopyName(_layer.colorspace) : nullptr;
			const char *fmt = _layer.pixelFormat == MTLPixelFormatBGR10A2Unorm ? "BGR10A2Unorm"
				: _layer.pixelFormat == MTLPixelFormatRGBA16Float ? "RGBA16Float"
				: _layer.pixelFormat == MTLPixelFormatBGRA8Unorm ? "BGRA8Unorm" : "other";
			printf("layer: %s, colorspace %s, wantsEDR %d, EDRMetadata %s, displaySync %d, drawables %lu\n", fmt,
					csName ? [(__bridge NSString *)csName UTF8String] : "nil", _layer.wantsExtendedDynamicRangeContent,
					_layer.EDRMetadata ? "set" : "nil", _layer.displaySyncEnabled, (unsigned long)_layer.maximumDrawableCount);
			if(csName)
				CFRelease(csName);
		}
		printStats(_pipeline->stats(), label, _o.json);
		if(!allEnd && _phase + 1 < _phases.size())
		{
			_phase++;
			_phaseStartUs = now;
			_warmupDone = false;
			_pipeline->setPacingMode(_phases[_phase].pacing);
			_pipeline->setScaler(_phases[_phase].scaler);
			return;
		}
		[self finish];
	}
}

- (void)finish
{
	[_timer invalidate];
	_stop.store(true);
	if(_feeder.joinable())
		_feeder.join();
	for(auto &p : _o.readback)
	{
		float rgba[4];
		if(_pipeline->debugReadPixel(p.first, p.second, rgba))
			printf("readback %d,%d: R %.4f G %.4f B %.4f (10-bit %d %d %d, 8-bit %d %d %d)\n", p.first, p.second, rgba[0],
					rgba[1], rgba[2], (int)std::lround(rgba[0] * 1023), (int)std::lround(rgba[1] * 1023),
					(int)std::lround(rgba[2] * 1023), (int)std::lround(rgba[0] * 255), (int)std::lround(rgba[1] * 255),
					(int)std::lround(rgba[2] * 255));
		else
			printf("readback %d,%d: unavailable (offscreen mode only)\n", p.first, p.second);
	}
	double t0 = machUs();
	_pipeline.reset();
	printf("teardown: %.1f ms\n", (machUs() - t0) / 1000.0);
	fflush(stdout);
	[_window orderOut:nil];
	exit(0);
}

@end

int main(int argc, char **argv)
{
	@autoreleasepool
	{
		Options o;
		for(int i = 1; i < argc; i++)
		{
			std::string a = argv[i];
			auto next = [&]() -> const char * {
				if(i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
				return argv[++i];
			};
			if(a == "--help" || a == "-h") { usage(); return 0; }
			else if(a == "--codec") o.codec = next();
			else if(a == "--fps") o.fps = atof(next());
			else if(a == "--cadence") { std::string v = next(); o.recordedCadence = v != "fixed"; o.cadenceSet = true; }
			else if(a == "--frames") o.frames = atol(next());
			else if(a == "--duration") o.duration = atof(next());
			else if(a == "--loop") o.loop = true;
			else if(a == "--pacing") { if(!video::parsePacingMode(next(), &o.pacing)) { usage(); return 2; } }
			else if(a == "--scaler") { if(!video::parseScaler(next(), &o.scaler)) { usage(); return 2; } }
			else if(a == "--phases")
			{
				std::string v = next();
				size_t p = 0;
				while(p <= v.size())
				{
					size_t c = v.find(',', p);
					std::string item = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
					size_t colon = item.find(':');
					Phase ph;
					if(colon == std::string::npos || !video::parsePacingMode(item.substr(0, colon), &ph.pacing)
						|| !video::parseScaler(item.substr(colon + 1), &ph.scaler))
					{
						fprintf(stderr, "bad phase '%s'\n", item.c_str());
						return 2;
					}
					o.phases.push_back(ph);
					if(c == std::string::npos)
						break;
					p = c + 1;
				}
			}
			else if(a == "--phase-seconds") o.phaseSeconds = atof(next());
			else if(a == "--warmup") o.warmup = atof(next());
			else if(a == "--latch-us") o.latchUs = (uint32_t)atol(next());
			else if(a == "--refresh-hz") o.refreshHz = (float)atof(next());
			else if(a == "--drawables") o.drawables = atoi(next());
			else if(a == "--sync-decode") o.syncDecode = true;
			else if(a == "--compressed-output") o.compressed = true;
		else if(a == "--hdr-output") { std::string v = next(); if(v != "pq" && v != "linear") { usage(); return 2; } o.hdrLinear = v == "linear"; }
		else if(a == "--hdr-brightness") { o.hdrBrightness = (float)atof(next()) / 100.0f; if(!(o.hdrBrightness >= 0.05f && o.hdrBrightness <= 4.0f)) { usage(); return 2; } }
		else if(a == "--hdr-tone-map") { if(!video::parseHdrToneMap(next(), &o.hdrToneMap)) { usage(); return 2; } }
		else if(a == "--hdr-headroom") { o.hdrHeadroom = (float)atof(next()); if(!(o.hdrHeadroom >= 1.0f && o.hdrHeadroom <= 64.0f)) { usage(); return 2; } }
			else if(a == "--size") { if(!parseSize(next(), &o.winW, &o.winH)) { usage(); return 2; } }
			else if(a == "--back") o.back = true;
			else if(a == "--offscreen") { if(!parseSize(next(), &o.offW, &o.offH)) { usage(); return 2; } }
			else if(a == "--readback") { int x, y; if(sscanf(next(), "%d,%d", &x, &y) != 2) { usage(); return 2; } o.readback.push_back({x, y}); }
			else if(a == "--invalidate-at") o.invalidateAt = atol(next());
			else if(a == "--json") o.json = true;
			else if(a == "--allow-long") o.allowLong = true;
			else if(a == "--write-dump") o.writeDump = next();
			else if(a == "--cadence-model") o.cadenceModel = next();
			else if(!a.empty() && a[0] == '-') { fprintf(stderr, "unknown flag %s\n", a.c_str()); usage(); return 2; }
			else o.input = a;
		}
		if(o.input.empty()) { usage(); return 2; }
		double total = o.phases.empty() ? o.duration : o.phaseSeconds * o.phases.size();
		if(total > 15.0 && !o.allowLong)
		{
			fprintf(stderr, "run capped at 15 s (requested %.1f s); pass --allow-long to override\n", total);
			if(o.phases.empty()) o.duration = 15.0;
			else o.phaseSeconds = 15.0 / o.phases.size();
		}

		std::vector<Unit> units;
		std::string desc;
		if(!loadUnits(o, units, desc))
			return 1;
		printf("input: %s\n", desc.c_str());
		if(!o.writeDump.empty())
			return writeSyntheticDump(o, units);

		NSApplication *app = [NSApplication sharedApplication];
		[app setActivationPolicy:NSApplicationActivationPolicyAccessory];
		PLKReplayApp *delegate = [[PLKReplayApp alloc] initWithOptions:o units:std::move(units)];
		app.delegate = delegate;
		[app run];
	}
	return 0;
}
