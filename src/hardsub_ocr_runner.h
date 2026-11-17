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

#include "hardsub_scan.h"
#include "hardsub_timeline_scan.h"
#include "ocr/ocr_backend.h"

#include <memory>

namespace hardsub {

/// Builds the detection and recognition callables the whole-video scan uses.
///
/// Both the dialog and the console benchmark go through this so they measure
/// the same pipeline: the region is bounded to 1600 px before it is handed to
/// the detector, and a genuinely small line crop is upscaled before it is
/// recognized. Doing it in one place keeps a benchmark number meaningful for
/// the UI path.
///
/// The callables are not thread-safe: they use the engine's single session, so
/// callers either run them from one thread or serialize them.
RegionDetector MakeOcrDetector(std::shared_ptr<ocr::OcrBackend> backend,
                               ocr::OCROptions options,
                               CancelFn cancel,
                               ScanProfile *profile = nullptr);

RegionRecognizer MakeOcrRecognizer(std::shared_ptr<ocr::OcrBackend> backend,
                                   ocr::OCROptions options,
                                   CancelFn cancel,
                                   ScanProfile *profile = nullptr);

} // namespace hardsub
