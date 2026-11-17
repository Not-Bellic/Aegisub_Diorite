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

#pragma once

#include "ocr_process.h"
#include "ocr_result.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <string>

namespace ocr {

/// Which stages of the pipeline a call should run.
enum class OcrMode {
	/// Detection and recognition: text and boxes are both returned.
	Full,
	/// Detection only: boxes are returned, text is stripped. Much cheaper, and
	/// enough for boundary confirmation.
	DetectOnly,
	/// Recognition only: the whole image is treated as one text line and the
	/// detector is skipped. The caller already knows where the line is (a scan
	/// row crop), and the detector would only spend time finding it again.
	RecOnly,
};

/// Pixels the caller already has in memory, so an engine that works in memory
/// does not need a PNG round-trip. RGB, three bytes per pixel, row-major and
/// tightly packed. The pointer must stay valid for the duration of the call.
struct OCRImage {
	int width = 0;
	int height = 0;
	unsigned char const *rgb = nullptr;

	bool Valid() const { return width > 0 && height > 0 && rgb != nullptr; }
};

/// A single OCR engine behind a uniform interface. Implementations may run a
/// persistent child process (PaddleOCR-json), a one-shot process per image, or
/// an in-process session (ONNX Runtime).
///
/// Implementations are not thread-safe: all calls must come from one thread.
class OcrBackend {
public:
	virtual ~OcrBackend() = default;

	/// Load models and start the engine. Fills `diagnostic` and returns false
	/// when no OCR runtime is usable at all; Run() must not be called then.
	/// A backend may return true while running in a slower fallback mode.
	virtual bool Start(std::string& diagnostic) = 0;
	virtual void Stop() = 0;
	/// True while a reusable engine (child process, session) is loaded.
	virtual bool IsRunning() const = 0;

	/// Short name of the engine that actually runs (`onnx`, `paddle`,
	/// `paddle-oneshot`), for logs and diagnostics. Empty until Start()
	/// succeeded.
	virtual std::string Name() const = 0;

	/// Run one image. The result carries its own diagnostic when the call
	/// fails, so callers never need a second availability query.
	virtual OCRResult Run(agi::fs::path const& image_path, OCROptions const& options,
	                      OcrMode mode, std::function<bool()> const& cancel = {}) = 0;

	/// True when RunImage() runs the pixels directly. A file-based engine
	/// returns false and callers keep handing it a PNG path instead.
	virtual bool AcceptsImagesInMemory() const { return false; }

	/// Run one image the caller already holds in memory. The default
	/// implementation fails with a diagnostic; only engines that report
	/// AcceptsImagesInMemory() implement it.
	virtual OCRResult RunImage(OCRImage const& image, OCROptions const& options,
	                           OcrMode mode, std::function<bool()> const& cancel = {});
};

/// The bundled PaddleOCR-json runtime: a persistent pipe process on Windows,
/// and a one-shot process per image elsewhere or when the persistent process
/// cannot be started. Never returns nullptr; read Start()'s diagnostic to
/// learn whether anything is usable.
std::unique_ptr<OcrBackend> MakePaddleBackend(OCRProcessConfig const& config);

/// The in-process ONNX Runtime backend (PP-OCRv6 small detector and
/// recognizer). Never returns nullptr: Start() reports a missing runtime,
/// model files or dictionary instead, so a build without the OCR bundle simply
/// keeps the feature disabled.
std::unique_ptr<OcrBackend> MakeOnnxBackend();

/// Which engine to use, from the `Tool/OCR/Backend` option.
enum class OcrBackendKind {
	/// ONNX Runtime when it can start, otherwise the bundled Paddle runtime.
	Auto,
	/// The in-process ONNX Runtime engine only.
	Onnx,
	/// The bundled PaddleOCR-json runtime only.
	Paddle,
};

/// The configured engine kind. `Auto` when the option is unset or holds an
/// unknown value (including before the options are loaded).
OcrBackendKind ConfiguredBackendKind();

/// Stable lower-case name of `kind` (`auto`, `onnx`, `paddle`), for the option
/// value and log lines.
char const *BackendKindId(OcrBackendKind kind);

/// Parse `auto`, `onnx` or `paddle`, case-insensitively. Returns false and
/// leaves `out` untouched when the text is not one of them.
bool ParseBackendKind(std::string const& text, OcrBackendKind& out);

/// Create an engine of `kind`. Never returns nullptr; Start() reports a missing
/// or broken runtime through its diagnostic. In `Auto` mode the Paddle engine
/// is started only after the ONNX one failed, and the reason is logged.
std::unique_ptr<OcrBackend> MakeBackend(OcrBackendKind kind, OCRProcessConfig const& config);

/// True when the configured engine looks installed. Only checks the runtime
/// files (no DLL load, no model load), so it is cheap enough for menu
/// validators, which run on every menu update.
bool IsAvailable();

/// Why the configured engine is unavailable, or empty when it is. Combines the
/// ONNX and Paddle reasons in `Auto` mode.
std::string AvailabilityDiagnostic();

/// Run one image on a process-wide engine shared by the single-image features
/// (Image2Text and the OCR dialog). The engine is started on first use and kept
/// loaded for the rest of the session, so repeated recognitions do not pay the
/// model load again; calls are serialized. Never throws: a failure comes back
/// in the result's diagnostic.
OCRResult RunShared(agi::fs::path const& image_path, OCROptions const& options,
                    OcrMode mode = OcrMode::Full);

} // namespace ocr
