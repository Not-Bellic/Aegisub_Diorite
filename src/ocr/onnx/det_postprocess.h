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

#include <vector>

namespace ocr {
namespace onnx {

/// An axis-aligned text box in detector-input pixels. Burned-in subtitles are
/// horizontal, so the rotated rectangles a full DB implementation would produce
/// are not needed.
struct DetBox {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	double score = 0.0;
};

struct DetPostprocessOptions {
	/// Pixels at or above this probability are text.
	double thresh = 0.3;
	/// Components whose mean probability is below this are dropped.
	double box_thresh = 0.6;
	/// DB unclip ratio: the box is expanded by `area * ratio / perimeter`.
	double unclip_ratio = 1.5;
	/// Boxes smaller than this many pixels on either side are dropped.
	int min_size = 3;
};

/// Group a detector probability map into boxes. `prob` holds width * height
/// floats in row-major order. Components are 8-connected; each one is expanded
/// with the DB unclip distance and clamped to the map. Boxes come back sorted
/// top-to-bottom, then left-to-right.
std::vector<DetBox> GroupDetectorOutput(float const *prob, int width, int height,
                                        DetPostprocessOptions const &options);

} // namespace onnx
} // namespace ocr
