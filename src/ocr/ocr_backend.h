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

	/// Run one image. The result carries its own diagnostic when the call
	/// fails, so callers never need a second availability query.
	virtual OCRResult Run(agi::fs::path const& image_path, OCROptions const& options,
	                      OcrMode mode, std::function<bool()> const& cancel = {}) = 0;
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

} // namespace ocr
