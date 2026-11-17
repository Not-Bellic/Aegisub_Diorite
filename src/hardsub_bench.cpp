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

#include "hardsub_bench.h"

#include "hardsub_ocr_runner.h"
#include "hardsub_timeline_scan.h"
#include "ocr/ocr_backend.h"
#include "scan_video_decoder.h"

#include <libaegisub/io.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <wx/image.h>
#include <wx/string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace hardsub {
namespace {

using Clock = std::chrono::steady_clock;

/// Console progress goes to stderr, the report to stdout; a crash before the
/// report therefore still shows the last step that ran.
void Trace(char const *step) {
	std::fprintf(stderr, "[bench] %s\n", step);
	std::fflush(stderr);
}

struct Request {
	bool requested = false;
	bool no_ocr = false;
	bool hardware_decode = false;
	ocr::OcrBackendKind backend = ocr::OcrBackendKind::Auto;
	std::string video;
	agi::fs::path out;
	int region_x = -1;
	int region_y = -1;
	int region_w = -1;
	int region_h = -1;
	int scale = 100;
	int start = 0;
	int count = -1;
};

agi::fs::path ToPath(std::string const &utf8) {
	return agi::fs::path(wxString::FromUTF8(utf8.c_str()).ToStdWstring());
}

std::string EscapeJson(std::string const &text) {
	std::string out;
	out.reserve(text.size() + 8);
	for (unsigned char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
				out += buffer;
			}
			else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	return out;
}

std::string Number(double value, char const *format) {
	char buffer[32];
	std::snprintf(buffer, sizeof buffer, format, value);
	return buffer;
}

bool ParseList(std::string const &text, int *values, int count) {
	size_t pos = 0;
	for (int i = 0; i < count; ++i) {
		if (pos >= text.size())
			return false;
		size_t comma = text.find(',', pos);
		std::string part = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		if (part.empty())
			return false;
		values[i] = std::atoi(part.c_str());
		if (comma == std::string::npos)
			pos = text.size();
		else
			pos = comma + 1;
	}
	return true;
}

Request Parse(std::vector<std::string> const &arguments) {
	Request request;
	for (size_t i = 0; i < arguments.size(); ++i) {
		std::string arg = arguments[i];
		std::string value;
		bool have_value = false;

		// Both `--out file` and `--out=file` are accepted.
		if (arg.rfind("--", 0) == 0) {
			size_t const equals = arg.find('=');
			if (equals != std::string::npos) {
				value = arg.substr(equals + 1);
				arg.resize(equals);
				have_value = true;
			}
		}

		// An option value never starts with --, so a missing value does not
		// swallow the next option.
		auto take_value = [&]() -> bool {
			if (have_value)
				return true;
			if (i + 1 >= arguments.size())
				return false;
			if (arguments[i + 1].rfind("--", 0) == 0)
				return false;
			value = arguments[++i];
			return true;
		};

		if (arg == "--hardsub-bench") {
			request.requested = true;
			continue;
		}
		if (arg == "--no-ocr") {
			request.no_ocr = true;
			continue;
		}
		if (arg == "--hardware-decode") {
			request.hardware_decode = true;
			continue;
		}
		if (arg == "--region") {
			int values[4] = {0, 0, 0, 0};
			if (take_value() && ParseList(value, values, 4)) {
				request.region_x = values[0];
				request.region_y = values[1];
				request.region_w = values[2];
				request.region_h = values[3];
			}
			continue;
		}
		if (arg == "--scale") {
			if (take_value())
				request.scale = std::atoi(value.c_str());
			continue;
		}
		if (arg == "--start") {
			if (take_value())
				request.start = std::atoi(value.c_str());
			continue;
		}
		if (arg == "--count") {
			if (take_value())
				request.count = std::atoi(value.c_str());
			continue;
		}
		if (arg == "--backend") {
			if (take_value()) {
				ocr::OcrBackendKind kind;
				if (ocr::ParseBackendKind(value, kind))
					request.backend = kind;
			}
			continue;
		}
		if (arg == "--out") {
			if (take_value())
				request.out = ToPath(value);
			continue;
		}
		if (arg.rfind("--", 0) == 0)
			continue; // another Aegisub option; not ours
		if (request.video.empty() && !arg.empty())
			request.video = arg;
	}
	return request;
}

void AttachConsoleIfNeeded() {
#ifdef _WIN32
	// A GUI build owns no console, so borrow the parent's to print the report.
	// A redirected stdout (a pipe or a file) must be left alone.
	HANDLE const handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD const type = handle ? ::GetFileType(handle) : FILE_TYPE_UNKNOWN;
	if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK)
		return;

	if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
		std::FILE *stream = nullptr;
		freopen_s(&stream, "CONOUT$", "w", stdout);
	}
#endif
}

[[noreturn]] void Terminate(int code) {
#ifdef _WIN32
	// The application is only half initialized here; running the C runtime's
	// atexit handlers on top of a partial wxWidgets setup is what used to kill
	// the OCR self-test mode.
	::ExitProcess(static_cast<UINT>(code));
#else
	std::exit(code);
#endif
}

std::string BuildReport(Request const &request, int width, int height, int frame_count,
                        long long wall_ms, hardsub::TimelineScanResult const &result,
                        hardsub::ScanVideoDecoder::Stats const &decoder,
                        std::string const &engine, std::string const &error) {
	ScanProfile const &profile = result.profile;
	long long const total = std::max<long long>(
		1, profile.ms_decode + profile.ms_region + profile.ms_change +
		profile.OcrMs() + profile.ms_ocr_image + profile.ms_boundary);
	auto pct = [total](long long ms) { return 100.0 * ms / total; };

	std::string json = "{\n";
	json += "  \"ok\": ";
	json += (result.ok && error.empty()) ? "true" : "false";
	json += ",\n  \"error\": \"" + EscapeJson(error) + "\"";
	json += ",\n  \"video\": \"" + EscapeJson(request.video) + "\"";
	json += ",\n  \"width\": " + std::to_string(width);
	json += ",\n  \"height\": " + std::to_string(height);
	json += ",\n  \"frame_count\": " + std::to_string(frame_count);
	json += ",\n  \"region\": [" + std::to_string(request.region_x) + ", " +
		std::to_string(request.region_y) + ", " + std::to_string(request.region_w) + ", " +
		std::to_string(request.region_h) + "]";
	json += ",\n  \"scale\": " + std::to_string(request.scale);
	json += ",\n  \"engine\": \"" + EscapeJson(engine) + "\"";
	json += ",\n  \"wall_ms\": " + Number(static_cast<double>(wall_ms), "%.0f");
	json += ",\n  \"frames_evaluated\": " + std::to_string(result.frames_evaluated);
	json += ",\n  \"rows\": " + std::to_string(profile.rows);
	json += ",\n  \"events\": " + std::to_string(result.events.size());
	json += ",\n  \"ocr_detect_calls\": " + std::to_string(profile.ocr_detect_calls);
	json += ",\n  \"ocr_recognize_calls\": " + std::to_string(profile.ocr_recognize_calls);
	json += ",\n  \"boundary_frames\": " + std::to_string(profile.boundary_frames);
	json += ",\n  \"decoder\": {\"frames\": " + std::to_string(decoder.frames) +
		", \"decode_ms\": " + std::to_string(decoder.MsDecode()) +
		", \"copy_ms\": " + std::to_string(decoder.MsCopy()) + "}";
	json += ",\n  \"ms\": {\"decode\": " + std::to_string(profile.ms_decode) +
		", \"region\": " + std::to_string(profile.ms_region) +
		", \"change\": " + std::to_string(profile.ms_change) +
		", \"ocr_engine\": " + std::to_string(profile.OcrMs()) +
		", \"ocr_image\": " + std::to_string(profile.ms_ocr_image) +
		", \"boundary\": " + std::to_string(profile.ms_boundary);
	json += "},\n  \"share\": {\"decode\": " + Number(pct(profile.ms_decode), "%.1f") +
		", \"pixels\": " + Number(pct(profile.ms_region + profile.ms_change), "%.1f") +
		", \"ocr\": " + Number(pct(profile.OcrMs() + profile.ms_ocr_image), "%.1f") +
		", \"rows\": " + Number(pct(profile.ms_boundary), "%.1f") + "}";

	json += ",\n  \"first_events\": [";
	size_t const shown = std::min<size_t>(result.events.size(), 10);
	for (size_t i = 0; i < shown; ++i) {
		if (i)
			json += ",";
		auto const &ev = result.events[i];
		json += "\n    {\"start\": " + std::to_string(ev.start_frame) +
			", \"end\": " + std::to_string(ev.end_frame) +
			", \"text\": \"" + EscapeJson(ev.text) + "\"}";
	}
	if (shown)
		json += "\n  ";
	json += "]\n}\n";
	return json;
}

} // namespace

bool BenchRequested(std::vector<std::string> const &arguments) {
	for (auto const &argument : arguments) {
		if (argument == "--hardsub-bench")
			return true;
	}
	return false;
}

[[noreturn]] void RunBench(std::vector<std::string> const &arguments) {
	Request const request = Parse(arguments);

	AttachConsoleIfNeeded();

	if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
		wxImage::AddHandler(new wxPNGHandler);

	std::string report;
	int code = 2;

	if (request.video.empty()) {
		report = "{\n  \"ok\": false,\n  \"error\": \"usage: aegisub --hardsub-bench <video> "
		         "[--region x,y,w,h] [--scale 50] [--start N] [--count N] "
		         "[--backend auto|onnx|paddle] [--no-ocr] [--out report.json]\"\n}\n";
	}
	else {
		Trace("open video");
		std::string decoder_error;
		hardsub::ScanDecoderSettings settings;
		settings.hardware_decoding = request.hardware_decode;
		hardsub::ScanVideoDecoder decoder(ToPath(request.video), decoder_error, settings);
		if (!decoder.Valid()) {
			report = "{\n  \"ok\": false,\n  \"error\": \"" + EscapeJson(decoder_error) + "\"\n}\n";
		}
		else {
			int const width = decoder.GetWidth();
			int const height = decoder.GetHeight();

			hardsub::TimelineScanOptions options;
			options.region_x = request.region_x >= 0 ? request.region_x : 0;
			options.region_y = request.region_y >= 0 ? request.region_y : height * 4 / 5;
			options.region_w = request.region_w > 0 ? request.region_w : width;
			options.region_h = request.region_h > 0 ? request.region_h : std::max(1, height - options.region_y);
			options.frame_count = decoder.GetFrameCount();
			options.frame_start = std::max(0, request.start);
			options.frame_end = request.count > 0
				? std::min(options.frame_count - 1, options.frame_start + request.count - 1)
				: options.frame_count - 1;
			options.scan_scale = std::max(1, std::min(100, request.scale)) / 100.0;

			auto profile = std::make_shared<hardsub::ScanProfile>();
			options.profile = profile;

			auto loader = hardsub::FrameLoader(
				[&decoder](int f) { return decoder.GetFrame(f); });

			ocr::OCROptions ocr_options;
			ocr_options.keep_line_breaks = true;
			std::string engine;
			std::shared_ptr<ocr::OcrBackend> backend;
			if (!request.no_ocr) {
				Trace("start OCR engine");
				backend = ocr::MakeBackend(request.backend, ocr::OCRProcessConfig{true, true, false});
				std::string diagnostic;
				if (!backend || !backend->Start(diagnostic)) {
					report = "{\n  \"ok\": false,\n  \"error\": \"" +
						EscapeJson(diagnostic.empty() ? std::string("The OCR engine could not be started.") : diagnostic) +
						"\"\n}\n";
					std::fputs(report.c_str(), stdout);
					std::fflush(stdout);
					Terminate(code);
				}
				engine = backend->Name();
			}

			// Without OCR the callables stay empty: the scan then skips the OCR
			// branch entirely instead of preparing an image it would discard, so
			// --no-ocr measures the pixel path on its own.
			hardsub::RegionDetector detect;
			hardsub::RegionRecognizer recognize;
			if (!request.no_ocr) {
				detect = hardsub::MakeOcrDetector(backend, ocr_options, hardsub::CancelFn(),
				                                  profile.get());
				recognize = hardsub::MakeOcrRecognizer(backend, ocr_options, hardsub::CancelFn(),
				                                       profile.get());
			}

			int last_percent = -100;
			auto started = Clock::now();
			auto progress = [&last_percent](ScanPhase phase, int done, int total) {
				int const percent = total > 0 ? done * 100 / total : 0;
				if (phase == ScanPhase::Rows || percent >= last_percent + 10) {
					last_percent = percent;
					std::fprintf(stderr, "[%s] %d/%d (%d%%)\n",
					             phase == ScanPhase::Main ? "main" : "rows",
					             done, total, percent);
				}
			};

			hardsub::TimelineScanResult result;
			try {
				Trace("scan");
				result = hardsub::ScanVideoTimeline(options, loader, detect, recognize,
				                                    hardsub::CancelFn(), progress,
				                                    hardsub::RowCallback());
			}
			catch (std::exception const &e) {
				result.ok = false;
				result.error = e.what();
			}

			Trace("report");
			long long const wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				Clock::now() - started).count();
			report = BuildReport(request, width, height, options.frame_count, wall_ms, result,
			                     decoder.GetStats(), engine, result.error);
			code = result.ok ? 0 : 1;

			if (backend)
				backend->Stop();
		}
	}

	std::fputs(report.c_str(), stdout);
	std::fflush(stdout);

	if (!request.out.empty()) {
		try {
			agi::io::Save save(request.out);
			save.Get() << report;
		}
		catch (std::exception const &) {
			code = 2;
		}
	}

	Terminate(code);
}

} // namespace hardsub
