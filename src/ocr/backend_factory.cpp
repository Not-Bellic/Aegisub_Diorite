// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "ocr_backend.h"

#include "ocr_process.h"
#include "onnx/ort_api.h"

#include "../options.h"

#include <libaegisub/log.h>

#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

namespace ocr {
namespace {

/// Tries the in-process ONNX engine first and falls back to the bundled
/// PaddleOCR-json runtime when it cannot start (runtime or models not
/// installed, a Windows build older than the bundle, a non-Windows build).
class AutoBackend final : public OcrBackend {
	OCRProcessConfig config;
	std::unique_ptr<OcrBackend> active;

public:
	explicit AutoBackend(OCRProcessConfig const &cfg) : config(cfg) {}

	bool Start(std::string &diagnostic) override {
		Stop();
		diagnostic.clear();

		std::string onnx_diagnostic;
		auto onnx = MakeOnnxBackend();
		if (onnx->Start(onnx_diagnostic)) {
			active = std::move(onnx);
			return true;
		}

		// Keep the reason: it is the only pointer to which files are missing.
		std::string paddle_diagnostic;
		auto paddle = MakePaddleBackend(config);
		if (paddle->Start(paddle_diagnostic)) {
			LOG_W("ocr/backend") << "ONNX Runtime is unavailable, using PaddleOCR-json: "
			                     << onnx_diagnostic;
			active = std::move(paddle);
			return true;
		}

		diagnostic = "No OCR engine could be started.\n\nONNX Runtime:\n" + onnx_diagnostic +
			"\n\nPaddleOCR-json:\n" + paddle_diagnostic;
		return false;
	}

	void Stop() override {
		if (active)
			active->Stop();
		active.reset();
	}

	bool IsRunning() const override {
		return active && active->IsRunning();
	}

	std::string Name() const override {
		return active ? active->Name() : std::string();
	}

	OCRResult Run(agi::fs::path const &image_path, OCROptions const &options, OcrMode mode,
	              std::function<bool()> const &cancel) override {
		if (!active) {
			OCRResult result;
			result.diagnostic = "The OCR engine has not been started.";
			return result;
		}
		return active->Run(image_path, options, mode, cancel);
	}

	bool AcceptsImagesInMemory() const override {
		return active && active->AcceptsImagesInMemory();
	}

	OCRResult RunImage(OCRImage const &image, OCROptions const &options, OcrMode mode,
	                   std::function<bool()> const &cancel) override {
		if (!active) {
			OCRResult result;
			result.diagnostic = "The OCR engine has not been started.";
			return result;
		}
		return active->RunImage(image, options, mode, cancel);
	}
};

/// State behind RunShared(). Leaked on purpose: background tasks can still be
/// inside the engine when the process exits, and destroying it there would race
/// with them.
struct SharedBackendState {
	std::mutex mutex;
	std::unique_ptr<OcrBackend> backend;
	/// Why the engine could not be started, so later calls do not retry (and
	/// reload the DLL) on every image.
	std::string diagnostic;
};

SharedBackendState &SharedState() {
	static SharedBackendState *state = new SharedBackendState;
	return *state;
}

std::string PaddleUnavailableReason() {
	if (!config::path)
		return "The OCR runtime directory is not available yet.";

	agi::fs::path executable;
	agi::fs::path models_dir;
	agi::fs::path config_path;
	std::string diagnostic;
	if (OCRProcess::FindRuntime(executable, models_dir, config_path, diagnostic))
		return {};
	return diagnostic;
}

} // namespace

OCRResult OcrBackend::RunImage(OCRImage const &image, OCROptions const &, OcrMode,
                               std::function<bool()> const &) {
	OCRResult result;
	result.diagnostic = image.Valid()
		? "This OCR engine cannot read an image from memory."
		: "The OCR input image is empty.";
	return result;
}

char const *BackendKindId(OcrBackendKind kind) {
	switch (kind) {
	case OcrBackendKind::Onnx: return "onnx";
	case OcrBackendKind::Paddle: return "paddle";
	case OcrBackendKind::Auto: break;
	}
	return "auto";
}

bool ParseBackendKind(std::string const &text, OcrBackendKind &out) {
	std::string value;
	value.reserve(text.size());
	for (char c : text)
		value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

	if (value == "auto" || value.empty()) {
		out = OcrBackendKind::Auto;
		return true;
	}
	if (value == "onnx" || value == "onnxruntime") {
		out = OcrBackendKind::Onnx;
		return true;
	}
	if (value == "paddle" || value == "paddleocr" || value == "paddleocr-json") {
		out = OcrBackendKind::Paddle;
		return true;
	}
	return false;
}

OcrBackendKind ConfiguredBackendKind() {
	try {
		// The OCR dialog and the self-test can run before the options load.
		if (config::opt) {
			OcrBackendKind kind = OcrBackendKind::Auto;
			if (ParseBackendKind(OPT_GET("Tool/OCR/Backend")->GetString(), kind))
				return kind;
		}
	}
	catch (...) {
	}
	return OcrBackendKind::Auto;
}

std::unique_ptr<OcrBackend> MakeBackend(OcrBackendKind kind, OCRProcessConfig const &config) {
	switch (kind) {
	case OcrBackendKind::Onnx:
		return MakeOnnxBackend();
	case OcrBackendKind::Paddle:
		return MakePaddleBackend(config);
	case OcrBackendKind::Auto:
		break;
	}
	return std::unique_ptr<OcrBackend>(new AutoBackend(config));
}

bool IsAvailable() {
	OcrBackendKind const kind = ConfiguredBackendKind();

	if (kind != OcrBackendKind::Paddle) {
		std::string onnx_reason;
		if (onnx::AssetsPresent(onnx_reason))
			return true;
	}

	if (kind != OcrBackendKind::Onnx && PaddleUnavailableReason().empty())
		return true;

	return false;
}

std::string AvailabilityDiagnostic() {
	OcrBackendKind const kind = ConfiguredBackendKind();

	std::string onnx_reason;
	bool const onnx_present = onnx::AssetsPresent(onnx_reason);
	std::string const paddle_reason = PaddleUnavailableReason();
	bool const paddle_present = paddle_reason.empty();

	if (kind == OcrBackendKind::Onnx)
		return onnx_present ? std::string() : onnx_reason;
	if (kind == OcrBackendKind::Paddle)
		return paddle_present ? std::string() : paddle_reason;

	if (onnx_present || paddle_present)
		return {};

	return onnx_reason + "\n\n" + paddle_reason;
}

OCRResult RunShared(agi::fs::path const &image_path, OCROptions const &options, OcrMode mode) {
	SharedBackendState &state = SharedState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!state.backend && state.diagnostic.empty()) {
		OCRProcessConfig config;
		// Burned-in video subtitles are never rotated, so the 0/180 degree
		// classifier is dead weight for every caller of this helper.
		config.cls = false;

		auto backend = MakeBackend(ConfiguredBackendKind(), config);
		std::string diagnostic;
		auto const start = std::chrono::steady_clock::now();
		if (backend && backend->Start(diagnostic)) {
			LOG_I("ocr/backend") << "using " << backend->Name() << " (started in "
				<< std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - start).count() << " ms)";
			state.backend = std::move(backend);
		}
		else {
			state.diagnostic = diagnostic.empty()
				? std::string("The OCR engine could not be started.")
				: diagnostic;
			LOG_E("ocr/backend") << "no OCR engine: " << state.diagnostic;
		}
	}

	if (!state.backend) {
		OCRResult result;
		result.diagnostic = state.diagnostic;
		return result;
	}

	return state.backend->Run(image_path, options, mode);
}

} // namespace ocr
