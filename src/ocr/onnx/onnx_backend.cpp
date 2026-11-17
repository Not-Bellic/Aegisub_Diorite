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

#include "../ocr_backend.h"

#include "ctc_decode.h"
#include "det_postprocess.h"
#include "ort_api.h"

#include <libaegisub/fs.h>

#include "onnxruntime_c_api.h"

#include <wx/gdicmn.h>
#include <wx/image.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ocr {
namespace {

using onnx::CtcDecoded;
using onnx::DetBox;
using onnx::DetPostprocessOptions;

/// Height every recognizer input is normalized to, from the model metadata.
constexpr int kRecHeight = 48;
/// Widest recognizer input the exported model accepts.
constexpr int kMaxRecWidth = 3200;
/// The detector never needs more than this; the metadata allows up to 4000.
constexpr int kMaxDetSide = 2048;
constexpr int kMinDetSide = 320;

struct ModelPaths {
	agi::fs::path runtime;
	agi::fs::path detector;
	agi::fs::path recognizer;
	agi::fs::path dictionary;
};

ModelPaths ResolvePaths() {
	agi::fs::path const dir = onnx::AssetDir();
	ModelPaths paths;
	paths.runtime = dir / "onnxruntime.dll";
	paths.detector = dir / "ppocrv6_small_det.onnx";
	paths.recognizer = dir / "ppocrv6_small_rec.onnx";
	paths.dictionary = dir / "ppocrv6_dict.txt";
	return paths;
}

class OnnxBackend final : public OcrBackend {
	struct Session {
		OrtSession *handle = nullptr;
		std::string input_name;
		std::string output_name;
	};

	OrtApi const *api = nullptr;
	OrtEnv *env = nullptr;
	OrtMemoryInfo *memory = nullptr;
	Session detector;
	Session recognizer;
	std::vector<std::string> dictionary;
	std::string startup_diagnostic;
	bool started = false;

public:
	~OnnxBackend() override { Stop(); }

	bool Start(std::string &diagnostic) override {
		Stop();
		diagnostic.clear();

		std::string runtime_diagnostic;
		OrtApi const *loaded = onnx::Api(runtime_diagnostic);
		if (!loaded) {
			startup_diagnostic = runtime_diagnostic;
			diagnostic = runtime_diagnostic;
			return false;
		}
		api = loaded;

		ModelPaths const paths = ResolvePaths();
		if (!agi::fs::FileExists(paths.detector) || !agi::fs::FileExists(paths.recognizer) ||
		    !agi::fs::FileExists(paths.dictionary)) {
			startup_diagnostic = "The ONNX OCR models are missing from " + paths.detector.parent_path().string() +
				". Reinstall Aegisub or select the OCR option to restore them.";
			diagnostic = startup_diagnostic;
			return false;
		}

		OrtStatus *status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "aegisub-ocr", &env);
		if (status) {
			diagnostic = TakeStatus(status);
			startup_diagnostic = diagnostic;
			Stop();
			return false;
		}

		int const threads = std::max(1, std::min(8, static_cast<int>(std::thread::hardware_concurrency())));
		if (!OpenSession(paths.detector, threads, detector, diagnostic) ||
		    !OpenSession(paths.recognizer, threads, recognizer, diagnostic)) {
			startup_diagnostic = diagnostic;
			Stop();
			return false;
		}

		status = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory);
		if (status) {
			diagnostic = TakeStatus(status);
			startup_diagnostic = diagnostic;
			Stop();
			return false;
		}

		if (!onnx::LoadDict(paths.dictionary, dictionary, diagnostic)) {
			startup_diagnostic = diagnostic;
			Stop();
			return false;
		}

		startup_diagnostic.clear();
		started = true;
		return true;
	}

	void Stop() override {
		if (api) {
			if (memory) {
				api->ReleaseMemoryInfo(memory);
				memory = nullptr;
			}
			if (recognizer.handle) {
				api->ReleaseSession(recognizer.handle);
				recognizer.handle = nullptr;
			}
			if (detector.handle) {
				api->ReleaseSession(detector.handle);
				detector.handle = nullptr;
			}
			if (env) {
				api->ReleaseEnv(env);
				env = nullptr;
			}
		}
		detector.input_name.clear();
		detector.output_name.clear();
		recognizer.input_name.clear();
		recognizer.output_name.clear();
		dictionary.clear();
		started = false;
	}

	bool IsRunning() const override { return started; }

	OCRResult Run(agi::fs::path const &image_path, OCROptions const &options, OcrMode mode,
	              std::function<bool()> const &cancel) override {
		OCRResult result;
		if (!started) {
			result.diagnostic = startup_diagnostic.empty()
				? "The ONNX OCR backend is not running." : startup_diagnostic;
			return result;
		}

		wxImage image;
		if (!LoadImage(image_path, image, result.diagnostic))
			return result;

		int const limit = options.det_limit_side_len > 0
			? std::max(kMinDetSide, std::min(kMaxDetSide, options.det_limit_side_len))
			: 960;

		std::vector<DetBox> boxes;
		int detector_width = 0;
		int detector_height = 0;
		std::string error;
		if (!Detect(image, limit, boxes, detector_width, detector_height, error)) {
			result.diagnostic = error;
			return result;
		}

		double const scale_x = detector_width > 0 ? static_cast<double>(image.GetWidth()) / detector_width : 1.0;
		double const scale_y = detector_height > 0 ? static_cast<double>(image.GetHeight()) / detector_height : 1.0;

		std::vector<OCRBox> regions;
		for (DetBox const &box : boxes) {
			int x0 = std::max(0, static_cast<int>(std::floor(box.x * scale_x)));
			int y0 = std::max(0, static_cast<int>(std::floor(box.y * scale_y)));
			int x1 = std::min(image.GetWidth(), static_cast<int>(std::ceil((box.x + box.w) * scale_x)));
			int y1 = std::min(image.GetHeight(), static_cast<int>(std::ceil((box.y + box.h) * scale_y)));
			if (x1 - x0 < 2 || y1 - y0 < 2)
				continue;
			OCRBox region;
			region.x = x0;
			region.y = y0;
			region.w = x1 - x0;
			region.h = y1 - y0;
			region.score = box.score;
			regions.push_back(region);
		}

		if (mode == OcrMode::DetectOnly) {
			for (OCRBox const &region : regions)
				result.lines.push_back(MakeLine(region, std::string(), region.score));
			result.ok = true;
			return result;
		}

		std::vector<OCRLine> lines;
		for (OCRBox const &region : regions) {
			if (cancel && cancel()) {
				result.diagnostic = "cancelled";
				return result;
			}

			std::string text;
			double confidence = 0.0;
			if (!RecognizeLine(image, region, text, confidence, error)) {
				result.diagnostic = error;
				return result;
			}
			if (text.empty())
				continue;
			lines.push_back(MakeLine(region, text, confidence));
		}

		result.lines = std::move(lines);
		result.text = NormalizeText(result.lines, options.keep_line_breaks);
		result.ok = true;
		return result;
	}

private:
	struct OCRBox {
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
		double score = 0.0;
	};

	static OCRLine MakeLine(OCRBox const &box, std::string text, double confidence) {
		OCRLine line;
		line.text = std::move(text);
		line.confidence = confidence;
		line.box.push_back({ box.x, box.y });
		line.box.push_back({ box.x + box.w, box.y });
		line.box.push_back({ box.x + box.w, box.y + box.h });
		line.box.push_back({ box.x, box.y + box.h });
		return line;
	}

	std::string TakeStatus(OrtStatus *status) {
		std::string message = "The OCR runtime reported an error.";
		if (status) {
			char const *text = api ? api->GetErrorMessage(status) : nullptr;
			if (text)
				message = text;
			if (api)
				api->ReleaseStatus(status);
		}
		return message;
	}

	bool OpenSession(agi::fs::path const &model, int threads, Session &target, std::string &diagnostic) {
		OrtSessionOptions *options = nullptr;
		OrtStatus *status = api->CreateSessionOptions(&options);
		if (status) {
			diagnostic = TakeStatus(status);
			return false;
		}

		api->SetIntraOpNumThreads(options, threads);
		api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);

#ifdef _WIN32
		std::wstring const model_path = model.wstring();
#else
		std::string const model_path = model.string();
#endif
		OrtSession *session = nullptr;
		status = api->CreateSession(env, model_path.c_str(), options, &session);
		api->ReleaseSessionOptions(options);
		if (status) {
			diagnostic = "Cannot load " + model.string() + ": " + TakeStatus(status);
			return false;
		}
		target.handle = session;

		OrtAllocator *allocator = nullptr;
		status = api->GetAllocatorWithDefaultOptions(&allocator);
		if (status) {
			diagnostic = TakeStatus(status);
			return false;
		}

		char *name = nullptr;
		status = api->SessionGetInputName(session, 0, allocator, &name);
		if (status) {
			diagnostic = TakeStatus(status);
			return false;
		}
		if (name) {
			target.input_name = name;
			api->AllocatorFree(allocator, name);
		}

		name = nullptr;
		status = api->SessionGetOutputName(session, 0, allocator, &name);
		if (status) {
			diagnostic = TakeStatus(status);
			return false;
		}
		if (name) {
			target.output_name = name;
			api->AllocatorFree(allocator, name);
		}

		if (target.input_name.empty() || target.output_name.empty()) {
			diagnostic = "The OCR model " + model.string() + " has no named input or output.";
			return false;
		}
		return true;
	}

	bool RunFloat(Session const &session, std::vector<float> &input, std::vector<int64_t> const &shape,
	              std::vector<float> &output, std::vector<int64_t> &output_shape, std::string &error) {
		if (!session.handle || !memory) {
			error = "The OCR runtime is not initialized.";
			return false;
		}

		OrtValue *input_value = nullptr;
		OrtStatus *status = api->CreateTensorWithDataAsOrtValue(
			memory, input.data(), input.size() * sizeof(float), shape.data(), shape.size(),
			ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_value);
		if (status) {
			error = TakeStatus(status);
			return false;
		}

		char const *input_names[] = { session.input_name.c_str() };
		char const *output_names[] = { session.output_name.c_str() };
		OrtValue *output_value = nullptr;
		status = api->Run(session.handle, nullptr, input_names, &input_value, 1,
		                  output_names, 1, &output_value);
		api->ReleaseValue(input_value);
		if (status) {
			error = TakeStatus(status);
			return false;
		}

		void *data = nullptr;
		status = api->GetTensorMutableData(output_value, &data);
		if (status) {
			error = TakeStatus(status);
			api->ReleaseValue(output_value);
			return false;
		}

		OrtTensorTypeAndShapeInfo *info = nullptr;
		status = api->GetTensorTypeAndShape(output_value, &info);
		if (status) {
			error = TakeStatus(status);
			api->ReleaseValue(output_value);
			return false;
		}

		size_t rank = 0;
		api->GetDimensionsCount(info, &rank);
		output_shape.assign(rank, 0);
		if (rank > 0)
			api->GetDimensions(info, output_shape.data(), rank);
		api->ReleaseTensorTypeAndShapeInfo(info);

		size_t total = 1;
		for (int64_t dimension : output_shape)
			total *= static_cast<size_t>(dimension > 0 ? dimension : 0);
		if (total == 0 || !data) {
			api->ReleaseValue(output_value);
			error = "The OCR model returned an empty tensor.";
			return false;
		}

		float const *values = static_cast<float const *>(data);
		output.assign(values, values + total);
		api->ReleaseValue(output_value);
		return true;
	}

	bool Detect(wxImage const &image, int limit, std::vector<DetBox> &boxes,
	            int &content_width, int &content_height, std::string &error) {
		int const source_width = image.GetWidth();
		int const source_height = image.GetHeight();
		if (source_width <= 0 || source_height <= 0) {
			error = "The OCR input image is empty.";
			return false;
		}

		double scale = 1.0;
		int const longest = std::max(source_width, source_height);
		if (longest > limit)
			scale = static_cast<double>(limit) / longest;

		content_width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
		content_height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
		// Both sides of a DB input must be multiples of 32; the extra rows and
		// columns are zero padded (which is the mean colour once normalized).
		int const padded_width = std::max(32, ((content_width + 31) / 32) * 32);
		int const padded_height = std::max(32, ((content_height + 31) / 32) * 32);

		wxImage scaled = image.Scale(content_width, content_height, wxIMAGE_QUALITY_BILINEAR);
		if (!scaled.IsOk()) {
			error = "The OCR input image could not be resized.";
			return false;
		}

		size_t const plane = static_cast<size_t>(padded_width) * padded_height;
		std::vector<float> input(plane * 3, 0.0f);
		unsigned char const *rgb = scaled.GetData();
		if (!rgb) {
			error = "The OCR input image has no pixel data.";
			return false;
		}

		for (int y = 0; y < content_height; ++y) {
			for (int x = 0; x < content_width; ++x) {
				size_t const source = (static_cast<size_t>(y) * content_width + x) * 3;
				size_t const target = static_cast<size_t>(y) * padded_width + x;
				// The model takes BGR; the means and deviations are per channel.
				input[target] = (rgb[source + 2] / 255.0f - 0.485f) / 0.229f;
				input[plane + target] = (rgb[source + 1] / 255.0f - 0.456f) / 0.224f;
				input[plane * 2 + target] = (rgb[source] / 255.0f - 0.406f) / 0.225f;
			}
		}

		std::vector<int64_t> const shape{ 1, 3, padded_height, padded_width };
		std::vector<float> output;
		std::vector<int64_t> output_shape;
		if (!RunFloat(detector, input, shape, output, output_shape, error))
			return false;

		int out_height = padded_height;
		int out_width = padded_width;
		if (output_shape.size() == 4) {
			out_height = static_cast<int>(output_shape[2]);
			out_width = static_cast<int>(output_shape[3]);
		}
		if (out_height <= 0 || out_width <= 0) {
			error = "The OCR detector returned an empty probability map.";
			return false;
		}

		DetPostprocessOptions const postprocess;
		boxes = onnx::GroupDetectorOutput(output.data(), out_width, out_height, postprocess);
		return true;
	}

	bool RecognizeLine(wxImage const &image, OCRBox const &box, std::string &text,
	                   double &confidence, std::string &error) {
		int const margin_x = std::max(1, box.w / 40);
		int const margin_y = std::max(1, box.h / 10);
		int const left = std::max(0, box.x - margin_x);
		int const top = std::max(0, box.y - margin_y);
		int const right = std::min(image.GetWidth(), box.x + box.w + margin_x);
		int const bottom = std::min(image.GetHeight(), box.y + box.h + margin_y);
		int const width = right - left;
		int const height = bottom - top;
		if (width < 2 || height < 2)
			return true;

		wxImage crop = image.GetSubImage(wxRect(left, top, width, height));
		if (!crop.IsOk())
			return true;

		int target_width = static_cast<int>(std::lround(
			static_cast<double>(kRecHeight) * width / height));
		target_width = std::max(1, std::min(kMaxRecWidth, target_width));
		int const padded_width = std::min(kMaxRecWidth, std::max(32, ((target_width + 31) / 32) * 32));

		wxImage scaled = crop.Scale(target_width, kRecHeight, wxIMAGE_QUALITY_BILINEAR);
		if (!scaled.IsOk()) {
			error = "The OCR text line could not be resized.";
			return false;
		}

		size_t const plane = static_cast<size_t>(kRecHeight) * padded_width;
		std::vector<float> input(plane * 3, 0.0f);
		unsigned char const *rgb = scaled.GetData();
		if (!rgb) {
			error = "The OCR text line has no pixel data.";
			return false;
		}

		for (int y = 0; y < kRecHeight; ++y) {
			for (int x = 0; x < target_width; ++x) {
				size_t const source = (static_cast<size_t>(y) * target_width + x) * 3;
				size_t const target = static_cast<size_t>(y) * padded_width + x;
				input[target] = rgb[source + 2] / 127.5f - 1.0f;
				input[plane + target] = rgb[source + 1] / 127.5f - 1.0f;
				input[plane * 2 + target] = rgb[source] / 127.5f - 1.0f;
			}
		}

		std::vector<int64_t> const shape{ 1, 3, kRecHeight, padded_width };
		std::vector<float> output;
		std::vector<int64_t> output_shape;
		if (!RunFloat(recognizer, input, shape, output, output_shape, error))
			return false;

		if (output_shape.size() != 3) {
			error = "The OCR recognizer returned an unexpected tensor shape.";
			return false;
		}

		CtcDecoded const decoded = onnx::DecodeCtc(
			output.data(), static_cast<int>(output_shape[1]), static_cast<int>(output_shape[2]), dictionary);
		text = decoded.text;
		confidence = decoded.confidence;
		return true;
	}

	static bool LoadImage(agi::fs::path const &path, wxImage &image, std::string &error) {
		if (!agi::fs::FileExists(path)) {
			error = "The OCR input image does not exist: " + path.string();
			return false;
		}
		image.LoadFile(wxString(path.wstring().c_str()), wxBITMAP_TYPE_ANY);
		if (!image.IsOk()) {
			error = "The OCR input image could not be decoded.";
			return false;
		}
		return true;
	}
};

} // namespace

std::unique_ptr<OcrBackend> MakeOnnxBackend() {
	return std::unique_ptr<OcrBackend>(new OnnxBackend());
}

} // namespace ocr
