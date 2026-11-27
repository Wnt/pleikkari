// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "NeuralEnhancer.h"

#include "Log.h"

// Types only: every function is reached through dlsym, so nothing here refers to the library.
#define PLKNR_TYPES_ONLY
#include <plknr.h>

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace plk::app {

namespace {

bool exists(const std::string &path, bool directory)
{
	struct stat st;
	if(stat(path.c_str(), &st) != 0)
		return false;
	return directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}

std::string parentOf(const std::string &path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? std::string(".") : slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string executablePath()
{
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buf(size + 1, 0);
	if(_NSGetExecutablePath(buf.data(), &size) != 0)
		return std::string();
	char resolved[PATH_MAX];
	return realpath(buf.data(), resolved) ? std::string(resolved) : std::string(buf.data());
}

plk_nr_controls controlsFor(NrLook look)
{
	plk_nr_controls c = {};
	c.intensity = 1.0f;
	c.detail_strength = 1.0f;
	c.colour_strength = 1.0f;
	c.temporal = 1;
	switch(look)
	{
		case NrLook::Standard: c.profile = PLK_NR_PROFILE_STANDARD; break;
		case NrLook::Natural: c.profile = PLK_NR_PROFILE_NATURAL; break;
		case NrLook::Cinematic: c.profile = PLK_NR_PROFILE_CINEMATIC; break;
		case NrLook::Neutral: c.profile = PLK_NR_PROFILE_NEUTRAL; break;
	}
	return c;
}

} // namespace

// The library's entry points. A library stays loaded once it is: Swift code cannot be
// unloaded, and switching between the two networks loads the same library again.
struct NeuralEnhancer::Api
{
	uint32_t (*abi_version)(void) = nullptr;
	plk_nr_engine *(*create)(const plk_nr_config *, char *, size_t) = nullptr;
	const char *(*describe)(const plk_nr_engine *) = nullptr;
	int (*submit)(plk_nr_engine *, CVPixelBufferRef, uint64_t, int, plk_nr_callback, void *) = nullptr;
	void (*set_controls)(plk_nr_engine *, const plk_nr_controls *) = nullptr;
	void (*destroy)(plk_nr_engine *) = nullptr;
	plk_fg_engine *(*fg_create)(const char *, int, char *, size_t) = nullptr;
	int (*fg_interpolate)(plk_fg_engine *, CVPixelBufferRef, CVPixelBufferRef, int, uint64_t, plk_nr_callback, void *) = nullptr;
	void (*fg_destroy)(plk_fg_engine *) = nullptr;

	// An M5 or later has a Neural Accelerator in every GPU core. MLX sends the network's
	// large matrix products there by itself; MLX-DLSS's feed-forward kernels go there only
	// when asked (MLXDLSS_TENSOR_OPS_FFN, off upstream until someone had measured it on such
	// a chip). Measured on an M5 Max, 2026-11-26, alternating runs: a result of the HT
	// network at 1080p in 53.4 ms instead of 58, of the small one at 720p in 15.1 instead
	// of 16.1, and the same frames byte for byte. Set before the engine reads it; a value
	// in the environment already (0 to switch it off) is left alone.
	static void preferNeuralAccelerators()
	{
		char brand[128] = {};
		size_t size = sizeof(brand) - 1;
		if(sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) != 0)
			return;
		int generation = 0;
		if(sscanf(brand, "Apple M%d", &generation) != 1 || generation < 5)
			return;
		if(setenv("MLXDLSS_TENSOR_OPS_FFN", "1", 0) == 0 && strcmp(getenv("MLXDLSS_TENSOR_OPS_FFN"), "1") == 0)
			PLK_LOGI("Neural rendering: feed-forward kernels on the GPU's Neural Accelerators (%s)", brand);
	}

	static const Api *open(const std::string &path, std::string *error)
	{
		static std::mutex mutex;
		static std::map<std::string, std::unique_ptr<Api>> loaded;
		std::lock_guard<std::mutex> lk(mutex);
		auto found = loaded.find(path);
		if(found != loaded.end())
			return found->second.get();
		preferNeuralAccelerators();
		void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if(!handle)
		{
			const char *why = dlerror();
			*error = std::string("cannot load ") + path + ": " + (why ? why : "unknown error");
			return nullptr;
		}
		auto api = std::make_unique<Api>();
		api->abi_version = (decltype(api->abi_version))dlsym(handle, "plk_nr_abi_version");
		api->create = (decltype(api->create))dlsym(handle, "plk_nr_create");
		api->describe = (decltype(api->describe))dlsym(handle, "plk_nr_describe");
		api->submit = (decltype(api->submit))dlsym(handle, "plk_nr_submit");
		api->set_controls = (decltype(api->set_controls))dlsym(handle, "plk_nr_set_controls");
		api->destroy = (decltype(api->destroy))dlsym(handle, "plk_nr_destroy");
		api->fg_create = (decltype(api->fg_create))dlsym(handle, "plk_fg_create");
		api->fg_interpolate = (decltype(api->fg_interpolate))dlsym(handle, "plk_fg_interpolate");
		api->fg_destroy = (decltype(api->fg_destroy))dlsym(handle, "plk_fg_destroy");
		if(!api->abi_version || !api->create || !api->describe || !api->submit || !api->set_controls || !api->destroy
				|| !api->fg_create || !api->fg_interpolate || !api->fg_destroy)
		{
			*error = path + " is not a neural rendering engine (an entry point is missing)";
			return nullptr;
		}
		const uint32_t version = api->abi_version();
		if(version != PLK_NR_ABI_VERSION)
		{
			*error = path + " has interface version " + std::to_string(version) + ", this app wants "
					+ std::to_string(PLK_NR_ABI_VERSION) + ": rebuild it with macos/nr/build.sh";
			return nullptr;
		}
		return (loaded[path] = std::move(api)).get();
	}
};

// What a frame in flight still needs after the NeuralEnhancer object is gone. Ending it
// waits for that frame (plk_nr_destroy), which is why it is not done where the last
// reference happens to be dropped.
struct NeuralEnhancer::Core
{
	const Api *api = nullptr;
	plk_nr_engine *engine = nullptr;
	std::atomic<uint64_t> errors{0};

	~Core() { api->destroy(engine); }
};

const char *nrModelFileName(NrNetwork network)
{
	return network == NrNetwork::HT ? "NeuralRendering.dlssmodel" : "NeuralRendering-Small.dlssmodel";
}

std::string defaultNrLibraryPath(const std::string &executable_path)
{
	// <app>/Contents/MacOS/Pleikkari -> <app>/Contents/Frameworks/libplknr.dylib
	return parentOf(parentOf(executable_path)) + "/Frameworks/libplknr.dylib";
}

std::string defaultNrModelsDirectory(const std::string &home)
{
	return home + "/Library/Application Support/Pleikkari/models";
}

NrPaths resolveNrPaths(const Options &o, NrNetwork network, const std::string &executable_path, const std::string &home)
{
	NrPaths paths;
	paths.library = o.nr_library.empty() ? defaultNrLibraryPath(executable_path) : o.nr_library;
	if(!o.nr_model.empty())
		paths.model = o.nr_model;
	else
		paths.model = (o.nr_models.empty() ? defaultNrModelsDirectory(home) : o.nr_models) + "/" + nrModelFileName(network);
	return paths;
}

NrPaths resolveNrPaths(const Options &o, NrNetwork network)
{
	const char *home = getenv("HOME");
	return resolveNrPaths(o, network, executablePath(), home ? home : "");
}

std::string nrMissing(const NrPaths &paths)
{
	if(!exists(paths.library, false))
		return "no engine at " + paths.library + " (macos/nr/build.sh builds it)";
	if(!exists(paths.model + "/manifest.json", false))
		return "no model at " + paths.model + " (macos/nr/tools/prepare-model.sh makes it)";
	return std::string();
}

std::shared_ptr<NeuralEnhancer> NeuralEnhancer::load(const Config &config, std::string *error)
{
	std::string why;
	const Api *api = Api::open(config.paths.library, &why);
	if(!api)
	{
		if(error)
			*error = why;
		return nullptr;
	}
	plk_nr_config c = {};
	c.struct_size = (uint32_t)sizeof(c);
	c.model_path = config.paths.model.c_str();
	c.half_precision = 1;
	c.controls = controlsFor(config.look);
	char message[512] = "";
	plk_nr_engine *engine = api->create(&c, message, sizeof(message));
	if(!engine)
	{
		if(error)
			*error = std::string(message[0] ? message : "the engine refused the model") + " (" + config.paths.model + ")";
		return nullptr;
	}
	const char *description = api->describe(engine);
	auto core = std::make_unique<Core>();
	core->api = api;
	core->engine = engine;
	return std::shared_ptr<NeuralEnhancer>(new NeuralEnhancer(std::move(core), description ? description : "neural rendering"));
}

namespace {

dispatch_group_t retiredGroup()
{
	static dispatch_group_t group = dispatch_group_create();
	return group;
}

} // namespace

NeuralEnhancer::NeuralEnhancer(std::unique_ptr<Core> core, std::string description)
	: core_(std::move(core)), description_(std::move(description))
{
}

NeuralEnhancer::~NeuralEnhancer()
{
	dispatch_group_async_f(retiredGroup(), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), core_.release(),
			[](void *core) { delete (Core *)core; });
}

void NeuralEnhancer::waitForRetired(double seconds)
{
	dispatch_group_wait(retiredGroup(), dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * 1e9)));
}

uint64_t NeuralEnhancer::errors() const
{
	return core_->errors.load();
}

namespace {

struct Pending
{
	video::FrameEnhancer::Done done;
	// The core's counter. It outlives the call: the core's end waits for the frame in flight.
	std::atomic<uint64_t> *errors;
};

} // namespace

void NeuralEnhancer::onResult(void *context, const void *result)
{
	std::unique_ptr<Pending> pending((Pending *)context);
	const auto *r = (const plk_nr_result *)result;
	if(!r->output)
	{
		// The first failure and then every hundredth: a model that fails does so every frame.
		const uint64_t n = pending->errors->fetch_add(1) + 1;
		if(n == 1 || n % 100 == 0)
			PLK_LOGW("Neural rendering: frame %llu failed (%llu so far): %s", (unsigned long long)r->seq,
					(unsigned long long)n, r->error ? r->error : "no picture");
	}
	pending->done(r->output, r->total_ms);
}

bool NeuralEnhancer::submit(CVPixelBufferRef frame, uint64_t seq, bool reset, Done done)
{
	auto *pending = new Pending{ std::move(done), &core_->errors };
	const plk_nr_callback callback = [](void *context, const plk_nr_result *result) { onResult(context, result); };
	if(core_->api->submit(core_->engine, frame, seq, reset ? 1 : 0, callback, pending))
		return true;
	delete pending;
	return false;
}

void NeuralEnhancer::setLook(NrLook look)
{
	const plk_nr_controls c = controlsFor(look);
	core_->api->set_controls(core_->engine, &c);
}

std::string frameGenerationWeightsPath(const Options &o)
{
	const char *home = getenv("HOME");
	return (o.nr_models.empty() ? defaultNrModelsDirectory(home ? home : "") : o.nr_models) + "/framegen.safetensors";
}

std::string frameGenerationMissing(const std::string &library, const std::string &weights)
{
	if(!exists(library, false))
		return "no engine at " + library + " (macos/nr/build.sh builds it)";
	if(!exists(weights, false))
		return "no frame generation weights at " + weights + " (mlxdlss-weights extract-fg makes them from nvngx_dlssg.dll)";
	return std::string();
}

std::shared_ptr<FrameGenerator> FrameGenerator::load(const std::string &library, const std::string &weights, std::string *error)
{
	std::string why;
	const NeuralEnhancer::Api *api = NeuralEnhancer::Api::open(library, &why);
	char message[512] = "";
	plk_fg_engine *engine = api ? api->fg_create(weights.c_str(), 1, message, sizeof(message)) : nullptr;
	if(!engine)
	{
		if(error)
			*error = api ? std::string(message) : why;
		return nullptr;
	}
	return std::shared_ptr<FrameGenerator>(new FrameGenerator(api, engine));
}

FrameGenerator::~FrameGenerator()
{
	struct Gone
	{
		const NeuralEnhancer::Api *api;
		plk_fg_engine *engine;
	};
	dispatch_group_async_f(retiredGroup(), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
			new Gone{ (const NeuralEnhancer::Api *)api_, (plk_fg_engine *)engine_ }, [](void *context) {
				std::unique_ptr<Gone> gone((Gone *)context);
				gone->api->fg_destroy(gone->engine);
			});
}

bool FrameGenerator::interpolate(CVPixelBufferRef previous, CVPixelBufferRef current, int count, uint64_t seq, Done done)
{
	auto *pending = new Done(std::move(done));
	const plk_nr_callback callback = [](void *context, const plk_nr_result *result) {
		Done *finished = (Done *)context;
		const bool failed = !result->output || result->count == 0;
		if(failed)
			PLK_LOGW("Frame generation: picture %llu failed: %s", (unsigned long long)result->seq,
					result->error ? result->error : "no picture");
		(*finished)(failed ? nullptr : result->output, (int)result->index, failed ? 0 : (int)result->count, result->total_ms);
		// The last call for this pair.
		if(failed || result->index + 1 >= result->count)
			delete finished;
	};
	if(((const NeuralEnhancer::Api *)api_)->fg_interpolate((plk_fg_engine *)engine_, previous, current, count, seq, callback, pending))
		return true;
	delete pending;
	return false;
}

} // namespace plk::app
