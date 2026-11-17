// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "ort_api.h"

#include "../../options.h"

#include <libaegisub/path.h>

#include <cstdlib>
#include <mutex>
#include <string>

#include "onnxruntime_c_api.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ocr {
namespace onnx {
namespace {

struct RuntimeState {
	OrtApi const *api = nullptr;
	std::string path;
	std::string diagnostic;
};

RuntimeState &State() {
	static RuntimeState state;
	return state;
}

#ifdef _WIN32
typedef OrtApiBase const *(ORT_API_CALL *GetApiBaseFn)(void);

void Load(RuntimeState &state, agi::fs::path const &dll) {
	std::wstring const wide = dll.wstring();
	state.path = dll.string();

	HMODULE module = ::LoadLibraryExW(wide.c_str(), nullptr,
	                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
	                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!module) {
		// Older loaders reject the search flags; the DLL's directory has to be
		// added to the search path by hand.
		std::wstring const directory = dll.parent_path().wstring();
		::SetDllDirectoryW(directory.c_str());
		module = ::LoadLibraryW(wide.c_str());
		::SetDllDirectoryW(nullptr);
	}
	if (!module) {
		state.diagnostic = "ONNX Runtime is not installed: failed to load " + state.path;
		return;
	}

	GetApiBaseFn get_api_base = reinterpret_cast<GetApiBaseFn>(
		::GetProcAddress(module, "OrtGetApiBase"));
	if (!get_api_base) {
		state.diagnostic = "The installed onnxruntime.dll does not export OrtGetApiBase.";
		return;
	}

	OrtApiBase const *base = get_api_base();
	if (!base) {
		state.diagnostic = "onnxruntime.dll returned no API table.";
		return;
	}

	state.api = base->GetApi(ORT_API_VERSION);
	if (!state.api) {
		state.diagnostic = "The installed onnxruntime.dll is too old for this build "
		                   "(needs ONNX Runtime API version " +
		                   std::to_string(ORT_API_VERSION) + ").";
		return;
	}
	state.diagnostic.clear();
}
#else
void Load(RuntimeState &state, agi::fs::path const &dll) {
	state.path = dll.string();
	state.diagnostic = "The in-process ONNX OCR backend is only available on Windows builds.";
}
#endif

} // namespace

agi::fs::path AssetDir() {
	if (char const *override_dir = std::getenv("AEGISUB_OCR_ASSETS")) {
		if (*override_dir)
			return agi::fs::path(override_dir);
	}
	if (!config::path)
		return {};
	try {
		return config::path->Decode("?data/ocr/onnx");
	}
	catch (...) {
		return {};
	}
}

OrtApi const *Api(std::string &diagnostic) {
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);

	RuntimeState &state = State();
	if (!state.api && state.diagnostic.empty()) {
		agi::fs::path const directory = AssetDir();
		if (directory.empty())
			state.diagnostic = "The ONNX OCR runtime directory could not be resolved.";
		else
			Load(state, directory / "onnxruntime.dll");
	}

	diagnostic = state.diagnostic;
	return state.api;
}

std::string RuntimePath() {
	return State().path;
}

} // namespace onnx
} // namespace ocr
