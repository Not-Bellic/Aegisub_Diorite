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

#include "ocr_backend.h"

#include "ocr_engine.h"
#include "ocr_process.h"

#include "../compat.h"

#include <libaegisub/fs.h>

#include <utility>

namespace ocr {
namespace {

/// Wraps the bundled PaddleOCR-json runtime. Prefers the persistent pipe
/// process; falls back to the one-shot CLI engine when the process is not
/// available (other platforms, a missing runtime, or a process that exited).
class PaddleBackend final : public OcrBackend {
	OCRProcessConfig config;
	OCRProcess process;
	bool process_started = false;

public:
	explicit PaddleBackend(OCRProcessConfig const& cfg) : config(cfg) {}

	bool Start(std::string& diagnostic) override {
		Stop();

		agi::fs::path executable;
		agi::fs::path models_dir;
		agi::fs::path config_path;
		if (!OCRProcess::FindRuntime(executable, models_dir, config_path, diagnostic))
			return false;

		// The engine reads det/rec/cls and the detector input cap from its
		// command line, so they are fixed for the lifetime of the process.
		if (process.Start(executable, models_dir, config_path, diagnostic, config)) {
			process_started = true;
			diagnostic.clear();
			return true;
		}

		// Starting the pipe mode is not fatal: the one-shot engine may still
		// work and Run() will use it. Keep the reason out of the way.
		diagnostic.clear();
		return true;
	}

	void Stop() override {
		process.Stop();
		process_started = false;
	}

	bool IsRunning() const override {
		return process_started && process.IsRunning();
	}

	OCRResult Run(agi::fs::path const& image_path, OCROptions const& options,
	              OcrMode mode, std::function<bool()> const& cancel) override {
		if (process_started && process.IsRunning())
			return process.RunImage(image_path, options, mode == OcrMode::DetectOnly, cancel);

		// One-shot fallback. Re-check the bundle first so a missing or
		// incomplete runtime returns the same diagnostic the pipe mode would,
		// instead of an opaque "no text" result.
		OCREngine engine;
		if (mode == OcrMode::DetectOnly) {
			auto diagnostic = engine.GetDetectionDiagnostic(options);
			if (!diagnostic.IsEmpty())
				return Failed(from_wx(diagnostic));
			return engine.DetectTextRegions(image_path, options);
		}

		auto diagnostic = engine.GetDiagnostic(options);
		if (!diagnostic.IsEmpty())
			return Failed(from_wx(diagnostic));
		return engine.RecognizeImage(image_path, options);
	}

private:
	static OCRResult Failed(std::string diagnostic) {
		OCRResult result;
		result.diagnostic = std::move(diagnostic);
		return result;
	}
};

} // namespace

std::unique_ptr<OcrBackend> MakePaddleBackend(OCRProcessConfig const& config) {
	return std::unique_ptr<OcrBackend>(new PaddleBackend(config));
}

} // namespace ocr
