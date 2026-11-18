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

/// @file localization_project.h
/// @brief Shared loading, options and file-list helpers for the localization
/// dialog and the embedded edit-box panel.

#pragma once

#include "localization_types.h"

#include <libaegisub/fs_fwd.h>

#include <map>
#include <string>
#include <vector>

namespace localization {

/// UTF-8 representation of a filesystem path.
std::string PathUtf8(agi::fs::path const& path);

/// Signature of the configured files including size and modification time,
/// used to reload when a localization file changes on disk.
std::string FileSignature(std::vector<agi::fs::path> const& paths);

/// Paths saved in Tool/Localization/Files that still exist.
std::vector<agi::fs::path> ReadPersistedPaths();

/// Read a localization file as UTF-8, detecting its encoding.
std::string ReadFileText(agi::fs::path const& path, std::string& error);

/// File roles persisted in Tool/Localization/Roles, keyed by full path.
std::map<std::string, FileRole> ReadFileRoles();
void SaveFileRoles(std::map<std::string, FileRole> const& roles);

/// Load all localization files, assigning role and a display name that is
/// unique even when two files share a basename.
std::vector<LocalizationFile> LoadFiles(
	std::vector<agi::fs::path> const& paths,
	std::map<std::string, FileRole> const& roles);

/// Matching options persisted under Tool/Localization.
MatchOptions ReadMatchOptions();
void SaveMatchOptions(MatchOptions const& options);

} // namespace localization
