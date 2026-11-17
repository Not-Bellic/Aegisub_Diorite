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

#include "hardsub_ocr_runner.h"

#include <libaegisub/fs.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <string>

#include <wx/filename.h>
#include <wx/image.h>

namespace hardsub {
namespace {

using Clock = std::chrono::steady_clock;

long long ElapsedMs(Clock::time_point from) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		Clock::now() - from).count();
}

/// Removes the temporary PNG when this task returns, on any path.
struct TempPngGuard {
	wxString &path;
	explicit TempPngGuard(wxString &p) : path(p) {}
	~TempPngGuard() {
		if (!path.empty())
			agi::fs::Remove(agi::fs::path(std::wstring(path.wc_str())));
	}
};

wxImage BuildImage(RegionImage const &crop, int scale) {
	if (!crop.Valid())
		return wxImage();
	wxImage img(crop.width, crop.height);
	std::memcpy(img.GetData(), crop.rgb.data(), crop.rgb.size());
	if (scale > 1)
		img = img.Scale(crop.width * scale, crop.height * scale, wxIMAGE_QUALITY_BICUBIC);
	return img;
}

/// Scales an image so its longest side is at most `max_side` (only downscales).
/// The whole-video scan feeds a large region (e.g. 3840x432) to OCR on every
/// sample; bounding it avoids the big-image codec/OCR edge cases and speeds the
/// scan up. The caller maps any OCR box coordinates back by the ratio of the
/// original to the returned image's size.
wxImage FitToMax(wxImage img, int max_side) {
	if (!img.IsOk())
		return img;
	int w = img.GetWidth();
	int h = img.GetHeight();
	int longest = std::max(w, h);
	if (longest > max_side) {
		double s = static_cast<double>(max_side) / longest;
		img = img.Scale(std::max(1, static_cast<int>(w * s)),
		                std::max(1, static_cast<int>(h * s)), wxIMAGE_QUALITY_BICUBIC);
	}
	return img;
}

/// Calls `fn` under a SEH guard. Returns 0 on success, or the SEH exception code
/// if `fn` hit a structured exception (e.g. an access violation in a third-party
/// library). Lets the scan survive a crashing OCR/encode call instead of taking
/// the whole process down.
int GuardSEH(std::function<void()> const &fn) {
	__try {
		fn();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return static_cast<int>(GetExceptionCode());
	}
}

/// Runs one crop through the backend.
///
/// An in-memory engine gets the pixels directly. The file-based ones (the
/// Paddle runtime) still need a path, so they keep the temporary-PNG route with
/// the same bounding the UI always had. The bounding happens only on that
/// route: an in-memory engine scales the image itself.
ocr::OCRResult RunCrop(std::shared_ptr<ocr::OcrBackend> const &backend,
                       RegionImage const &crop, ocr::OCROptions const &options,
                       ocr::OcrMode mode, std::function<bool()> const &cancel,
                       long long *image_ms) {
	ocr::OCRResult out;
	if (!backend)
		return out;

	if (backend->AcceptsImagesInMemory()) {
		ocr::OCRImage image;
		image.width = crop.width;
		image.height = crop.height;
		image.rgb = crop.rgb.data();
		out = backend->RunImage(image, options, mode, cancel);
		return out;
	}

	wxString temp;
	TempPngGuard guard(temp);
	auto const encode_begin = Clock::now();
	wxImage img = FitToMax(BuildImage(crop, 1), 1600);
	if (!img.IsOk())
		return out;
	temp = wxFileName::CreateTempFileName("aegisub-hardsub-");
	if (temp.empty() || !img.SaveFile(temp, wxBITMAP_TYPE_PNG))
		return out;
	if (image_ms)
		*image_ms += ElapsedMs(encode_begin);

	ocr::OCRResult tmp = backend->Run(agi::fs::path(std::wstring(temp.wc_str())),
	                                  options, mode, cancel);
	if (!tmp.ok || mode != ocr::OcrMode::DetectOnly)
		return tmp;

	// Detection boxes come back in the bounded image's coordinates.
	double sx = static_cast<double>(crop.width) / img.GetWidth();
	double sy = static_cast<double>(crop.height) / img.GetHeight();
	for (auto &line : tmp.lines)
		for (auto &pt : line.box) {
			pt.first = static_cast<int>(pt.first * sx);
			pt.second = static_cast<int>(pt.second * sy);
		}
	return tmp;
}

} // namespace

RegionDetector MakeOcrDetector(std::shared_ptr<ocr::OcrBackend> backend,
                               ocr::OCROptions options,
                               CancelFn cancel,
                               ScanProfile *profile) {
	return [backend, options, cancel, profile](RegionImage const &crop) -> ocr::OCRResult {
		ocr::OCRResult out;
		if (profile)
			++profile->ocr_detect_calls;
		int code = GuardSEH([&] {
			auto const begin = Clock::now();
			long long image_ms = 0;
			out = RunCrop(backend, crop, options, ocr::OcrMode::DetectOnly, cancel, &image_ms);
			if (profile) {
				profile->ms_ocr_image += image_ms;
				profile->ms_detect += ElapsedMs(begin) - image_ms;
			}
		});
		if (code)
			return ocr::OCRResult();
		return out;
	};
}

RegionRecognizer MakeOcrRecognizer(std::shared_ptr<ocr::OcrBackend> backend,
                                   ocr::OCROptions options,
                                   CancelFn cancel,
                                   ScanProfile *profile) {
	return [backend, options, cancel, profile](RegionImage const &crop) -> ocr::OCRResult {
		ocr::OCRResult out;
		if (profile)
			++profile->ocr_recognize_calls;
		int code = GuardSEH([&] {
			auto const begin = Clock::now();
			long long image_ms = 0;
			out = RunCrop(backend, crop, options, ocr::OcrMode::RecOnly, cancel, &image_ms);
			if (profile) {
				profile->ms_ocr_image += image_ms;
				profile->ms_recognize += ElapsedMs(begin) - image_ms;
			}
		});
		if (code)
			return ocr::OCRResult();
		return out;
	};
}

} // namespace hardsub
