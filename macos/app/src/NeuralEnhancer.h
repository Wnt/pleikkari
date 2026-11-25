// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The neural-rendering engine (macos/nr, libplknr.dylib) as the video pipeline's
// FrameEnhancer.
//
// The app does not link the engine: the library is opened at run time, so an app without
// it (or without a model) runs exactly as before, and the Swift and MLX code is only in
// the process when neural rendering is asked for. The engine takes one frame at a time and
// never queues; see macos/nr/Sources/CPlkNR/include/plknr.h.
//
// What the engine needs on disk:
//   the library   <app>/Contents/Frameworks/libplknr.dylib, with MLX's Metal library in
//                 <app>/Contents/Resources/mlx-swift_Cmlx.bundle (macos/app/build.sh puts
//                 both there when macos/nr/build.sh has been run), or --nr-library
//   a model       a .dlssmodel package made by macos/nr/tools/prepare-model.sh from an
//                 NVIDIA nvngx_dlssnr.dll. It is NVIDIA's and never part of the app:
//                 ~/Library/Application Support/Pleikkari/models, or --nr-models, --nr-model

#ifndef PLK_APP_NEURALENHANCER_H
#define PLK_APP_NEURALENHANCER_H

#include "Options.h"

#include <plk/FrameEnhancer.h>

#include <atomic>
#include <memory>
#include <string>

namespace plk::app {

/** The model package of a network, as prepare-model.sh names it. */
const char *nrModelFileName(NrNetwork network);
/** <app>/Contents/Frameworks/libplknr.dylib for the executable at this path. */
std::string defaultNrLibraryPath(const std::string &executable_path);
/** <home>/Library/Application Support/Pleikkari/models */
std::string defaultNrModelsDirectory(const std::string &home);

struct NrPaths
{
	std::string library;
	std::string model;
};
/** What a run with these options loads for `network`. */
NrPaths resolveNrPaths(const Options &o, NrNetwork network, const std::string &executable_path, const std::string &home);
/** The same for this process (its own executable, the user's home). */
NrPaths resolveNrPaths(const Options &o, NrNetwork network);
/** Empty when the library and the model are there; otherwise what is missing, for the log. */
std::string nrMissing(const NrPaths &paths);

class NeuralEnhancer : public video::FrameEnhancer
{
public:
	struct Config
	{
		NrPaths paths;
		NrLook look = NrLook::Standard;
	};
	/** Opens the library and loads the model (hundreds of milliseconds): not on the main thread. */
	static std::shared_ptr<NeuralEnhancer> load(const Config &config, std::string *error);
	/**
	 * Returns at once, on whatever thread drops the last reference (a render thread, a Metal
	 * callback): the engine itself, which waits for its frame in flight before it goes, is
	 * ended on a queue of its own.
	 */
	~NeuralEnhancer() override;
	/** Waits (up to `seconds`) until every engine let go of so far has ended. For the end of a run. */
	static void waitForRetired(double seconds = 5.0);

	bool submit(CVPixelBufferRef frame, uint64_t seq, bool reset, Done done) override;
	std::string describe() const override { return description_; }

	/** Takes effect with the next frame the engine takes. */
	void setLook(NrLook look);
	uint64_t errors() const;

private:
	struct Api;
	struct Core; // the engine and what its callbacks reach; outlives this object
	NeuralEnhancer(std::unique_ptr<Core> core, std::string description);
	static void onResult(void *context, const void *result);

	std::unique_ptr<Core> core_;
	const std::string description_;
};

} // namespace plk::app

#endif // PLK_APP_NEURALENHANCER_H
