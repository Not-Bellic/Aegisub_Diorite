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

#include <libaegisub/fs_fwd.h>

#include <string>

struct OrtApi;

namespace ocr {
namespace onnx {

/// Directory holding onnxruntime.dll, the two models and the dictionary:
/// `?data/ocr/onnx`, or the `AEGISUB_OCR_ASSETS` environment variable when set
/// (unit tests and diagnostics).
agi::fs::path AssetDir();

/// True when the runtime DLL, both models and the dictionary are present.
/// Only stats the files: nothing is loaded, so this is safe to call from a menu
/// validator. Fills `diagnostic` with the reason when they are missing.
bool AssetsPresent(std::string &diagnostic);

/// Load onnxruntime.dll once and return its API table. Returns nullptr and
/// fills `diagnostic` when the runtime is missing or too old. Thread-safe.
OrtApi const *Api(std::string &diagnostic);

/// Path of the DLL that was loaded, empty before a successful load.
std::string RuntimePath();

} // namespace onnx
} // namespace ocr
