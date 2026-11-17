// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include "hardsub_scan.h"
#include "ocr/ocr_result.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hardsub {

/// Options for scanning an entire video for hard subtitles inside a configurable
/// region. The true (frame-exact) start/end of each detected row is recovered by
/// the same template-coverage scan used by the single-subtitle dialog; OCR is
/// only used here to discover rows and to read their text.
struct TimelineScanOptions {
	/// Region to scan, in video pixel coordinates.
	int region_x = 0;
	int region_y = 0;
	int region_w = 0;
	int region_h = 0;

	int frame_count = 0;
	int frame_start = 0;
	/// Inclusive; a negative value means the last frame.
	int frame_end = -1;

	/// Only keep rows whose horizontal center is inside the middle `center_band`
	/// percent of the region. 100 disables the filter.
	bool center_only = true;
	double center_band = 10.0;

	/// Drops detected runs shorter than this many frames.
	int min_duration_frames = 12;
	/// Debounce frames used both for OCR settle detection and the boundary scan.
	int confirm_frames = 2;

	/// Resolution factor applied to the scan region before any processing
	/// (1.0 = full resolution). Values below 1 crop the region at a smaller size
	/// with box averaging, which makes the per-frame pixel passes (crop, change
	/// detection, signature) and the OCR input cheaper at the cost of missing
	/// small or low-contrast text. The UI offers 0.75, 0.5, 0.33 and 0.25.
	double scan_scale = 1.0;

	/// Minimum number of frames between two OCR detection samples while the
	/// region is stable. Lower values are more responsive but slower.
	int detect_every = 16;
	/// Hard cap on frames between two OCR samples even while the region is
	/// never stable (e.g. continuous camera motion).
	int detect_every_max = 24;

	/// When two or more rows share exactly the same start/end frame, merge them
	/// into a single event joined with a line break; otherwise keep them separate.
	bool merge_same_interval = true;

	/// Replace the in-text line breaks ("\N") with a single space when building
	/// the final event text. True collapses multi-line OCR rows into one line.
	bool replace_newlines = true;

	/// Template-coverage thresholds forwarded to ScanBoundaries.
	double threshold = 0.40;
	double threshold_exit = 0.20;

	/// Frames sampled to vote on the text of a row whose first reading is short
	/// or uncertain (1 = the representative frame only). The extra samples are
	/// the frames right after the representative one, which the scan decoder
	/// serves without seeking.
	int consensus_samples = 3;
	/// Rows whose normalized text is shorter than this many characters are
	/// dropped (0 keeps every non-empty row).
	int min_text_chars = 1;
	/// Merge rows that describe the same on-screen subtitle: overlapping
	/// interval, same visual line, similar text.
	bool dedupe_events = true;
};

/// A bounding rectangle in region-relative pixel coordinates.
struct TimelineRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	bool Valid() const { return w > 0 && h > 0; }
	int CenterX() const { return x + w / 2; }
	int CenterY() const { return y + h / 2; }
};

/// One detected hard-subtitle row (may represent one visual line, or a merged
/// cluster of several simultaneously visible lines).
struct TimelineRowEvent {
	int start_frame = -1;
	int end_frame = -1;
	/// Frame used as the template/recognition anchor; lies inside [start,end].
	int rep_frame = -1;
	/// A double-checked full-region (not per-row) OCR pass ran for this row.
	bool ocr_verified = false;
	/// Text recognized for the row (possibly with embedded newlines if merged).
	std::string text;
	/// Mean confidence of the recognized lines (0..1).
	double confidence = 0.0;
	/// Row rectangle in video pixel coordinates.
	TimelineRect row_video;
	/// Row rectangle in region-relative pixel coordinates (as returned by OCR).
	TimelineRect row_region;
	/// True when this row's horizontal center passed the center filter.
	bool center_ok = false;
};

struct TimelineScanResult {
	bool ok = false;
	bool cancelled = false;
	std::string error;
	std::vector<TimelineRowEvent> events;
	int frames_evaluated = 0;
	int ocr_samples = 0;
};

/// Loads one frame by number; returning nullptr aborts the scan.
/// Runs text detection over a region image and returns recognised boxes (in the
/// image's own pixel coordinates; text is optional).
using RegionDetector = std::function<ocr::OCRResult(RegionImage const&)>;
/// Runs text recognition over a region image and returns the recognised text.
using RegionRecognizer = std::function<ocr::OCRResult(RegionImage const&)>;
/// Invoked once for every fully recognized row as soon as its text and timing
/// are resolved, so the UI can show it while the rest of the video is scanned.
using RowCallback = std::function<void(TimelineRowEvent const&)>;

/// Scan every frame in [frame_start, frame_end] of the given region, discover the
/// hard-subtitle rows with sparse OCR, then recover each row's exact start/end
/// with the template-coverage scan. Returns the discovered events in time order.
TimelineScanResult ScanVideoTimeline(TimelineScanOptions const& opts,
                                     FrameLoader const& loader,
                                     RegionDetector const& detect,
                                     RegionRecognizer const& recognize,
                                     CancelFn const& cancel = CancelFn(),
                                     ProgressFn const& progress = ProgressFn(),
                                     RowCallback const& row_callback = RowCallback());

// ---- Pure helpers (unit-testable without a decoder or OCR runtime) -----------

/// Convert an OCR line's polygon box into a bounding rectangle.
TimelineRect LineBoundingRect(ocr::OCRLine const& line);

/// Normalized form of an OCR reading, used to compare two readings of the same
/// text: tags and line break escapes stripped, punctuation removed, case
/// folded, whitespace collapsed.
std::string NormalizeOcrText(std::string const& text);

/// Number of Unicode characters (code points) in a UTF-8 string.
int OcrCharCount(std::string const& text);

/// Similarity (0..1) between two strings that are already normalized with
/// NormalizeOcrText.
double OcrTextSimilarity(std::string const& a, std::string const& b);

/// Minimum mean confidence a reading needs to be kept, by character count.
/// CTC scores are averaged over very few time steps for short strings, so a
/// two-character misread routinely scores above 0.9; the bar has to rise as
/// the text gets shorter.
double MinimumOcrConfidence(int text_chars);

/// Pick the consensus reading from several frames of the same subtitle: the
/// largest cluster of similar readings wins (a longest-string contest would
/// reward hallucinated extra characters), then total confidence, then length.
std::string VoteBestOcrText(std::vector<std::pair<std::string, double>> const& candidates);

/// Merge rows that describe the same on-screen subtitle: their intervals must
/// overlap by at least 80% of the shorter row, their boxes must sit on the same
/// visual line, and their normalized text must be the same or at least
/// `text_similarity` similar. Identical text separated by at most
/// `max_gap_frames` frames is stitched back together (a track that split for a
/// moment). Rows whose normalized text is shorter than `min_text_chars` are
/// dropped. The result is ordered by start frame.
std::vector<TimelineRowEvent> DedupeTimelineEvents(std::vector<TimelineRowEvent> events,
                                                   int min_text_chars,
                                                   double text_similarity,
                                                   int max_gap_frames);

/// Group OCR lines into visual rows. Lines whose vertical boxes substantially
/// overlap are treated as fragments of the same row (the scan region is assumed
/// to contain no overlapping subtitles). Rows are returned top-to-bottom.
std::vector<std::pair<std::vector<ocr::OCRLine>, TimelineRect>>
ClusterRows(std::vector<ocr::OCRLine> const& lines, int region_h);

/// Coarse luminance signature of a region image (16x8 downsampled luminance).
/// Used to decide whether a region has "settled" its content between OCR runs.
unsigned long long RegionSignature(RegionImage const& img);

/// Per-row signature from very small text-like (luminance-deviation) pixels.
std::vector<unsigned char> RowSignature(RegionImage const& img);

/// Normalised (0..1) distance between two row signatures; smaller means more alike.
double SignatureDistance(std::vector<unsigned char> const& a,
                         std::vector<unsigned char> const& b);

/// True when the horizontal centre of `rect` (a region-relative box) lies inside
/// the middle `center_band` percent of a region of width `region_w`.
bool RowPassesCenterFilter(TimelineRect const& rect, int region_w, double center_band);

} // namespace hardsub
