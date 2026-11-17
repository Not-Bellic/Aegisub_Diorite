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

#include "det_postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ocr {
namespace onnx {
namespace {

/// Neighbour offsets for 8-connectivity, as (dx, dy) pairs.
int const kNeighbourX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
int const kNeighbourY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

} // namespace

std::vector<DetBox> GroupDetectorOutput(float const *prob, int width, int height,
                                        DetPostprocessOptions const &options) {
	std::vector<DetBox> boxes;
	if (!prob || width <= 0 || height <= 0)
		return boxes;

	size_t const count = static_cast<size_t>(width) * static_cast<size_t>(height);
	std::vector<uint8_t> mask(count, 0);
	for (size_t i = 0; i < count; ++i)
		mask[i] = prob[i] >= options.thresh ? 1 : 0;

	std::vector<int32_t> visited(count, 0);
	std::vector<int32_t> stack;
	std::vector<int32_t> component;
	stack.reserve(1024);
	component.reserve(1024);

	for (int32_t start = 0; start < static_cast<int32_t>(count); ++start) {
		if (!mask[start] || visited[start])
			continue;

		stack.clear();
		component.clear();
		stack.push_back(start);
		visited[start] = 1;

		while (!stack.empty()) {
			int32_t index = stack.back();
			stack.pop_back();
			component.push_back(index);

			int cx = index % width;
			int cy = index / width;
			for (int k = 0; k < 8; ++k) {
				int nx = cx + kNeighbourX[k];
				int ny = cy + kNeighbourY[k];
				if (nx < 0 || ny < 0 || nx >= width || ny >= height)
					continue;
				int32_t next = ny * width + nx;
				if (!mask[next] || visited[next])
					continue;
				visited[next] = 1;
				stack.push_back(next);
			}
		}

		int min_x = width;
		int min_y = height;
		int max_x = -1;
		int max_y = -1;
		double sum = 0.0;
		for (int32_t index : component) {
			int x = index % width;
			int y = index / width;
			min_x = std::min(min_x, x);
			max_x = std::max(max_x, x);
			min_y = std::min(min_y, y);
			max_y = std::max(max_y, y);
			sum += prob[index];
		}

		if (max_x < min_x || max_y < min_y)
			continue;

		int box_w = max_x - min_x + 1;
		int box_h = max_y - min_y + 1;
		if (box_w < options.min_size || box_h < options.min_size)
			continue;

		double const score = sum / static_cast<double>(component.size());
		if (score < options.box_thresh)
			continue;

		// DB's unclip: move each edge outward by the distance that keeps the
		// area ratio, which for a rectangle is area * ratio / perimeter.
		double const perimeter = 2.0 * (box_w + box_h);
		double const distance = perimeter > 0.0
			? static_cast<double>(component.size()) * options.unclip_ratio / perimeter
			: 0.0;

		int x0 = static_cast<int>(std::floor(min_x - distance));
		int y0 = static_cast<int>(std::floor(min_y - distance));
		int x1 = static_cast<int>(std::ceil(max_x + distance));
		int y1 = static_cast<int>(std::ceil(max_y + distance));

		DetBox box;
		box.x = std::max(0, x0);
		box.y = std::max(0, y0);
		box.w = std::min(width - 1, x1) - box.x + 1;
		box.h = std::min(height - 1, y1) - box.y + 1;
		if (box.w < options.min_size || box.h < options.min_size)
			continue;
		box.score = score;
		boxes.push_back(box);
	}

	std::stable_sort(boxes.begin(), boxes.end(), [](DetBox const &a, DetBox const &b) {
		if (a.y != b.y)
			return a.y < b.y;
		return a.x < b.x;
	});
	return boxes;
}

} // namespace onnx
} // namespace ocr
