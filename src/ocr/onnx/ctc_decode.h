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
#include <vector>

namespace ocr {
namespace onnx {

struct CtcDecoded {
	std::string text;
	/// Mean probability of the emitted timesteps (0 when nothing was read).
	double confidence = 0.0;
};

/// Greedy CTC decode of a [steps, classes] probability matrix.
///
/// Class 0 is the blank, classes 1..dict.size() are the dictionary entries in
/// file order, and the class right after the dictionary is a space (PaddleOCR
/// appends one when the model was exported with `use_space_char`). The exported
/// PP-OCRv6 recognizer already applies softmax, so the matrix is used as is.
CtcDecoded DecodeCtc(float const *probs, int steps, int classes,
                     std::vector<std::string> const &dict);

/// Load a dictionary file: UTF-8, one character per line, no BOM. A trailing
/// empty line is ignored; an empty file is an error.
bool LoadDict(agi::fs::path const &path, std::vector<std::string> &dict, std::string &error);

} // namespace onnx
} // namespace ocr
