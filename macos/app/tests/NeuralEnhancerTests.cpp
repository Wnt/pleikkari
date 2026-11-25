// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Where the engine and its models are looked for, and what a missing piece says. The
// engine itself is not built by this build: macos/nr has its own, and the replay with
// --nr on is its test with the app.

#include "NeuralEnhancer.h"
#include "TestHarness.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace plk;
using namespace plk::app;

PLK_TEST(neural_enhancer_paths)
{
	const std::string exe = "/Apps/Pleikkari.app/Contents/MacOS/Pleikkari";
	CHECK(defaultNrLibraryPath(exe) == "/Apps/Pleikkari.app/Contents/Frameworks/libplknr.dylib");
	CHECK(defaultNrModelsDirectory("/Users/x") == "/Users/x/Library/Application Support/Pleikkari/models");
	CHECK(std::string(nrModelFileName(NrNetwork::Small)) == "NeuralRendering-Small.dlssmodel");
	CHECK(std::string(nrModelFileName(NrNetwork::HT)) == "NeuralRendering.dlssmodel");

	Options o;
	NrPaths p = resolveNrPaths(o, NrNetwork::Small, exe, "/Users/x");
	CHECK(p.library == "/Apps/Pleikkari.app/Contents/Frameworks/libplknr.dylib");
	CHECK(p.model == "/Users/x/Library/Application Support/Pleikkari/models/NeuralRendering-Small.dlssmodel");
	CHECK(resolveNrPaths(o, NrNetwork::HT, exe, "/Users/x").model
			== "/Users/x/Library/Application Support/Pleikkari/models/NeuralRendering.dlssmodel");

	// The command line wins, piece by piece; one model overrides the choice of network.
	o.nr_library = "/build/libplknr.dylib";
	o.nr_models = "/models";
	p = resolveNrPaths(o, NrNetwork::HT, exe, "/Users/x");
	CHECK(p.library == "/build/libplknr.dylib");
	CHECK(p.model == "/models/NeuralRendering.dlssmodel");
	o.nr_model = "/elsewhere/Mine.dlssmodel";
	CHECK(resolveNrPaths(o, NrNetwork::Small, exe, "/Users/x").model == "/elsewhere/Mine.dlssmodel");
}

PLK_TEST(neural_enhancer_says_what_is_missing)
{
	char dir[] = "/tmp/plk-nr-test-XXXXXX";
	REQUIRE(mkdtemp(dir) != nullptr);
	const std::string root = dir;
	NrPaths p;
	p.library = root + "/libplknr.dylib";
	p.model = root + "/NeuralRendering-Small.dlssmodel";
	CHECK(nrMissing(p).find("no engine at " + p.library) == 0);

	// A file that is no library: there, but it does not load, and the error names it.
	FILE *f = fopen(p.library.c_str(), "w");
	REQUIRE(f != nullptr);
	fputs("not a library\n", f);
	fclose(f);
	CHECK(nrMissing(p).find("no model at " + p.model) == 0);
	REQUIRE(mkdir(p.model.c_str(), 0700) == 0);
	CHECK(nrMissing(p).find("no model at ") == 0); // a directory without a manifest is no model
	f = fopen((p.model + "/manifest.json").c_str(), "w");
	REQUIRE(f != nullptr);
	fputs("{}\n", f);
	fclose(f);
	CHECK(nrMissing(p).empty());

	NeuralEnhancer::Config config;
	config.paths = p;
	std::string error;
	CHECK(NeuralEnhancer::load(config, &error) == nullptr);
	CHECK(error.find("cannot load " + p.library) == 0);

	unlink((p.model + "/manifest.json").c_str());
	rmdir(p.model.c_str());
	unlink(p.library.c_str());
	rmdir(dir);
}
