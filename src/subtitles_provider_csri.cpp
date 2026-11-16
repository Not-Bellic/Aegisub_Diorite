// Copyright (c) 2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file subtitles_provider_csri.cpp
/// @brief Wrapper for CSRI-based subtitle renderers
/// @ingroup subtitle_rendering
///

#ifdef WITH_CSRI
#include "subtitles_provider_csri.h"

#include "include/aegisub/subtitles_provider.h"
#include "options.h"
#include "subtitle_format_ass.h"
#include "video_frame.h"

#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>

#include <mutex>

#ifdef WIN32
#define CSRIAPI
#endif

#include <csri/csri.h>
#include <csri/logging.h>

namespace {
// CSRI renderers are not required to be thread safe (and VSFilter very much
// is not)
std::mutex csri_mutex;

// CSRI reports plugin discovery and load failures through an optional logging
// extension. Without a callback those messages go to stderr and are lost.
void CSRI_log_callback(void *, csri_logging_severity severity, const char *message) {
	switch (severity) {
	case CSRI_LOG_ERROR: LOG_E("subtitle/provider/csri") << message; break;
	case CSRI_LOG_WARNING: LOG_W("subtitle/provider/csri") << message; break;
	case CSRI_LOG_NOTICE:
	case CSRI_LOG_INFO: LOG_D("subtitle/provider/csri") << message; break;
	default: break;
	}
}

// Must run before the first enumeration so plugin load failures reach the log.
void InitCSRILogging() {
	static std::once_flag once;
	std::call_once(once, [] {
		auto *ext = static_cast<csri_logging_ext *>(csri_query_ext(nullptr, CSRI_EXT_LOGGING));
		if (ext && ext->set_logcallback)
			ext->set_logcallback(CSRI_log_callback, nullptr);
	});
}

struct closer {
	void operator()(csri_inst *inst) { if (inst) csri_close(inst); }
};

class CSRISubtitlesProvider final : public SubtitlesProvider {
	std::unique_ptr<csri_inst, closer> instance;
	csri_rend *renderer = nullptr;

	void LoadSubtitles(const char *data, size_t len) override {
		std::lock_guard<std::mutex> lock(csri_mutex);
		instance.reset(csri_open_mem(renderer, data, len, nullptr));
	}

public:
	CSRISubtitlesProvider(std::string subType);

	void DrawSubtitles(VideoFrame &dst, double time) override;
};

CSRISubtitlesProvider::CSRISubtitlesProvider(std::string type) {
	std::lock_guard<std::mutex> lock(csri_mutex);
	InitCSRILogging();
	for (csri_rend *cur = csri_renderer_default(); cur; cur = csri_renderer_next(cur)) {
		if (type == csri_renderer_info(cur)->name) {
			renderer = cur;
			break;
		}
	}

	if (!renderer)
		throw agi::InternalError("renderer is not registered by the CSRI library");
}

void CSRISubtitlesProvider::DrawSubtitles(VideoFrame &dst, double time) {
	if (!instance) return;

	csri_frame frame;
	if (dst.flipped) {
		frame.planes[0] = dst.data.data() + (dst.height-1) * dst.width * 4;
		frame.strides[0] = -(signed)dst.width * 4;
	}
	else {
		frame.planes[0] = dst.data.data();
		frame.strides[0] = dst.width * 4;
	}
	frame.pixfmt = CSRI_F_BGR_;

	csri_fmt format = { frame.pixfmt, dst.width, dst.height };

	std::lock_guard<std::mutex> lock(csri_mutex);
	if (!csri_request_fmt(instance.get(), &format))
		csri_render(instance.get(), &frame, time + 1e-9);
}
}

namespace csri {
std::vector<std::string> List() {
	std::lock_guard<std::mutex> lock(csri_mutex);
	InitCSRILogging();
	std::vector<std::string> final;
	for (csri_rend *cur = csri_renderer_default(); cur; cur = csri_renderer_next(cur)) {
		std::string name(csri_renderer_info(cur)->name);
		if (name.find("aegisub") != name.npos)
			final.insert(final.begin(), name);
		else
			final.push_back(name);
	}
	return final;
}

std::string PluginDirectory() {
#ifdef _WIN32
	if (!config::path) return {};
	return config::path->Decode("?data/csri").string();
#else
	return {};
#endif
}

std::unique_ptr<SubtitlesProvider> Create(std::string const& name, agi::BackgroundRunner *) {
	return agi::make_unique<CSRISubtitlesProvider>(name);
}
}
#endif // WITH_CSRI
