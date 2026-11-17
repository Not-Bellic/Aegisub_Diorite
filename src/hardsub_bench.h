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

#include <string>
#include <vector>

namespace hardsub {

/// True when the command line asks for the whole-video scan benchmark. Checked
/// before the application initializes, so it only looks at the arguments.
bool BenchRequested(std::vector<std::string> const &arguments);

/// Console mode for the whole-video scan: no window, no dialog, one JSON report
/// with the per-phase timing profile. This is the harness the scan's cost work
/// is measured and regressed with.
///
///   aegisub --hardsub-bench <video> [--region x,y,w,h] [--scale 50]
///           [--start N] [--count N] [--backend auto|onnx|paddle] [--no-ocr]
///           [--out report.json]
///
/// The region defaults to the bottom fifth of the video, like the dialog.
/// Does not return: the process exits with 0 when the scan ran, 1 when it was
/// cancelled or failed, and 2 when it could not start (bad arguments, no frame
/// loader, an OCR engine that would not start).
[[noreturn]] void RunBench(std::vector<std::string> const &arguments);

} // namespace hardsub
