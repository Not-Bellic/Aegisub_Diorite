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

/// @file localization_tm.h
/// @brief Persistent source -> translation memory for accepted matches.

#pragma once

#include "localization_types.h"

#include <libaegisub/fs_fwd.h>

#include <string>
#include <utility>
#include <vector>

namespace localization {

/// A small tab-separated translation memory. Each accepted match is appended
/// and loaded again on the next start, so repeated lines are matched at 100%.
struct TranslationMemory {
	std::vector<std::pair<std::string, std::string>> entries;

	/// Load entries from a UTF-8 TSV file. Missing files are ignored.
	void Load(agi::fs::path const& path);
	/// Add one source -> translation pair, skipping empty or duplicate ones.
	void Append(std::string source, std::string target);
	/// Write all entries back as UTF-8 TSV.
	void Save(agi::fs::path const& path) const;

	/// Build a localization file whose pairs are the memory entries.
	LocalizationFile BuildFile(std::string const& name,
		std::string const& language) const;
};

} // namespace localization
