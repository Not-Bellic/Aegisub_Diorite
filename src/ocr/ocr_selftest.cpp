// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "ocr_selftest.h"

#include "ocr_backend.h"

#include <libaegisub/io.h>

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

namespace ocr {
namespace {

struct Request {
	bool requested = false;
	bool detect_only = false;
	OcrBackendKind backend = OcrBackendKind::Auto;
	agi::fs::path image;
	agi::fs::path out;
};

agi::fs::path ToPath(std::string const &utf8) {
	return agi::fs::path(wxString::FromUTF8(utf8.c_str()).ToStdWstring());
}

/// Locale-independent UTF-8 form of a path, for the JSON report.
std::string Utf8(agi::fs::path const &path) {
	auto const utf8 = wxString(path.wstring()).ToUTF8();
	return std::string(utf8.data(), utf8.length());
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

double ElapsedMs(std::chrono::steady_clock::time_point from,
                 std::chrono::steady_clock::time_point to) {
	return std::chrono::duration<double, std::milli>(to - from).count();
}

std::string FormatNumber(double value, char const *format) {
	char buffer[32];
	std::snprintf(buffer, sizeof buffer, format, value);
	return buffer;
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

		if (arg == "--ocr-selftest") {
			request.requested = true;
			continue;
		}
		if (arg == "--detect-only") {
			request.detect_only = true;
			continue;
		}
		if (arg == "--out") {
			if (take_value())
				request.out = ToPath(value);
			continue;
		}
		if (arg == "--backend") {
			if (take_value()) {
				OcrBackendKind kind;
				if (ParseBackendKind(value, kind))
					request.backend = kind;
			}
			continue;
		}
		if (arg.rfind("--", 0) == 0)
			continue; // another Aegisub option; not ours
		if (request.image.empty() && !arg.empty())
			request.image = ToPath(arg);
	}

	return request;
}

std::string Usage() {
	return "{\n  \"ok\": false,\n  \"diagnostic\": \"The OCR self-test needs an image: "
	       "aegisub --ocr-selftest <image> [--out <report.json>] [--backend auto|onnx|paddle]\"\n}\n";
}

std::string BuildReport(Request const &request, OCRResult const &result, double start_ms,
                        double run_ms, bool started, std::string const &start_diagnostic) {
	std::string json = "{\n";
	json += "  \"ok\": ";
	json += result.ok ? "true" : "false";
	json += ",\n  \"backend\": \"";
	json += BackendKindId(request.backend);
	json += "\",\n  \"engine_started\": ";
	json += started ? "true" : "false";
	json += ",\n  \"mode\": \"";
	json += request.detect_only ? "detect_only" : "full";
	json += "\",\n  \"image\": \"";
	json += EscapeJson(Utf8(request.image));
	json += "\",\n  \"start_ms\": ";
	json += FormatNumber(start_ms, "%.1f");
	json += ",\n  \"run_ms\": ";
	json += FormatNumber(run_ms, "%.1f");
	json += ",\n  \"text\": \"";
	json += EscapeJson(result.text);
	json += "\",\n  \"lines\": [";

	for (size_t i = 0; i < result.lines.size(); ++i) {
		OCRLine const &line = result.lines[i];
		if (i)
			json += ",";
		json += "\n    {\"text\": \"";
		json += EscapeJson(line.text);
		json += "\", \"confidence\": ";
		json += FormatNumber(line.confidence, "%.4f");
		json += ", \"box\": [";
		for (size_t point = 0; point < line.box.size(); ++point) {
			if (point)
				json += ", ";
			json += "[" + std::to_string(line.box[point].first) + ", " +
				std::to_string(line.box[point].second) + "]";
		}
		json += "]}";
	}
	if (!result.lines.empty())
		json += "\n  ";
	json += "],\n  \"diagnostic\": \"";
	json += EscapeJson(result.diagnostic.empty() ? start_diagnostic : result.diagnostic);
	json += "\"\n}\n";
	return json;
}

void AttachConsoleIfNeeded() {
#ifdef _WIN32
	// A GUI build owns no console, so borrow the parent's to print the report.
	// A redirected stdout (a pipe or a file, as CI uses) must be left alone:
	// reopening CONOUT$ there would send the report to the log window instead.
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
	// ExitProcess rather than std::exit: the application is only half
	// initialized here, and running the C runtime's atexit handlers on top of
	// a partial wxWidgets setup is exactly what used to kill this mode.
	::ExitProcess(static_cast<UINT>(code));
#else
	std::exit(code);
#endif
}

} // namespace

bool SelfTestRequested(std::vector<std::string> const &arguments) {
	for (auto const &argument : arguments) {
		if (argument == "--ocr-selftest")
			return true;
	}
	return false;
}

[[noreturn]] void RunSelfTest(std::vector<std::string> const &arguments) {
	Request const request = Parse(arguments);

	AttachConsoleIfNeeded();

	// The engine decodes images through wxImage, and the normal startup that
	// registers the PNG handler has not run in this mode.
	if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
		wxImage::AddHandler(new wxPNGHandler);

	std::string report;
	int code = 2;

	if (request.image.empty()) {
		report = Usage();
	}
	else {
		auto backend = MakeBackend(request.backend, OCRProcessConfig{true, true, false});
		std::string start_diagnostic;
		auto const start_begin = std::chrono::steady_clock::now();
		bool const started = backend && backend->Start(start_diagnostic);
		double const start_ms = ElapsedMs(start_begin, std::chrono::steady_clock::now());

		OCRResult result;
		double run_ms = 0.0;
		if (started) {
			auto const run_begin = std::chrono::steady_clock::now();
			result = backend->Run(request.image, OCROptions{},
			                      request.detect_only ? OcrMode::DetectOnly : OcrMode::Full);
			run_ms = ElapsedMs(run_begin, std::chrono::steady_clock::now());
			backend->Stop();
		}
		else {
			result.diagnostic = start_diagnostic;
		}

		report = BuildReport(request, result, start_ms, run_ms, started, start_diagnostic);
		if (!result.ok)
			code = 2;
		else if (!result.lines.empty())
			code = 0;
		else
			code = 1;
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

} // namespace ocr
