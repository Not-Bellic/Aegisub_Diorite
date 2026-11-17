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

#include "ocr_warmup.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

void StartOcrWarmup(std::shared_ptr<OcrWarmState> const &state) {
	if (!state)
		return;

	// The task captures the state, never the dialog, so closing the dialog while
	// the engine is still loading is safe; dispatch also routes any exception to
	// the crash handler instead of terminating the process.
	agi::dispatch::Background().Async([state] {
		try {
			auto backend = ocr::MakeBackend(ocr::ConfiguredBackendKind(), state->config);
			std::string diagnostic;
			auto const start = std::chrono::steady_clock::now();
			bool const started = backend && backend->Start(diagnostic);

			std::lock_guard<std::mutex> lock(state->mutex);
			if (started) {
				LOG_I("ocr/warmup") << "engine " << backend->Name() << " ready in "
					<< std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - start).count() << " ms";
				state->backend = std::shared_ptr<ocr::OcrBackend>(std::move(backend));
				state->diagnostic.clear();
			}
			else if (diagnostic.empty()) {
				state->diagnostic = "The OCR engine could not be started.";
			}
			else {
				state->diagnostic = diagnostic;
			}
			if (!started)
				LOG_E("ocr/warmup") << "no OCR engine: " << state->diagnostic;
			// Mark the attempt finished even on failure: waiters would otherwise
			// block for the full timeout when no runtime is installed.
			state->ready = true;
		}
		catch (...) {
			// OCR startup failure must never crash the app.
			std::lock_guard<std::mutex> lock(state->mutex);
			state->diagnostic = "The OCR engine could not be started.";
			state->ready = true;
		}
	});
}

std::shared_ptr<ocr::OcrBackend> WaitForOcrEngine(std::shared_ptr<OcrWarmState> const &state,
                                                  int timeout_ms) {
	if (!state)
		return {};

	auto const deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(std::max(0, timeout_ms));
	while (!state->ready.load()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return {};
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	std::lock_guard<std::mutex> lock(state->mutex);
	return state->backend;
}

std::string OcrWarmDiagnostic(std::shared_ptr<OcrWarmState> const &state) {
	if (!state)
		return {};

	std::lock_guard<std::mutex> lock(state->mutex);
	return state->diagnostic;
}
