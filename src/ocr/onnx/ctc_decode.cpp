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

#include "ctc_decode.h"

#include <libaegisub/io.h>

#include <cstddef>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace ocr {
namespace onnx {

CtcDecoded DecodeCtc(float const *probs, int steps, int classes,
                     std::vector<std::string> const &dict) {
	CtcDecoded decoded;
	if (!probs || steps <= 0 || classes <= 0)
		return decoded;

	int const dict_size = static_cast<int>(dict.size());
	// PaddleOCR appends one space class after the dictionary.
	int const space_index = dict_size + 1;

	double confidence_sum = 0.0;
	int emitted = 0;
	int previous = -1;

	for (int t = 0; t < steps; ++t) {
		float const *row = probs + static_cast<size_t>(t) * classes;
		int index = 0;
		float value = row[0];
		for (int c = 1; c < classes; ++c) {
			if (row[c] > value) {
				value = row[c];
				index = c;
			}
		}

		if (index != previous && index != 0) {
			if (index - 1 < dict_size)
				decoded.text += dict[static_cast<size_t>(index - 1)];
			else if (index == space_index)
				decoded.text += ' ';
			else
				decoded.text += '?';
			confidence_sum += value;
			++emitted;
		}
		previous = index;
	}

	if (emitted > 0)
		decoded.confidence = confidence_sum / emitted;
	return decoded;
}

bool LoadDict(agi::fs::path const &path, std::vector<std::string> &dict, std::string &error) {
	dict.clear();
	error.clear();

	std::unique_ptr<std::istream> stream;
	try {
		stream = agi::io::Open(path);
	}
	catch (std::exception const &e) {
		error = std::string("Cannot open the OCR dictionary: ") + e.what();
		return false;
	}

	std::string line;
	while (std::getline(*stream, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		dict.push_back(line);
	}

	// A file ending with a newline produces one trailing empty entry.
	while (!dict.empty() && dict.back().empty())
		dict.pop_back();

	if (dict.empty()) {
		error = "The OCR dictionary is empty.";
		return false;
	}
	return true;
}

} // namespace onnx
} // namespace ocr
