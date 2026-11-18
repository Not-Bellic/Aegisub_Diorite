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

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace localization {

/// Opaque per-file normalization/segmentation cache built by Prepare().
struct PreparedFile;

/// Which side of a bilingual entry a text item came from. Plain (monolingual)
/// entries are Unknown; source-side items are only useful for locating the
/// entry key and must never be offered as a translation themselves.
enum class TextRole {
	Unknown,
	Source,
	Translation,
};

/// Role of a whole localization file. Auto keeps the normal text/key matching;
/// Source/Target let unkeyed monolingual files be paired by position.
enum class FileRole {
	Auto,
	Source,
	Target,
};

/// A single piece of text found in a localization file.
/// @param key Optional key/id of the entry (empty when the file has no keys)
/// @param text The text content
/// @param file Display name of the source file
/// @param role Source/translation side of a bilingual record
struct TextItem {
	std::string key;
	std::string text;
	std::string file;
	TextRole role = TextRole::Unknown;
};

/// A source -> translation pair found inside a single file.
struct BilingualRecord {
	std::string key;
	std::string source;
	std::string translation;
	std::string file;
};

/// The result of loading one localization file.
struct LocalizationFile {
	std::string name;
	/// Dominant language of the file text (e.g. "中文", "English", "日本語"),
	/// detected when the file is loaded. Empty when unknown.
	std::string language;
	std::vector<TextItem> items;
	std::vector<BilingualRecord> pairs;
	/// Explicit source/target role, used to pair unkeyed monolingual files.
	FileRole role = FileRole::Auto;
	/// True for in-memory helper files (project TM), which are never paired
	/// with localization files by position.
	bool synthetic = false;
	bool ok = true;
	std::string error;
	/// Cached normalized texts and segments; invalidated by Prepare() when the
	/// relevant matching options change.
	mutable std::shared_ptr<PreparedFile const> prepared;
};

/// Matching behavior options. Fuzzy matching, tag stripping, punctuation
/// stripping, case folding and same-language filtering all default to enabled.
struct MatchOptions {
	bool fuzzy = true;
	bool ignore_tags = true;
	bool ignore_punctuation = true;
	bool ignore_case = true;
	double threshold = 0.7;
	/// Split segments at sentence endings (。.!?…) before matching.
	bool split_sentences = true;
	/// Optional regular expression: every match becomes a segment boundary
	/// and the matched text is removed. Empty disables regex splitting.
	std::string split_regex;
	/// Optional regular expression used by the UI to extract entry keys from
	/// the line fields (Actor, Effect, Style, text). The first capture group
	/// is used, or the whole match when there is no group.
	std::string key_regex;
	/// When a split regex is configured, results whose replacement text
	/// contains no match of the regex are ranked first. Clean segments are
	/// usually the intended insertion text, while results that still contain
	/// the regex text (e.g. unsplit {TA7} tags) are less desirable.
	bool prefer_without_split_regex = true;
	/// Results whose file language matches this value are sorted first.
	/// Empty disables the preference.
	std::string preferred_language = "中文";
	/// Drop results whose file language equals the language of the subtitle
	/// line, unless it is the preferred (target) language, and only when a
	/// result in another language is available. Same-language text cannot
	/// translate the line and only adds noise when both the source and the
	/// translated file are loaded.
	bool hide_source_language = true;
	/// Pair unkeyed monolingual files by position when their entry counts and
	/// languages line up (or when they carry explicit Source/Target roles).
	bool pair_unkeyed_files = true;
	/// Include the current script's existing lines and the persistent
	/// translation memory when matching.
	bool use_project_tm = true;
};

/// Context for interactive matching: the last accepted result anchors nearby
/// entries (game scripts are usually in script order), and the current line's
/// actor can boost entries whose key contains it.
struct MatchContext {
	std::size_t anchor_file = static_cast<std::size_t>(-1);
	std::size_t anchor_position = 0;
	std::string actor;
};

/// One candidate result for a subtitle line.
struct MatchResult {
	double score = 0.0;
	bool exact = false;
	/// True when the match happened against the entry key rather than its text
	bool from_key = false;
	/// Source file of the matched entry
	std::string file;
	/// Index of the source file in the vector passed to Match. Unique even
	/// when two files share the same display name.
	std::size_t file_index = 0;
	/// Key of the matched entry (may be empty)
	std::string key;
	/// Position of the entry within its file, used for anchor proximity.
	std::size_t position = 0;
	/// The text that matched (or the key when from_key is true)
	std::string matched;
	/// The localized text to insert/replace with
	std::string replacement;
	/// Human-readable explanation of where the replacement came from
	std::string origin;
};

/// Match a subtitle line against all loaded localization files.
/// @param extra_keys Optional keys extracted from line fields; they are
///        matched against entry keys in addition to the line text.
std::vector<MatchResult> Match(std::string const& subtitle_text,
	std::vector<LocalizationFile> const& files, MatchOptions const& options,
	std::vector<std::string> const& extra_keys = {},
	MatchContext const& context = MatchContext());

} // namespace localization
