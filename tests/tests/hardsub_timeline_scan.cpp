#include <main.h>

#include "hardsub_timeline_scan.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace {

using namespace hardsub;

VideoFrame MakeFrame(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = width * 4;
	frame.flipped = false;
	frame.data.assign(frame.pitch * height, 0);
	for (size_t i = 0; i < frame.data.size(); i += 4) {
		frame.data[i] = b;
		frame.data[i + 1] = g;
		frame.data[i + 2] = r;
		frame.data[i + 3] = 255;
	}
	return frame;
}

RegionImage MakeBandRegion(int width, int height, unsigned char bg,
                           int band_y0, int band_y1, unsigned char band_lum) {
	auto frame = MakeFrame(width, height, bg, bg, bg);
	for (int y = band_y0; y < band_y1; ++y) {
		for (int x = 0; x < width; ++x) {
			size_t off = size_t(y) * frame.pitch + size_t(x) * 4;
			frame.data[off] = band_lum;
			frame.data[off + 1] = band_lum;
			frame.data[off + 2] = band_lum;
		}
	}
	return CropRegion(frame, 0, 0, width, height);
}

ocr::OCRLine MakeLine(std::string text, int x0, int y0, int x1, int y1) {
	ocr::OCRLine line;
	line.text = std::move(text);
	line.confidence = 0.9;
	line.box = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
	return line;
}

} // namespace

TEST(hardsub_timeline_scan, line_bounding_rect) {
	auto line = MakeLine("hi", 2, 3, 8, 10);
	auto rect = LineBoundingRect(line);
	EXPECT_EQ(rect.x, 2);
	EXPECT_EQ(rect.y, 3);
	EXPECT_EQ(rect.w, 6);
	EXPECT_EQ(rect.h, 7);
	EXPECT_TRUE(rect.Valid());
}

TEST(hardsub_timeline_scan, cluster_rows_separates_stacked_lines) {
	std::vector<ocr::OCRLine> lines = {
		MakeLine("top", 10, 0, 60, 12),
		MakeLine("bottom", 10, 30, 60, 44),
	};
	auto rows = ClusterRows(lines, 60);
	EXPECT_EQ(rows.size(), 2u);
	EXPECT_LT(rows[0].second.y, rows[1].second.y);
}

TEST(hardsub_timeline_scan, cluster_rows_merges_overlapping_fragments) {
	// Two boxes that overlap >50% in height are fragments of one visual row.
	std::vector<ocr::OCRLine> lines = {
		MakeLine("a", 0, 0, 40, 12),
		MakeLine("b", 30, 2, 70, 10),
	};
	auto rows = ClusterRows(lines, 60);
	EXPECT_EQ(rows.size(), 1u);
	EXPECT_GE(rows[0].second.w, 60);
}

TEST(hardsub_timeline_scan, row_center_filter) {
	// Region 100 wide, band 10 => center must be within 5 px of the middle.
	TimelineRect centered{40, 0, 20, 12};
	EXPECT_TRUE(RowPassesCenterFilter(centered, 100, 10));
	TimelineRect off{25, 0, 20, 12};            // center at 35, 15 px off middle
	EXPECT_FALSE(RowPassesCenterFilter(off, 100, 10));
	EXPECT_TRUE(RowPassesCenterFilter(off, 100, 100)); // 100 disables the filter
}

TEST(hardsub_timeline_scan, row_signature_matches_similar_content) {
	auto a = RowSignature(MakeBandRegion(40, 16, 64, 4, 10, 220));
	auto b = RowSignature(MakeBandRegion(40, 16, 70, 4, 10, 220)); // same band, slightly different bg
	EXPECT_LT(SignatureDistance(a, b), 0.2);
	auto c = RowSignature(MakeBandRegion(40, 16, 64, 10, 15, 220)); // band moved
	EXPECT_GT(SignatureDistance(a, c), 0.2);
}

TEST(hardsub_timeline_scan, scan_timeline_no_ocr_finds_nothing) {
	TimelineScanOptions opts;
	opts.region_x = 0;
	opts.region_y = 0;
	opts.region_w = 64;
	opts.region_h = 64;
	opts.frame_count = 30;
	opts.frame_start = 0;
	opts.frame_end = -1;
	opts.detect_every = 1;

	auto loader = FrameLoader([](int) -> std::shared_ptr<VideoFrame> {
		return std::make_shared<VideoFrame>(MakeFrame(64, 64, 120, 120, 120));
	});
	auto detect = RegionDetector([](RegionImage const&) -> ocr::OCRResult { return ocr::OCRResult(); });
	auto recognize = RegionRecognizer([](RegionImage const&) -> ocr::OCRResult { return ocr::OCRResult(); });

	auto result = ScanVideoTimeline(opts, loader, detect, recognize);
	ASSERT_TRUE(result.ok);
	EXPECT_TRUE(result.events.empty());
	EXPECT_GT(result.frames_evaluated, 0);
}

// A subtitle-like bright band appears on a static background for frames
// [40,60]. A "perfect" detector reports a box around the band exactly when it
// is present. This exercises the whole discovery + boundary-scan pipeline and
// pins down whether the scan logic recovers a known subtitle at all.
TEST(hardsub_timeline_scan, scan_timeline_finds_known_subtitle) {
	TimelineScanOptions opts;
	opts.region_x = 0;
	opts.region_y = 0;
	opts.region_w = 256;
	opts.region_h = 40;
	opts.frame_count = 100;
	opts.frame_start = 0;
	opts.frame_end = -1;
	opts.center_only = false;
	opts.center_band = 100;
	opts.detect_every = 4;
	opts.detect_every_max = 24;
	opts.confirm_frames = 2;
	opts.min_duration_frames = 2;

	// Dark background with a bright text band in rows [20,30), often a small
	// fraction of the region so it does not trip the "content changed" gate.
	auto loader = FrameLoader([opts](int f) -> std::shared_ptr<VideoFrame> {
		bool sub = f >= 40 && f <= 60;
		auto frame = MakeFrame(opts.region_w, opts.region_h, 35, 35, 35);
		if (sub) {
			// Stroke-dash "text": bright glyphs separated by dark gaps so the
			// detected box contains both text and background pixels (like a
			// real burned-in subtitle), which TemplateTextPixels needs.
			for (int y = 20; y < 30; ++y) {
				for (int x = 128; x < 192; ++x) {
					if ((x & 7) < 4) continue; // dark gap
					size_t off = size_t(y) * frame.pitch + size_t(x) * 4;
					frame.data[off] = 215;
					frame.data[off + 1] = 215;
					frame.data[off + 2] = 215;
				}
			}
		}
		return std::make_shared<VideoFrame>(std::move(frame));
	});

	auto detect = RegionDetector([](RegionImage const& r) -> ocr::OCRResult {
		ocr::OCRResult out;
		int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
		for (int y = 0; y < r.height; ++y) {
			for (int x = 0; x < r.width; ++x) {
				const unsigned char* p = r.rgb.data() + (size_t(y) * r.width + x) * 3;
				int l = (p[0] + p[1] + p[2]) / 3;
				if (l > 180) {
					minx = std::min(minx, x);
					miny = std::min(miny, y);
					maxx = std::max(maxx, x);
					maxy = std::max(maxy, y);
				}
			}
		}
		if (maxx >= 0) {
			out.ok = true;
			// The detector box is deliberately a little larger than the glyphs
			// so it always includes a few rows/cols of background.
			out.lines.push_back(MakeLine("marker",
				std::max(0, minx - 2), std::max(0, miny - 1),
				std::min(r.width - 1, maxx + 2), std::min(r.height - 1, maxy + 1)));
		}
		return out;
	});
	auto recognize = RegionRecognizer([](RegionImage const&) -> ocr::OCRResult {
		ocr::OCRResult out;
		out.ok = true;
		ocr::OCRLine line;
		line.text = "known";
		line.confidence = 0.9;
		out.lines.push_back(line);
		return out;
	});

	auto result = ScanVideoTimeline(opts, loader, detect, recognize);
	ASSERT_TRUE(result.ok);
	ASSERT_FALSE(result.events.empty());
	EXPECT_EQ(result.events.size(), 1u);
	// The recovered timing must lie inside the subtitle's true window.
	EXPECT_GE(result.events[0].start_frame, 38);
	EXPECT_LE(result.events[0].end_frame, 62);
	EXPECT_EQ(result.events[0].text, "known");
}

// The reduced-resolution mode must feed a smaller region image to OCR and to
// every per-frame pass, while still recovering the same subtitle and reporting
// its box in video coordinates.
TEST(hardsub_timeline_scan, scan_timeline_downscales_region_before_scanning) {
	TimelineScanOptions opts;
	opts.region_x = 0;
	opts.region_y = 0;
	opts.region_w = 256;
	opts.region_h = 40;
	opts.frame_count = 100;
	opts.frame_start = 0;
	opts.frame_end = -1;
	opts.center_only = false;
	opts.center_band = 100;
	opts.detect_every = 4;
	opts.detect_every_max = 24;
	opts.confirm_frames = 2;
	opts.min_duration_frames = 2;
	opts.scan_scale = 0.5;

	auto loader = FrameLoader([opts](int f) -> std::shared_ptr<VideoFrame> {
		bool sub = f >= 40 && f <= 60;
		auto frame = MakeFrame(opts.region_w, opts.region_h, 35, 35, 35);
		if (sub) {
			for (int y = 20; y < 30; ++y) {
				for (int x = 128; x < 192; ++x) {
					if ((x & 7) < 4) continue;
					size_t off = size_t(y) * frame.pitch + size_t(x) * 4;
					frame.data[off] = 215;
					frame.data[off + 1] = 215;
					frame.data[off + 2] = 215;
				}
			}
		}
		return std::make_shared<VideoFrame>(std::move(frame));
	});

	int seen_width = 0;
	int seen_height = 0;
	auto detect = RegionDetector([&](RegionImage const& r) -> ocr::OCRResult {
		seen_width = r.width;
		seen_height = r.height;
		ocr::OCRResult out;
		int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
		for (int y = 0; y < r.height; ++y) {
			for (int x = 0; x < r.width; ++x) {
				const unsigned char* p = r.rgb.data() + (size_t(y) * r.width + x) * 3;
				int l = (p[0] + p[1] + p[2]) / 3;
				if (l > 180) {
					minx = std::min(minx, x);
					miny = std::min(miny, y);
					maxx = std::max(maxx, x);
					maxy = std::max(maxy, y);
				}
			}
		}
		if (maxx >= 0) {
			out.ok = true;
			out.lines.push_back(MakeLine("marker",
				std::max(0, minx - 2), std::max(0, miny - 1),
				std::min(r.width - 1, maxx + 2), std::min(r.height - 1, maxy + 1)));
		}
		return out;
	});
	auto recognize = RegionRecognizer([](RegionImage const&) -> ocr::OCRResult {
		ocr::OCRResult out;
		out.ok = true;
		ocr::OCRLine line;
		line.text = "known";
		line.confidence = 0.9;
		out.lines.push_back(line);
		return out;
	});

	auto result = ScanVideoTimeline(opts, loader, detect, recognize);
	ASSERT_TRUE(result.ok);
	// OCR only ever saw the region at half resolution.
	EXPECT_EQ(seen_width, 128);
	EXPECT_EQ(seen_height, 20);
	ASSERT_EQ(result.events.size(), 1u);
	EXPECT_GE(result.events[0].start_frame, 38);
	EXPECT_LE(result.events[0].end_frame, 62);
	EXPECT_EQ(result.events[0].text, "known");
	// The reported box is mapped back into video pixel coordinates.
	EXPECT_GE(result.events[0].row_video.x, 0);
	EXPECT_LE(result.events[0].row_video.x + result.events[0].row_video.w, 256);
}

namespace {

TimelineRowEvent MakeRowEvent(int start, int end, std::string text, double confidence,
                              TimelineRect row) {
	TimelineRowEvent ev;
	ev.start_frame = start;
	ev.end_frame = end;
	ev.rep_frame = start + (end - start) / 2;
	ev.text = std::move(text);
	ev.confidence = confidence;
	ev.row_region = row;
	ev.ocr_verified = true;
	return ev;
}

} // namespace

TEST(hardsub_timeline_scan, ocr_text_normalization_ignores_case_and_punctuation) {
	// Readings of the same line that differ only in case, spacing or final
	// punctuation must compare as equal: that is what makes the vote and the
	// dedupe robust against formatting noise from the recognizer.
	auto a = NormalizeOcrText("She's Human.");
	auto b = NormalizeOcrText("she's   human");
	EXPECT_EQ(a, b);
	EXPECT_EQ(OcrCharCount(a), OcrCharCount(b));
	EXPECT_GT(OcrTextSimilarity(NormalizeOcrText("hello"), NormalizeOcrText("hallo")), 0.5);
	EXPECT_LT(OcrTextSimilarity(NormalizeOcrText("yes"), NormalizeOcrText("no")), 0.5);
}

TEST(hardsub_timeline_scan, minimum_confidence_rises_for_short_text) {
	// CTC scores over few time steps are inflated, so a short reading needs a
	// much higher bar than a long one before it is trusted.
	EXPECT_GT(MinimumOcrConfidence(1), MinimumOcrConfidence(3));
	EXPECT_GT(MinimumOcrConfidence(3), MinimumOcrConfidence(6));
	EXPECT_GT(MinimumOcrConfidence(6), MinimumOcrConfidence(12));
	EXPECT_GE(MinimumOcrConfidence(12), 0.5);
}

TEST(hardsub_timeline_scan, vote_prefers_repeated_reading_over_longer_one) {
	// A hallucinated reading tends to gain characters; the cluster that more
	// frames agree on must win even when another reading is longer.
	std::vector<std::pair<std::string, double>> candidates = {
		{"Hi.", 0.90},
		{"Hi.", 0.92},
		{"Hii there", 0.70},
	};
	EXPECT_EQ(VoteBestOcrText(candidates), "Hi.");
}

TEST(hardsub_timeline_scan, vote_single_candidate_is_returned_unchanged) {
	std::vector<std::pair<std::string, double>> candidates = {{"Only one", 0.8}};
	EXPECT_EQ(VoteBestOcrText(candidates), "Only one");
	EXPECT_TRUE(VoteBestOcrText({}).empty());
}

TEST(hardsub_timeline_scan, dedupe_merges_overlapping_duplicate_rows) {
	// The same subtitle discovered twice (a track split) covers almost the same
	// interval; it must collapse into one row spanning both detections.
	auto first = MakeRowEvent(100, 160, "I'm here.", 0.90, TimelineRect{10, 20, 200, 30});
	auto second = MakeRowEvent(104, 162, "I'm here.", 0.94, TimelineRect{11, 21, 200, 30});
	auto kept = DedupeTimelineEvents({first, second}, 1, 0.8, 2);

	ASSERT_EQ(kept.size(), 1u);
	EXPECT_EQ(kept[0].start_frame, 100);
	EXPECT_EQ(kept[0].end_frame, 162);
	EXPECT_DOUBLE_EQ(kept[0].confidence, 0.94);
	EXPECT_EQ(kept[0].text, "I'm here.");
}

TEST(hardsub_timeline_scan, dedupe_keeps_rows_with_different_text) {
	auto first = MakeRowEvent(100, 160, "Yes.", 0.90, TimelineRect{10, 20, 200, 30});
	auto second = MakeRowEvent(102, 158, "No.", 0.92, TimelineRect{10, 20, 200, 30});
	auto kept = DedupeTimelineEvents({first, second}, 1, 0.8, 2);
	EXPECT_EQ(kept.size(), 2u);
}

TEST(hardsub_timeline_scan, dedupe_keeps_repeated_line_at_a_new_time) {
	// The same words spoken again later are two subtitles, not a duplicate.
	auto first = MakeRowEvent(100, 160, "Yes.", 0.90, TimelineRect{10, 20, 200, 30});
	auto second = MakeRowEvent(300, 360, "Yes.", 0.90, TimelineRect{10, 20, 200, 30});
	auto kept = DedupeTimelineEvents({first, second}, 1, 0.8, 2);
	EXPECT_EQ(kept.size(), 2u);
}

TEST(hardsub_timeline_scan, dedupe_keeps_rows_on_different_lines) {
	// A two-line subtitle has two rows with (nearly) the same interval but
	// different vertical positions: those must not be folded together.
	auto top = MakeRowEvent(100, 160, "Same text.", 0.90, TimelineRect{10, 20, 200, 30});
	auto bottom = MakeRowEvent(100, 160, "Same text.", 0.90, TimelineRect{10, 200, 200, 30});
	auto kept = DedupeTimelineEvents({top, bottom}, 1, 0.8, 2);
	EXPECT_EQ(kept.size(), 2u);
}

TEST(hardsub_timeline_scan, dedupe_drops_unreadable_rows) {
	auto blank = MakeRowEvent(10, 40, "   ", 0.90, TimelineRect{0, 0, 10, 10});
	auto one_char = MakeRowEvent(50, 80, "a", 0.99, TimelineRect{0, 0, 10, 10});
	EXPECT_TRUE(DedupeTimelineEvents({blank}, 1, 0.8, 2).empty());
	EXPECT_TRUE(DedupeTimelineEvents({one_char}, 2, 0.8, 2).empty());
	EXPECT_EQ(DedupeTimelineEvents({one_char}, 1, 0.8, 2).size(), 1u);
}

TEST(hardsub_timeline_scan, scan_timeline_votes_across_frames_for_short_rows) {
	// A short, uncertain reading triggers the extra samples; the vote must pick
	// the reading that more frames agree on.
	TimelineScanOptions opts;
	opts.region_x = 0;
	opts.region_y = 0;
	opts.region_w = 256;
	opts.region_h = 40;
	opts.frame_count = 100;
	opts.frame_start = 0;
	opts.frame_end = -1;
	opts.center_only = false;
	opts.center_band = 100;
	opts.detect_every = 4;
	opts.detect_every_max = 24;
	opts.confirm_frames = 2;
	opts.min_duration_frames = 2;
	opts.consensus_samples = 3;

	auto loader = FrameLoader([opts](int f) -> std::shared_ptr<VideoFrame> {
		bool sub = f >= 40 && f <= 60;
		auto frame = MakeFrame(opts.region_w, opts.region_h, 35, 35, 35);
		if (sub) {
			for (int y = 20; y < 30; ++y) {
				for (int x = 128; x < 192; ++x) {
					if ((x & 7) < 4) continue;
					size_t off = size_t(y) * frame.pitch + size_t(x) * 4;
					frame.data[off] = 215;
					frame.data[off + 1] = 215;
					frame.data[off + 2] = 215;
				}
			}
		}
		return std::make_shared<VideoFrame>(std::move(frame));
	});

	auto detect = RegionDetector([](RegionImage const& r) -> ocr::OCRResult {
		ocr::OCRResult out;
		int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
		for (int y = 0; y < r.height; ++y) {
			for (int x = 0; x < r.width; ++x) {
				const unsigned char* p = r.rgb.data() + (size_t(y) * r.width + x) * 3;
				if ((p[0] + p[1] + p[2]) / 3 > 180) {
					minx = std::min(minx, x);
					miny = std::min(miny, y);
					maxx = std::max(maxx, x);
					maxy = std::max(maxy, y);
				}
			}
		}
		if (maxx >= 0) {
			out.ok = true;
			out.lines.push_back(MakeLine("marker", minx, miny, maxx, maxy));
		}
		return out;
	});

	int calls = 0;
	auto recognize = RegionRecognizer([&calls](RegionImage const&) -> ocr::OCRResult {
		ocr::OCRResult out;
		out.ok = true;
		ocr::OCRLine line;
		// Short and unsure, so the scan samples neighbouring frames: the first
		// one is misread, the others agree on the real text.
		line.text = calls++ == 0 ? "Hey" : "Hi";
		line.confidence = 0.80;
		out.lines.push_back(line);
		return out;
	});

	auto result = ScanVideoTimeline(opts, loader, detect, recognize);
	ASSERT_TRUE(result.ok);
	ASSERT_EQ(result.events.size(), 1u);
	EXPECT_EQ(result.events[0].text, "Hi");
	EXPECT_GT(calls, 1);
}
