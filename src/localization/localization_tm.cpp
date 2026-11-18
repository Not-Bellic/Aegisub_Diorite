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

#include "localization_tm.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <istream>
#include <ostream>
#include <sstream>
#include <string>

namespace localization {

void TranslationMemory::Load(agi::fs::path const& path) {
	entries.clear();
	try {
		if (!agi::fs::FileExists(path)) return;
		auto stream = agi::io::Open(path);
		std::string line;
		while (std::getline(*stream, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			size_t tab = line.find('\t');
			if (tab == std::string::npos) continue;
			std::string source = line.substr(0, tab);
			std::string target = line.substr(tab + 1);
			if (!source.empty() && !target.empty())
				entries.emplace_back(std::move(source), std::move(target));
		}
	}
	catch (agi::Exception const&) {
		entries.clear();
	}
}

void TranslationMemory::Append(std::string source, std::string target) {
	if (source.empty() || target.empty() || source == target) return;
	for (auto const& entry : entries)
		if (entry.first == source && entry.second == target)
			return;
	entries.emplace_back(std::move(source), std::move(target));
}

void TranslationMemory::Save(agi::fs::path const& path) const {
	agi::io::Save out(path);
	for (auto const& entry : entries)
		out.Get() << entry.first << '\t' << entry.second << '\n';
}

LocalizationFile TranslationMemory::BuildFile(std::string const& name,
	std::string const& language) const {
	LocalizationFile file;
	file.name = name;
	file.language = language;
	file.role = FileRole::Target;
	file.synthetic = true;
	for (auto const& entry : entries)
		file.pairs.push_back({"", entry.first, entry.second, name});
	return file;
}

} // namespace localization
