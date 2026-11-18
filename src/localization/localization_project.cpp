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

#include "localization_project.h"

#include "charset_detect.h"
#include "compat.h"
#include "localization_loader.h"
#include "options.h"
#include "text_file_reader.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <wx/string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace localization {

std::string PathUtf8(agi::fs::path const& path) {
	wxString wide(path.wstring());
	auto utf8 = wide.utf8_str();
	if (!utf8.data()) return {};
	return std::string(utf8.data(), utf8.length());
}

std::string FileSignature(std::vector<agi::fs::path> const& paths) {
	std::string signature;
	for (auto const& path : paths) {
		signature += PathUtf8(path);
		signature += '\x1F';
		try {
			signature += std::to_string(
				static_cast<long long>(agi::fs::ModifiedTime(path)));
			signature += ':';
			signature += std::to_string(
				static_cast<unsigned long long>(agi::fs::Size(path)));
		}
		catch (agi::Exception const&) {
			signature += '?';
		}
		signature += '\n';
	}
	return signature;
}

std::vector<agi::fs::path> ReadPersistedPaths() {
	std::vector<agi::fs::path> paths;
	std::string saved = OPT_GET("Tool/Localization/Files")->GetString();
	size_t start = 0;
	while (start <= saved.size()) {
		size_t end = saved.find('\n', start);
		if (end == std::string::npos) end = saved.size();
		std::string item = saved.substr(start, end - start);
		if (!item.empty()) {
			wxString wide = to_wx(item);
			agi::fs::path path(std::wstring(wide.wc_str()));
			if (agi::fs::FileExists(path))
				paths.push_back(path);
		}
		if (end == saved.size()) break;
		start = end + 1;
	}
	return paths;
}

std::string ReadFileText(agi::fs::path const& path, std::string& error) {
	try {
		std::string encoding = CharSetDetect::GetEncoding(path);
		TextFileReader reader(path, encoding, false);
		std::string content;
		while (reader.HasMoreLines()) {
			content += reader.ReadLineFromFile();
			content += '\n';
		}
		return content;
	}
	catch (agi::Exception const& e) {
		error = e.GetMessage();
		return {};
	}
}

std::map<std::string, FileRole> ReadFileRoles() {
	std::map<std::string, FileRole> roles;
	std::string saved = OPT_GET("Tool/Localization/Roles")->GetString();
	size_t start = 0;
	while (start <= saved.size()) {
		size_t end = saved.find('\n', start);
		if (end == std::string::npos) end = saved.size();
		std::string line = saved.substr(start, end - start);
		size_t tab = line.find('\t');
		if (tab != std::string::npos) {
			std::string path = line.substr(0, tab);
			std::string role = line.substr(tab + 1);
			if (role == "source")
				roles[path] = FileRole::Source;
			else if (role == "target")
				roles[path] = FileRole::Target;
		}
		if (end == saved.size()) break;
		start = end + 1;
	}
	return roles;
}

void SaveFileRoles(std::map<std::string, FileRole> const& roles) {
	std::string joined;
	for (auto const& role : roles) {
		if (role.second == FileRole::Auto) continue;
		if (!joined.empty()) joined += '\n';
		joined += role.first;
		joined += '\t';
		joined += role.second == FileRole::Source ? "source" : "target";
	}
	OPT_SET("Tool/Localization/Roles")->SetString(joined);
}

std::vector<LocalizationFile> LoadFiles(
	std::vector<agi::fs::path> const& paths,
	std::map<std::string, FileRole> const& roles) {
	std::map<std::string, int> basename_counts;
	for (auto const& path : paths)
		++basename_counts[PathUtf8(path.filename())];

	std::vector<LocalizationFile> files;
	files.reserve(paths.size());
	for (auto const& path : paths) {
		LocalizationFile file;
		std::string basename = PathUtf8(path.filename());
		std::string display = basename;
		if (basename_counts[basename] > 1) {
			std::string parent = PathUtf8(path.parent_path().filename());
			if (!parent.empty())
				display = parent + "/" + basename;
		}

		std::string full_path = PathUtf8(path);
		auto role = roles.find(full_path);
		FileRole file_role = role == roles.end() ? FileRole::Auto : role->second;

		std::string error;
		std::string content = ReadFileText(path, error);
		if (!error.empty()) {
			file.name = display;
			file.role = file_role;
			file.ok = false;
			file.error = error;
		}
		else {
			file = LoadContent(content, display, PathUtf8(path.extension()));
			file.role = file_role;
		}
		files.push_back(std::move(file));
	}
	return files;
}

MatchOptions ReadMatchOptions() {
	MatchOptions options;
	options.fuzzy = OPT_GET("Tool/Localization/Fuzzy")->GetBool();
	options.ignore_tags = OPT_GET("Tool/Localization/Ignore Tags")->GetBool();
	options.ignore_punctuation =
		OPT_GET("Tool/Localization/Ignore Punctuation")->GetBool();
	options.ignore_case = OPT_GET("Tool/Localization/Ignore Case")->GetBool();
	options.threshold = OPT_GET("Tool/Localization/Threshold")->GetDouble();
	options.preferred_language =
		OPT_GET("Tool/Localization/Language")->GetString();
	options.split_sentences =
		OPT_GET("Tool/Localization/Split Sentences")->GetBool();
	options.split_regex = OPT_GET("Tool/Localization/Split Regex")->GetString();
	options.prefer_without_split_regex =
		OPT_GET("Tool/Localization/Prefer Without Split Regex")->GetBool();
	options.hide_source_language =
		OPT_GET("Tool/Localization/Hide Source Language")->GetBool();
	options.pair_unkeyed_files = true;
	options.use_project_tm =
		OPT_GET("Tool/Localization/Use Project TM")->GetBool();
	options.key_regex = OPT_GET("Tool/Localization/Key Regex")->GetString();
	return options;
}

void SaveMatchOptions(MatchOptions const& options) {
	OPT_SET("Tool/Localization/Fuzzy")->SetBool(options.fuzzy);
	OPT_SET("Tool/Localization/Ignore Tags")->SetBool(options.ignore_tags);
	OPT_SET("Tool/Localization/Ignore Punctuation")->SetBool(
		options.ignore_punctuation);
	OPT_SET("Tool/Localization/Ignore Case")->SetBool(options.ignore_case);
	OPT_SET("Tool/Localization/Threshold")->SetDouble(options.threshold);
	OPT_SET("Tool/Localization/Language")->SetString(options.preferred_language);
	OPT_SET("Tool/Localization/Split Sentences")->SetBool(
		options.split_sentences);
	OPT_SET("Tool/Localization/Split Regex")->SetString(options.split_regex);
	OPT_SET("Tool/Localization/Prefer Without Split Regex")->SetBool(
		options.prefer_without_split_regex);
	OPT_SET("Tool/Localization/Hide Source Language")->SetBool(
		options.hide_source_language);
	OPT_SET("Tool/Localization/Use Project TM")->SetBool(
		options.use_project_tm);
	OPT_SET("Tool/Localization/Key Regex")->SetString(options.key_regex);
}

} // namespace localization
