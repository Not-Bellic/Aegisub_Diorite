#include <main.h>

#include "ocr/onnx/ctc_decode.h"
#include "ocr/onnx/det_postprocess.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

std::vector<float> MakeMap(int width, int height, float background) {
	return std::vector<float>(static_cast<size_t>(width) * height, background);
}

void PaintRect(std::vector<float> &map, int width, int x0, int y0, int x1, int y1, float value) {
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x)
			map[static_cast<size_t>(y) * width + x] = value;
}

} // namespace

TEST(ocr_onnx, detector_groups_one_rectangle) {
	int const width = 64;
	int const height = 32;
	auto map = MakeMap(width, height, 0.0f);
	PaintRect(map, width, 10, 10, 19, 19, 0.9f);

	auto boxes = ocr::onnx::GroupDetectorOutput(map.data(), width, height, ocr::onnx::DetPostprocessOptions());
	ASSERT_EQ(boxes.size(), 1u);
	// The unclip distance is area * ratio / perimeter = 100 * 1.5 / 40 = 3.75.
	EXPECT_EQ(boxes[0].x, 6);
	EXPECT_EQ(boxes[0].y, 6);
	EXPECT_EQ(boxes[0].w, 18);
	EXPECT_EQ(boxes[0].h, 18);
	EXPECT_NEAR(boxes[0].score, 0.9, 1e-6);
}

TEST(ocr_onnx, detector_keeps_components_separate_and_sorts_them) {
	int const width = 64;
	int const height = 48;
	auto map = MakeMap(width, height, 0.0f);
	// The lower-right box is painted first so the ordering cannot come from
	// insertion order. Boxes sort top-to-bottom first, so the upper box is also
	// the left one: that keeps both ordering checks meaningful.
	PaintRect(map, width, 40, 30, 50, 37, 0.8f);
	PaintRect(map, width, 8, 5, 15, 14, 0.8f);

	auto boxes = ocr::onnx::GroupDetectorOutput(map.data(), width, height, ocr::onnx::DetPostprocessOptions());
	ASSERT_EQ(boxes.size(), 2u);
	EXPECT_LT(boxes[0].y, boxes[1].y);
	EXPECT_LT(boxes[0].x, boxes[1].x);
}

TEST(ocr_onnx, detector_drops_low_confidence_components) {
	int const width = 64;
	int const height = 32;
	auto map = MakeMap(width, height, 0.0f);
	PaintRect(map, width, 10, 10, 20, 20, 0.4f); // above thresh, below box_thresh

	auto boxes = ocr::onnx::GroupDetectorOutput(map.data(), width, height, ocr::onnx::DetPostprocessOptions());
	EXPECT_TRUE(boxes.empty());
}

TEST(ocr_onnx, detector_drops_tiny_components) {
	int const width = 64;
	int const height = 32;
	auto map = MakeMap(width, height, 0.0f);
	PaintRect(map, width, 10, 10, 11, 11, 0.95f); // 2x2, below min_size

	auto boxes = ocr::onnx::GroupDetectorOutput(map.data(), width, height, ocr::onnx::DetPostprocessOptions());
	EXPECT_TRUE(boxes.empty());
}

TEST(ocr_onnx, detector_handles_empty_input) {
	std::vector<float> map(16 * 16, 0.0f);
	EXPECT_TRUE(ocr::onnx::GroupDetectorOutput(nullptr, 16, 16, ocr::onnx::DetPostprocessOptions()).empty());
	EXPECT_TRUE(ocr::onnx::GroupDetectorOutput(map.data(), 0, 16, ocr::onnx::DetPostprocessOptions()).empty());
}

TEST(ocr_onnx, ctc_decode_emits_dictionary_characters) {
	// Dictionary order plus the trailing space class: 0 = blank, 1 = 'a',
	// 2 = 'b', 3 = space.
	std::vector<std::string> const dict = { "a", "b" };
	std::vector<float> const probs = {
		0.05f, 0.90f, 0.03f, 0.02f, // 'a'
		0.02f, 0.95f, 0.02f, 0.01f, // 'a' again: repeat, suppressed
		0.99f, 0.00f, 0.00f, 0.01f, // blank
		0.10f, 0.05f, 0.80f, 0.05f, // 'b'
	};

	auto const decoded = ocr::onnx::DecodeCtc(probs.data(), 4, 4, dict);
	EXPECT_EQ(decoded.text, "ab");
	EXPECT_NEAR(decoded.confidence, 0.85, 1e-5);
}

TEST(ocr_onnx, ctc_decode_maps_the_space_class) {
	std::vector<std::string> const dict = { "a", "b" };
	std::vector<float> const probs = {
		0.05f, 0.90f, 0.03f, 0.02f, // 'a'
		0.02f, 0.02f, 0.02f, 0.94f, // space
		0.05f, 0.03f, 0.90f, 0.02f, // 'b'
	};

	auto const decoded = ocr::onnx::DecodeCtc(probs.data(), 3, 4, dict);
	EXPECT_EQ(decoded.text, "a b");
}

TEST(ocr_onnx, ctc_decode_returns_nothing_for_blanks) {
	std::vector<std::string> const dict = { "a", "b" };
	std::vector<float> const probs = {
		0.99f, 0.01f, 0.00f, 0.00f,
		0.98f, 0.02f, 0.00f, 0.00f,
	};

	auto const decoded = ocr::onnx::DecodeCtc(probs.data(), 2, 4, dict);
	EXPECT_EQ(decoded.text, "");
	EXPECT_EQ(decoded.confidence, 0.0);
}
