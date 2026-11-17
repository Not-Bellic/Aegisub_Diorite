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

#pragma once

#include "ocr_backend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/// Shared state for an OCR engine pre-warmed in the background while a hard
/// subtitle scan dialog is open. The dialog owns it through a shared_ptr so the
/// warm-up task can finish after the window is gone: no joins, no dangling
/// pointers, and a runtime that is not installed cannot freeze the dialog while
/// it times out.
struct OcrWarmState {
	std::mutex mutex;
	std::shared_ptr<ocr::OcrBackend> backend;
	/// Why the start attempt failed; empty when it worked or is still pending.
	std::string diagnostic;
	std::atomic<bool> ready{false};
	/// Pipeline switches the engine is started with. The Paddle runtime reads
	/// them from its command line, so they are fixed for its lifetime; the ONNX
	/// engine ignores them and switches per request instead.
	ocr::OCRProcessConfig config{true, false, false};
};

/// Start `state`'s engine on the background dispatcher. `ready` is set when the
/// attempt finishes, successfully or not, so waiters never block forever.
void StartOcrWarmup(std::shared_ptr<OcrWarmState> const& state);

/// Wait up to `timeout_ms` for the warm-up to finish and return its engine, or
/// nullptr when it failed or is still loading. `timeout_ms <= 0` does not wait.
std::shared_ptr<ocr::OcrBackend> WaitForOcrEngine(std::shared_ptr<OcrWarmState> const& state,
                                                  int timeout_ms);

/// Why the warm-up failed; empty when it worked or is still pending. Intended
/// for the code path that finds no engine after WaitForOcrEngine().
std::string OcrWarmDiagnostic(std::shared_ptr<OcrWarmState> const& state);
