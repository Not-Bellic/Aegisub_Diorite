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

#include <string>
#include <vector>

namespace ocr {

/// True when the command line asks for the OCR self-test. Checked before the
/// application initializes, so it only looks at the arguments.
bool SelfTestRequested(std::vector<std::string> const& arguments);

/// Console mode used by the packaging smoke test:
///
///   aegisub --ocr-selftest <image> [--out <report.json>]
///           [--backend auto|onnx|paddle] [--detect-only]
///
/// Runs one image through the configured engine, prints the JSON report and
/// writes it to `--out` when given. Does not return: the process terminates with
/// exit code 0 when text was recognized, 1 when the image held no text, and 2
/// when the engine could not run. The engine location comes from the normal
/// install layout, or from `AEGISUB_OCR_ASSETS` for an unpacked copy.
[[noreturn]] void RunSelfTest(std::vector<std::string> const& arguments);

} // namespace ocr
