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

/// @file localization_matcher.cpp
/// @brief Normalization, segmentation and fuzzy matching for localization
/// entries.

#include "localization_matcher.h"

#include "localization_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace localization {

/// A normalized segment used for matching and alignment.
struct Segment {
	std::string raw;
	std::string norm;
	std::string marker;
};

namespace {

std::vector<uint32_t> Utf8ToCodepoints(std::string const& text) {
	std::vector<uint32_t> out;
	size_t i = 0;
	size_t n = text.size();
	while (i < n) {
		unsigned char c = static_cast<unsigned char>(text[i]);
		if (c < 0x80) {
			out.push_back(c);
			++i;
			continue;
		}

		int extra = 0;
		uint32_t cp = 0;
		if ((c & 0xE0) == 0xC0) {
			extra = 1;
			cp = c & 0x1F;
		}
		else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			cp = c & 0x0F;
		}
		else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			cp = c & 0x07;
		}
		else {
			++i;
			continue;
		}

		if (i + extra >= n) break;
		bool valid = true;
		for (int k = 1; k <= extra; ++k) {
			unsigned char cc = static_cast<unsigned char>(text[i + k]);
			if ((cc & 0xC0) != 0x80) {
				valid = false;
				break;
			}
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (!valid) {
			++i;
			continue;
		}
		out.push_back(cp);
		i += extra + 1;
	}
	return out;
}

std::string CodepointsToUtf8(std::vector<uint32_t> const& cps) {
	std::string out;
	for (uint32_t cp : cps) {
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		}
		else if (cp < 0x800) {
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000) {
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else {
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	return out;
}

bool IsAsciiSpace(uint32_t cp) {
	return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
		cp == '\f' || cp == '\v';
}

bool IsUnicodeSpace(uint32_t cp) {
	return cp == 0x00A0 || cp == 0x1680 ||
		(cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
		cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool IsZeroWidth(uint32_t cp) {
	return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 ||
		cp == 0xFEFF;
}

bool IsCombiningMark(uint32_t cp) {
	return (cp >= 0x0300 && cp <= 0x036F) ||
		(cp >= 0x1AB0 && cp <= 0x1AFF) ||
		(cp >= 0x1DC0 && cp <= 0x1DFF) ||
		(cp >= 0x20D0 && cp <= 0x20FF) ||
		(cp >= 0xFE20 && cp <= 0xFE2F);
}

/// CJK scripts do not use spaces as word separators, so spaces around them are
/// dropped during normalization.
bool IsCjk(uint32_t cp) {
	return (cp >= 0x2E80 && cp <= 0x9FFF) ||
		(cp >= 0xF900 && cp <= 0xFAFF) ||
		(cp >= 0xFE30 && cp <= 0xFE4F) ||
		(cp >= 0xFF00 && cp <= 0xFFEF) ||
		(cp >= 0x20000 && cp <= 0x2FA1F) ||
		(cp >= 0x3040 && cp <= 0x30FF) ||
		(cp >= 0xAC00 && cp <= 0xD7AF) ||
		(cp >= 0x1100 && cp <= 0x11FF);
}

bool IsAsciiAlpha(uint32_t cp) {
	return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

bool IsAsciiDigit(uint32_t cp) {
	return cp >= '0' && cp <= '9';
}

bool IsAsciiUpper(uint32_t cp) {
	return cp >= 'A' && cp <= 'Z';
}

/// Map typographic variants onto one canonical form before comparison:
/// full-width ASCII, curly quotes, dashes, ellipsis and Latin ligatures.
void AppendCanonical(std::vector<uint32_t>& out, uint32_t cp) {
	if (cp == 0x00AD) return; // soft hyphen
	if (cp >= 0xFF01 && cp <= 0xFF5E) {
		out.push_back(cp - 0xFEE0);
		return;
	}
	switch (cp) {
		case 0x2018: case 0x2019: case 0x201A: case 0x201B:
			out.push_back('\'');
			return;
		case 0x201C: case 0x201D: case 0x201E: case 0x201F:
			out.push_back('"');
			return;
		case 0x2010: case 0x2011: case 0x2012: case 0x2013:
		case 0x2014: case 0x2015: case 0x2212:
			out.push_back('-');
			return;
		case 0x2026:
			out.push_back('.');
			out.push_back('.');
			out.push_back('.');
			return;
		case 0xFB00:
			out.push_back('f'); out.push_back('f');
			return;
		case 0xFB01:
			out.push_back('f'); out.push_back('i');
			return;
		case 0xFB02:
			out.push_back('f'); out.push_back('l');
			return;
		case 0xFB03:
			out.push_back('f'); out.push_back('f'); out.push_back('i');
			return;
		case 0xFB04:
			out.push_back('f'); out.push_back('f'); out.push_back('l');
			return;
		case 0xFB05: case 0xFB06:
			out.push_back('s'); out.push_back('t');
			return;
		default:
			out.push_back(cp);
	}
}

bool IsPunctuation(uint32_t cp) {
	if (cp < 0x80) {
		return (cp >= 0x21 && cp <= 0x2F) ||
			(cp >= 0x3A && cp <= 0x40) ||
			(cp >= 0x5B && cp <= 0x60) ||
			(cp >= 0x7B && cp <= 0x7E);
	}
	return (cp >= 0x2000 && cp <= 0x206F) ||
		(cp >= 0x2E00 && cp <= 0x2E7F) ||
		(cp >= 0x3001 && cp <= 0x3003) ||
		(cp >= 0x3008 && cp <= 0x301F) ||
		cp == 0x3030 ||
		(cp >= 0xFE10 && cp <= 0xFE1F) ||
		(cp >= 0xFE30 && cp <= 0xFE4F) ||
		(cp >= 0xFF01 && cp <= 0xFF0F) ||
		(cp >= 0xFF1A && cp <= 0xFF1F) ||
		(cp >= 0xFF3B && cp <= 0xFF40) ||
		(cp >= 0xFF5B && cp <= 0xFF65) ||
		cp == 0x00A1 || cp == 0x00BF || cp == 0x2026 || cp == 0x2027 ||
		cp == 0x203C || cp == 0x2047 || cp == 0x00B7 || cp == 0x30FB ||
		cp == 0x301C || cp == 0x00AB || cp == 0x00BB;
}

uint32_t ToLower(uint32_t cp) {
	if (cp >= 'A' && cp <= 'Z') return cp + 32;
	if (cp >= 0xC0 && cp <= 0xD6) return cp + 32;
	if (cp >= 0xD8 && cp <= 0xDE) return cp + 32;
	// U+0130 (İ) is the one Latin Extended-A capital whose lowercase is "i"
	// rather than the next code point, which is dotless ı. Handle it before
	// the range rule so the parity shortcut cannot turn it into ı.
	if (cp == 0x130) return 'i';
	if (cp >= 0x100 && cp <= 0x137 && (cp & 1) == 0) return cp + 1;
	if (cp >= 0x139 && cp <= 0x148 && (cp & 1) == 1) return cp + 1;
	if (cp >= 0x14A && cp <= 0x177 && (cp & 1) == 0) return cp + 1;
	if (cp == 0x178) return 0x00FF;
	if (cp >= 0x179 && cp <= 0x17E && (cp & 1) == 1) return cp + 1;
	if (cp == 0x17F) return 's';
	if (cp >= 0x1DE && cp <= 0x1EF && (cp & 1) == 0) return cp + 1;
	if (cp >= 0x1E00 && cp <= 0x1E95 && (cp & 1) == 0) return cp + 1;
	if (cp >= 0x1EA0 && cp <= 0x1EFF && (cp & 1) == 0) return cp + 1;
	if (cp == 0x1E9E) return 0x00DF;
	if (cp == 0x130 || cp == 0x131) return 'i';
	if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 32;
	if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
	if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
	if (cp >= 0x460 && cp <= 0x481 && (cp & 1) == 0) return cp + 1;
	if (cp >= 0x48A && cp <= 0x4BF && (cp & 1) == 0) return cp + 1;
	if (cp >= 0x4C1 && cp <= 0x4CE && (cp & 1) == 1) return cp + 1;
	if (cp >= 0x4D0 && cp <= 0x52F && (cp & 1) == 0) return cp + 1;
	if (cp >= 0xFF21 && cp <= 0xFF3A) return cp + 0x20;
	return cp;
}

void ReplaceAll(std::string& s, std::string const& from, std::string const& to) {
	if (from.empty()) return;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

/// True for ASS override blocks ({\i1}), game line markers ({*1}) and
/// identifier markers ({TA7}). Literal braces in prose are kept.
bool LooksLikeMarker(std::string const& inner) {
	if (inner.empty()) return false;
	if (inner[0] == '\\' || inner[0] == '*') return true;

	// Bare numeric markers: {1}, {4-5}, {4,5}.
	size_t digits = 0;
	while (digits < inner.size() &&
		std::isdigit(static_cast<unsigned char>(inner[digits])))
		++digits;
	if (digits > 0) {
		if (digits == inner.size()) return true;
		if (inner[digits] == '-' || inner[digits] == ',') {
			size_t end = digits + 1;
			while (end < inner.size() &&
				std::isdigit(static_cast<unsigned char>(inner[end])))
				++end;
			if (end > digits + 1 && end == inner.size()) return true;
		}
	}

	size_t i = 0;
	while (i < inner.size() && std::isalpha(static_cast<unsigned char>(inner[i])))
		++i;
	if (i == 0 || i == inner.size()) return false;
	size_t j = i;
	while (j < inner.size() && std::isdigit(static_cast<unsigned char>(inner[j])))
		++j;
	return j == inner.size();
}

std::string ToLowerAscii(std::string s) {
	for (char& c : s)
		if (c >= 'A' && c <= 'Z') c += 32;
	return s;
}

/// True for known HTML-ish tags such as <b>, </i>, <font color=...>.
bool IsHtmlTag(std::string const& inner) {
	size_t i = 0;
	if (i < inner.size() && inner[i] == '/') ++i;
	size_t start = i;
	while (i < inner.size() &&
		(std::isalpha(static_cast<unsigned char>(inner[i])) || inner[i] == '-'))
		++i;
	if (i == start) return false;
	std::string name = ToLowerAscii(inner.substr(start, i - start));
	static const char* tags[] = {
		"b", "i", "u", "s", "br", "font", "span", "size", "color", "em",
		"strong", "p", "ruby", "rt", "rb", "c", "v", "lang", "hr"
	};
	for (auto const* tag : tags)
		if (name == tag) return true;
	return false;
}

std::string StripBraceTags(std::string const& s) {
	std::string out;
	size_t i = 0;
	size_t n = s.size();
	while (i < n) {
		if (s[i] == '{') {
			size_t end = s.find('}', i + 1);
			if (end != std::string::npos &&
				LooksLikeMarker(s.substr(i + 1, end - i - 1))) {
				i = end + 1;
				continue;
			}
		}
		out += s[i++];
	}
	return out;
}

std::string StripAngleTags(std::string const& s) {
	std::string out;
	size_t i = 0;
	size_t n = s.size();
	while (i < n) {
		if (s[i] == '<') {
			size_t end = s.find('>', i + 1);
			if (end != std::string::npos) {
				std::string inner = s.substr(i + 1, end - i - 1);
				if (IsHtmlTag(inner) || LooksLikeMarker(inner)) {
					i = end + 1;
					continue;
				}
			}
		}
		out += s[i++];
	}
	return out;
}

std::string TrimAscii(std::string s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

/// Bounded Levenshtein distance. Returns true when the distance is within
/// max_dist, and stores the exact distance in dist. Common prefixes/suffixes
/// are trimmed first and the table is limited to the diagonal band that can
/// still satisfy the bound.
bool DistanceWithin(std::vector<uint32_t> const& a, std::vector<uint32_t> const& b,
	size_t max_dist, size_t& dist) {
	size_t n = a.size();
	size_t m = b.size();
	if (n > m + max_dist || m > n + max_dist) return false;

	size_t start = 0;
	while (start < n && start < m && a[start] == b[start]) ++start;
	size_t end_a = n;
	size_t end_b = m;
	while (end_a > start && end_b > start && a[end_a - 1] == b[end_b - 1]) {
		--end_a;
		--end_b;
	}
	n = end_a - start;
	m = end_b - start;
	if (n == 0) {
		dist = m;
		return m <= max_dist;
	}
	if (m == 0) {
		dist = n;
		return n <= max_dist;
	}

	const size_t inf = std::numeric_limits<size_t>::max() / 4;
	std::vector<size_t> prev(m + 1, inf);
	std::vector<size_t> cur(m + 1, inf);
	size_t initial = std::min(m, max_dist);
	for (size_t j = 0; j <= initial; ++j) prev[j] = j;

	for (size_t i = 1; i <= n; ++i) {
		std::fill(cur.begin(), cur.end(), inf);
		size_t lo = i > max_dist ? i - max_dist : 1;
		size_t hi = std::min(m, i + max_dist);
		cur[0] = i <= max_dist ? i : inf;
		size_t row_min = cur[0];
		for (size_t j = lo; j <= hi; ++j) {
			size_t cost = a[start + i - 1] == b[start + j - 1] ? 0 : 1;
			size_t value = std::min({
				prev[j] + 1,
				cur[j - 1] + 1,
				prev[j - 1] + cost
			});
			cur[j] = value;
			row_min = std::min(row_min, value);
		}
		if (row_min > max_dist) return false;
		prev.swap(cur);
	}
	dist = prev[m];
	return dist <= max_dist;
}

size_t EditDistance(std::vector<uint32_t> const& a, std::vector<uint32_t> const& b) {
	size_t dist = 0;
	DistanceWithin(a, b, std::max(a.size(), b.size()), dist);
	return dist;
}

/// Query-side data reused for every candidate comparison.
struct QueryProfile {
	std::string norm;
	std::vector<uint32_t> cps;
	std::unordered_map<uint32_t, int> hist;
	std::vector<std::string> tokens;
};

std::vector<std::string> SplitTokens(std::string const& norm) {
	std::vector<std::string> tokens;
	size_t start = 0;
	while (start < norm.size()) {
		size_t end = norm.find(' ', start);
		if (end == std::string::npos) end = norm.size();
		if (end > start)
			tokens.push_back(norm.substr(start, end - start));
		start = end + 1;
	}
	return tokens;
}

QueryProfile MakeProfile(std::string norm) {
	QueryProfile profile;
	profile.cps = Utf8ToCodepoints(norm);
	for (uint32_t cp : profile.cps) ++profile.hist[cp];
	profile.tokens = SplitTokens(norm);
	profile.norm = std::move(norm);
	return profile;
}

/// Multiset overlap, used as a cheap lower bound on the edit distance.
size_t CommonCount(std::unordered_map<uint32_t, int> hist,
	std::vector<uint32_t> const& cps) {
	size_t common = 0;
	for (uint32_t cp : cps) {
		auto it = hist.find(cp);
		if (it != hist.end() && it->second > 0) {
			--it->second;
			++common;
		}
	}
	return common;
}

double TokenDice(std::vector<std::string> const& a,
	std::vector<std::string> const& b) {
	if (a.size() < 2 || b.size() < 2) return 0.0;
	std::unordered_map<std::string, int> counts;
	for (auto const& token : a) ++counts[token];
	size_t common = 0;
	for (auto const& token : b) {
		auto it = counts.find(token);
		if (it != counts.end() && it->second > 0) {
			--it->second;
			++common;
		}
	}
	return 2.0 * static_cast<double>(common) /
		static_cast<double>(a.size() + b.size());
}

double BigramDice(std::vector<uint32_t> const& a,
	std::vector<uint32_t> const& b) {
	if (a.empty() || b.empty()) return 0.0;

	auto build = [](std::vector<uint32_t> const& v) {
		std::unordered_map<uint64_t, int> grams;
		if (v.size() == 1) {
			grams[static_cast<uint64_t>(v[0]) | (uint64_t(1) << 40)] = 1;
		}
		for (size_t i = 0; i + 1 < v.size(); ++i) {
			uint64_t key = (static_cast<uint64_t>(v[i]) << 32) | v[i + 1];
			++grams[key];
		}
		return grams;
	};

	auto grams_a = build(a);
	auto grams_b = build(b);
	size_t total_a = 0;
	size_t total_b = 0;
	for (auto const& kv : grams_a) total_a += static_cast<size_t>(kv.second);
	for (auto const& kv : grams_b) total_b += static_cast<size_t>(kv.second);
	if (total_a == 0 || total_b == 0) return 0.0;

	size_t common = 0;
	for (auto const& kv : grams_a) {
		auto it = grams_b.find(kv.first);
		if (it != grams_b.end())
			common += static_cast<size_t>(std::min(kv.second, it->second));
	}
	return 2.0 * static_cast<double>(common) /
		static_cast<double>(total_a + total_b);
}

/// Number of Unicode code points in a UTF-8 string. Byte lengths are not a
/// meaningful proxy for character counts (one CJK character is three bytes).
size_t CodepointLength(std::string const& text) {
	size_t count = 0;
	for (unsigned char c : text)
		if ((c & 0xC0) != 0x80) ++count;
	return count;
}

/// A text segment produced by the splitter, before normalization.
struct RawSegment {
	std::string text;
	/// Marker that introduced this segment, e.g. "{*2}" or "{TA7}". Markers
	/// are preserved so source and translation segments can be aligned even
	/// when their texts share no characters.
	std::string marker;
};

constexpr size_t kMaxAlignedSegments = 32;
constexpr double kSegmentSkipCost = 0.9;

/// Cost of pairing one source segment with one translation segment. Markers
/// dominate, position keeps the mapping monotonic when markers are absent, and
/// text similarity only refines same-language alignments (source and
/// translation segments usually share no characters).
double SegmentMatchCost(Segment const& a, size_t i, size_t n,
	Segment const& b, size_t j, size_t m) {
	double position = 0.0;
	if (n > 1 && m > 1)
		position = std::abs(static_cast<double>(i) / static_cast<double>(n - 1) -
			static_cast<double>(j) / static_cast<double>(m - 1));
	else if (n > 1)
		position = static_cast<double>(i) / static_cast<double>(n - 1);
	else if (m > 1)
		position = static_cast<double>(j) / static_cast<double>(m - 1);

	double cost = position * 0.8;

	if (!a.marker.empty() && !b.marker.empty()) {
		if (a.marker == b.marker)
			cost -= 0.7;
		else
			cost += 0.6;
	}

	cost += 0.25 * (1.0 - Similarity(a.norm, b.norm));
	return std::max(0.0, cost);
}

/// Monotonic alignment of two segmented texts. Returns, for every segment of
/// @p from, the index of the aligned segment in @p to, or -1 when the segment
/// has no counterpart. Unlike the previous index-based mapping this tolerates
/// merged, split or missing sentences.
std::vector<int> MapSegments(std::vector<Segment> const& from,
	std::vector<Segment> const& to) {
	std::vector<int> map(from.size(), -1);
	size_t n = from.size();
	size_t m = to.size();
	if (n == 0 || m == 0) return map;

	if (n > kMaxAlignedSegments || m > kMaxAlignedSegments) {
		// Pathological segment counts: fall back to proportional positions.
		for (size_t i = 0; i < n; ++i)
			map[i] = static_cast<int>(std::min(m - 1, i * m / n));
		return map;
	}

	size_t stride = m + 1;
	std::vector<double> cost((n + 1) * stride, 0.0);
	std::vector<unsigned char> decision((n + 1) * stride, 0);
	for (size_t i = 1; i <= n; ++i) {
		cost[i * stride] = i * kSegmentSkipCost;
		decision[i * stride] = 1; // skip a segment of `from`
	}
	for (size_t j = 1; j <= m; ++j) {
		cost[j] = j * kSegmentSkipCost;
		decision[j] = 2; // skip a segment of `to`
	}

	for (size_t i = 1; i <= n; ++i) {
		for (size_t j = 1; j <= m; ++j) {
			double best = cost[(i - 1) * stride + j] + kSegmentSkipCost;
			unsigned char best_move = 1;
			double skip_to = cost[i * stride + (j - 1)] + kSegmentSkipCost;
			if (skip_to < best) {
				best = skip_to;
				best_move = 2;
			}
			double match = cost[(i - 1) * stride + (j - 1)] +
				SegmentMatchCost(from[i - 1], i - 1, n, to[j - 1], j - 1, m);
			if (match <= best) {
				best = match;
				best_move = 0;
			}
			cost[i * stride + j] = best;
			decision[i * stride + j] = best_move;
		}
	}

	size_t i = n;
	size_t j = m;
	while (i > 0 && j > 0) {
		unsigned char step = decision[i * stride + j];
		if (step == 0) {
			map[i - 1] = static_cast<int>(j - 1);
			--i;
			--j;
		}
		else if (step == 1) {
			--i;
		}
		else {
			--j;
		}
	}
	return map;
}

} // namespace

/// Cached normalization and segmentation for one localization file. The cache
/// is keyed by the options that affect Normalize() and SplitSegments().
struct PreparedFile {
	bool ignore_tags = false;
	bool ignore_punctuation = false;
	bool ignore_case = false;
	bool split_sentences = false;
	std::string split_regex;
	size_t item_count = 0;
	size_t pair_count = 0;

	struct Item {
		std::string norm_text;
		std::string norm_key;
		std::vector<Segment> segments;
	};
	struct Pair {
		std::string norm_source;
		std::vector<Segment> source_segments;
		std::vector<Segment> translation_segments;
	};

	std::vector<Item> items;
	std::vector<Pair> pairs;
};

/// Compile a regular expression once and cache it. Invalid patterns are
/// cached as null so they are not recompiled on every keystroke.
std::shared_ptr<std::regex const> CompileRegex(std::string const& pattern) {
	if (pattern.empty()) return {};

	static std::mutex mutex;
	static std::unordered_map<std::string,
		std::shared_ptr<std::regex const>> cache;
	std::lock_guard<std::mutex> lock(mutex);

	auto it = cache.find(pattern);
	if (it != cache.end()) return it->second;

	std::shared_ptr<std::regex const> compiled;
	try {
		compiled = std::make_shared<std::regex>(pattern);
	}
	catch (std::regex_error const&) {
		// Keep a null entry so validation and matching short-circuit.
	}
	if (cache.size() >= 32) cache.clear();
	cache.emplace(pattern, compiled);
	return compiled;
}

bool IsValidRegex(std::string const& pattern) {
	return pattern.empty() || CompileRegex(pattern) != nullptr;
}

std::string Normalize(std::string text, MatchOptions const& options) {
	// ASS escapes: line breaks and hard spaces become ordinary spaces.
	ReplaceAll(text, "\\N", " ");
	ReplaceAll(text, "\\n", " ");
	ReplaceAll(text, "\\h", " ");

	if (options.ignore_tags) {
		text = StripBraceTags(text);
		text = StripAngleTags(text);
	}

	std::vector<uint32_t> cps = Utf8ToCodepoints(text);
	std::vector<uint32_t> canon;
	canon.reserve(cps.size());
	for (uint32_t cp : cps)
		AppendCanonical(canon, cp);

	std::vector<uint32_t> out;
	bool last_space = false;
	for (uint32_t cp : canon) {
		if (IsZeroWidth(cp) || IsCombiningMark(cp)) continue;
		if (options.ignore_punctuation && IsPunctuation(cp)) continue;
		if (options.ignore_case) cp = ToLower(cp);

		bool ws = IsAsciiSpace(cp) || IsUnicodeSpace(cp);
		if (ws) {
			if (!last_space && !out.empty()) {
				out.push_back(' ');
				last_space = true;
			}
		}
		else {
			out.push_back(cp);
			last_space = false;
		}
	}

	// Spaces around CJK characters are not word separators.
	std::vector<uint32_t> filtered;
	filtered.reserve(out.size());
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i] == ' ') {
			bool prev_cjk = !filtered.empty() && IsCjk(filtered.back());
			bool next_cjk = i + 1 < out.size() && IsCjk(out[i + 1]);
			if (prev_cjk || next_cjk) continue;
		}
		filtered.push_back(out[i]);
	}
	while (!filtered.empty() && filtered.back() == ' ') filtered.pop_back();
	return CodepointsToUtf8(filtered);
}

double Similarity(std::string const& a, std::string const& b) {
	auto ca = Utf8ToCodepoints(a);
	auto cb = Utf8ToCodepoints(b);
	if (ca.empty() && cb.empty()) return 1.0;
	if (ca.empty() || cb.empty()) return 0.0;
	size_t maxlen = std::max(ca.size(), cb.size());
	size_t dist = EditDistance(ca, cb);
	return 1.0 - static_cast<double>(dist) / static_cast<double>(maxlen);
}

/// The word immediately before a trailing dot, without the dot itself.
static std::string TokenBeforeDot(std::vector<uint32_t> const& cps) {
	if (cps.size() < 2 || cps.back() != '.') return {};
	size_t end = cps.size() - 1;
	size_t start = end;
	while (start > 0) {
		uint32_t c = cps[start - 1];
		if (IsAsciiAlpha(c) || c == '.' || c == '\'' || c == '-')
			--start;
		else
			break;
	}
	std::vector<uint32_t> token(cps.begin() + static_cast<std::ptrdiff_t>(start),
		cps.begin() + static_cast<std::ptrdiff_t>(end));
	return CodepointsToUtf8(token);
}

/// Treat "Mr.", "Dr.", "U.S.", "J." and similar forms as non-breaking.
static bool IsAbbreviation(std::vector<uint32_t> const& cps) {
	std::string token = TokenBeforeDot(cps);
	if (token.empty()) return false;

	std::string lower = ToLowerAscii(token);
	static const char* abbreviations[] = {
		"mr", "mrs", "ms", "miss", "dr", "prof", "st", "jr", "sr", "vs",
		"etc", "e.g", "i.e", "fig", "no", "dept", "est", "approx", "inc",
		"ltd", "co", "corp", "univ", "ave", "blvd", "rd", "mt", "ft",
		"gen", "col", "sgt", "capt", "lt", "rev", "hon", "pres", "gov",
		"sen", "rep", "jan", "feb", "mar", "apr", "jun", "jul", "aug",
		"sep", "sept", "oct", "nov", "dec"
	};
	for (auto const* abbreviation : abbreviations)
		if (lower == abbreviation) return true;

	// Initials and acronyms: "J." or "U.S".
	bool any_letter = false;
	bool all_upper = true;
	bool has_dot = false;
	size_t letters = 0;
	for (unsigned char c : token) {
		if (std::isalpha(c)) {
			any_letter = true;
			++letters;
			if (!IsAsciiUpper(c)) all_upper = false;
		}
		else if (c == '.') {
			has_dot = true;
		}
	}
	return any_letter && all_upper && (letters == 1 || has_dot);
}

/// Split a single text at sentence endings (。.!?…) without splitting inside
/// "..." or "……" and without breaking abbreviations or decimal numbers.
/// Whitespace after the ending is consumed.
static std::vector<std::string> SplitBySentences(std::string const& text) {
	std::vector<std::string> out;
	std::vector<uint32_t> cur;
	auto flush = [&] {
		std::string s = TrimAscii(CodepointsToUtf8(cur));
		if (!s.empty()) out.push_back(std::move(s));
		cur.clear();
	};

	auto cps = Utf8ToCodepoints(text);
	bool has_content = false;
	for (size_t k = 0; k < cps.size(); ++k) {
		uint32_t cp = cps[k];
		cur.push_back(cp);

		bool dot = cp == '.';
		bool bang = cp == '!' || cp == '?';
		bool ellipsis = cp == 0x2026;
		bool cjk_ender = cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F;
		if (!(dot || bang || ellipsis || cjk_ender)) has_content = true;

		if (dot || ellipsis) {
			// Wait until the end of the "..." / "……" run.
			bool next_run = k + 1 < cps.size() &&
				(cps[k + 1] == '.' || cps[k + 1] == 0x2026);
			if (next_run) continue;
			if (dot) {
				bool decimal = k > 0 && k + 1 < cps.size() &&
					IsAsciiDigit(cps[k - 1]) && IsAsciiDigit(cps[k + 1]);
				if (decimal || IsAbbreviation(cur)) continue;
			}
		}

		bool next_ws = k + 1 >= cps.size() ||
			IsAsciiSpace(cps[k + 1]) || IsUnicodeSpace(cps[k + 1]);
		bool next_cjk = !next_ws && k + 1 < cps.size() && IsCjk(cps[k + 1]);

		bool split = false;
		if (has_content) {
			if (cjk_ender)
				split = true;
			else if (dot || bang)
				split = next_ws;
			else if (ellipsis)
				split = next_ws || next_cjk;
		}

		// Only split when the accumulated segment has actual content, so a
		// leading ellipsis such as "… this is..." is not split off.
		if (split) {
			flush();
			has_content = false;
			while (k + 1 < cps.size() &&
				(IsAsciiSpace(cps[k + 1]) || IsUnicodeSpace(cps[k + 1])))
				++k;
		}
	}
	flush();
	return out;
}

/// Split a text at every match of the regex, removing the matched text. The
/// matched text becomes the alignment marker of the following segment.
static std::vector<RawSegment> SplitByRegex(RawSegment const& segment,
	std::regex const& re) {
	std::vector<RawSegment> out;
	std::string current;
	// A structural marker ({*N}) labels the whole segment; regex markers only
	// label the piece they introduce when there is no structural marker.
	std::string structural = segment.marker;
	std::string marker = structural;
	auto flush = [&] {
		std::string trimmed = TrimAscii(current);
		if (!trimmed.empty())
			out.push_back({std::move(trimmed), marker});
		current.clear();
		marker.clear();
	};

	size_t pos = 0;
	for (std::sregex_iterator it(segment.text.begin(), segment.text.end(), re), end;
		it != end; ++it) {
		size_t mpos = static_cast<size_t>(it->position());
		size_t mlen = static_cast<size_t>(it->length());
		if (mlen == 0) continue; // ignore zero-length matches
		current += segment.text.substr(pos, mpos - pos);
		flush();
		marker = structural.empty() ? it->str() : structural;
		pos = mpos + mlen;
	}
	current += segment.text.substr(pos);
	flush();
	return out;
}

/// Internal splitter. Pass nullptr to disable regex splitting.
static std::vector<RawSegment> SplitSegmentsImpl(std::string const& text,
	bool split_sentences, std::regex const* split_regex) {
	std::vector<RawSegment> segments;
	std::string current;
	std::string pending_marker;
	auto flush = [&] {
		std::string trimmed = TrimAscii(current);
		if (!trimmed.empty())
			segments.push_back({std::move(trimmed), pending_marker});
		current.clear();
		pending_marker.clear();
	};

	// Pass 1: line breaks/escapes and game-style {*N}/{*4-5} markers
	size_t i = 0;
	size_t n = text.size();
	while (i < n) {
		if (text[i] == '{') {
			size_t j = i + 1;
			if (j < n && text[j] == '*') ++j;
			size_t d = j;
			while (d < n && std::isdigit(static_cast<unsigned char>(text[d]))) ++d;
			// Range markers such as {*4-5} or {*4,5} are also boundaries.
			if (d > j && d < n && (text[d] == '-' || text[d] == ',')) {
				size_t e = d + 1;
				while (e < n && std::isdigit(static_cast<unsigned char>(text[e]))) ++e;
				if (e > d + 1 && e < n && text[e] == '}') {
					flush();
					pending_marker = text.substr(i, e + 1 - i);
					i = e + 1;
					continue;
				}
			}
			if (d > j && d < n && text[d] == '}') {
				flush();
				pending_marker = text.substr(i, d + 1 - i);
				i = d + 1;
				continue;
			}
		}
		if (text[i] == '\\' && i + 1 < n &&
			(text[i + 1] == 'N' || text[i + 1] == 'n')) {
			flush();
			i += 2;
			continue;
		}
		if (text[i] == '\n' || text[i] == '\r') {
			flush();
			++i;
			continue;
		}
		current += text[i];
		++i;
	}
	flush();

	// Pass 2: user-defined regex boundaries
	if (split_regex) {
		std::vector<RawSegment> split;
		for (auto const& seg : segments) {
			auto sub = SplitByRegex(seg, *split_regex);
			for (auto& part : sub)
				split.push_back(std::move(part));
		}
		segments = std::move(split);
	}

	// Pass 3: sentence boundaries. Only the first sentence inherits the
	// marker; the rest are matched by position.
	if (split_sentences) {
		std::vector<RawSegment> split;
		for (auto const& seg : segments) {
			auto pieces = SplitBySentences(seg.text);
			for (size_t k = 0; k < pieces.size(); ++k)
				split.push_back({std::move(pieces[k]),
					k == 0 ? seg.marker : std::string()});
		}
		segments = std::move(split);
	}

	if (segments.empty()) {
		std::string trimmed = TrimAscii(text);
		if (!trimmed.empty()) segments.push_back({std::move(trimmed), {}});
	}
	return segments;
}

std::vector<std::string> SplitSegments(std::string const& text,
	bool split_sentences, std::string const& split_regex) {
	auto re = CompileRegex(split_regex);
	std::vector<std::string> out;
	for (auto& seg : SplitSegmentsImpl(text, split_sentences, re.get()))
		out.push_back(std::move(seg.text));
	return out;
}

std::vector<Segment> MakeSegments(std::string const& text,
	MatchOptions const& options, std::regex const* split_regex) {
	std::vector<Segment> segments;
	for (auto& raw : SplitSegmentsImpl(text, options.split_sentences,
		split_regex)) {
		std::string norm = Normalize(raw.text, options);
		if (!norm.empty())
			segments.push_back({std::move(raw.text), std::move(norm),
				std::move(raw.marker)});
	}
	return segments;
}

bool PreparedFileMatches(PreparedFile const& prepared,
	LocalizationFile const& file, MatchOptions const& options) {
	return prepared.ignore_tags == options.ignore_tags &&
		prepared.ignore_punctuation == options.ignore_punctuation &&
		prepared.ignore_case == options.ignore_case &&
		prepared.split_sentences == options.split_sentences &&
		prepared.split_regex == options.split_regex &&
		prepared.item_count == file.items.size() &&
		prepared.pair_count == file.pairs.size();
}

PreparedFile const& EnsurePrepared(LocalizationFile const& file,
	MatchOptions const& options, std::regex const* split_regex) {
	if (file.prepared && PreparedFileMatches(*file.prepared, file, options))
		return *file.prepared;

	auto prepared = std::make_shared<PreparedFile>();
	prepared->ignore_tags = options.ignore_tags;
	prepared->ignore_punctuation = options.ignore_punctuation;
	prepared->ignore_case = options.ignore_case;
	prepared->split_sentences = options.split_sentences;
	prepared->split_regex = options.split_regex;
	prepared->item_count = file.items.size();
	prepared->pair_count = file.pairs.size();
	prepared->items.reserve(file.items.size());
	for (auto const& item : file.items) {
		prepared->items.push_back({
			Normalize(item.text, options),
			Normalize(item.key, options),
			MakeSegments(item.text, options, split_regex)
		});
	}
	prepared->pairs.reserve(file.pairs.size());
	for (auto const& pair : file.pairs) {
		prepared->pairs.push_back({
			Normalize(pair.source, options),
			MakeSegments(pair.source, options, split_regex),
			MakeSegments(pair.translation, options, split_regex)
		});
	}
	file.prepared = std::move(prepared);
	return *file.prepared;
}

void Prepare(std::vector<LocalizationFile>& files,
	MatchOptions const& options) {
	auto split_regex = CompileRegex(options.split_regex);
	for (auto& file : files)
		if (file.ok)
			EnsurePrepared(file, options, split_regex.get());
}

std::vector<std::string> ExtractKeys(std::string const& text,
	std::string const& regex) {
	std::vector<std::string> keys;
	if (regex.empty()) return keys;

	auto re = CompileRegex(regex);
	if (!re) return keys;

	for (std::sregex_iterator it(text.begin(), text.end(), *re), end;
		it != end; ++it) {
		std::string key = it->size() > 1 && (*it)[1].matched
			? (*it)[1].str() : it->str();
		key = TrimAscii(std::move(key));
		if (!key.empty()) keys.push_back(std::move(key));
	}
	return keys;
}

std::vector<MatchResult> Match(std::string const& subtitle_text,
	std::vector<LocalizationFile> const& files, MatchOptions const& options,
	std::vector<std::string> const& extra_keys, MatchContext const& context) {
	std::vector<MatchResult> results;
	std::string query = Normalize(subtitle_text, options);
	if (query.empty()) return results;

	auto split_regex = CompileRegex(options.split_regex);

	struct NormalizedItem {
		TextItem const* item;
		size_t file_index;
		size_t position;
		PreparedFile::Item const* prepared;
		/// Synthetic positional key used when unkeyed files are paired.
		std::string key_override;

		std::string const& Key() const {
			return key_override.empty() ? prepared->norm_key : key_override;
		}
	};
	struct NormalizedPair {
		BilingualRecord const* record;
		size_t file_index;
		size_t position;
		PreparedFile::Pair const* prepared;
	};

	std::vector<NormalizedItem> items;
	std::vector<NormalizedPair> pairs;
	std::unordered_multimap<std::string, size_t> exact_index;
	// Normalized key -> indices of the items carrying that key, across all
	// files and both sides of bilingual entries. Using the normalized key is
	// what lets "Greeting" in one file link to "greeting" in another.
	std::unordered_map<std::string, std::vector<size_t>> key_map;

	std::string query_language = DetectLanguage(query);

	// Unkeyed monolingual files (two plain text lists, one per language) can
	// only be matched by position. Only pair an unambiguous couple: explicit
	// Source/Target roles, or exactly two unkeyed files with different
	// detected languages and the same number of entries.
	std::vector<std::string> position_prefix(files.size());
	if (options.pair_unkeyed_files) {
		auto is_unkeyed = [](LocalizationFile const& file) {
			if (!file.ok || file.synthetic || file.items.empty() ||
				!file.pairs.empty())
				return false;
			for (auto const& item : file.items)
				if (!item.key.empty()) return false;
			return true;
		};

		auto assign_pair = [&](size_t source, size_t target) {
			std::string prefix = "\x01pair:" + std::to_string(source) + "-" +
				std::to_string(target) + ":";
			position_prefix[source] = prefix;
			position_prefix[target] = prefix;
		};

		size_t source = std::numeric_limits<size_t>::max();
		size_t target = std::numeric_limits<size_t>::max();
		for (size_t i = 0; i < files.size(); ++i) {
			if (!is_unkeyed(files[i])) continue;
			if (files[i].role == FileRole::Source &&
				source == std::numeric_limits<size_t>::max())
				source = i;
			else if (files[i].role == FileRole::Target &&
				target == std::numeric_limits<size_t>::max())
				target = i;
		}

		if (source != std::numeric_limits<size_t>::max() &&
			target != std::numeric_limits<size_t>::max()) {
			if (files[source].items.size() == files[target].items.size())
				assign_pair(source, target);
		}
		else {
			std::vector<size_t> unkeyed;
			for (size_t i = 0; i < files.size(); ++i)
				if (is_unkeyed(files[i])) unkeyed.push_back(i);
			if (unkeyed.size() == 2 &&
				files[unkeyed[0]].items.size() == files[unkeyed[1]].items.size() &&
				!files[unkeyed[0]].language.empty() &&
				!files[unkeyed[1]].language.empty() &&
				files[unkeyed[0]].language != files[unkeyed[1]].language) {
				assign_pair(unkeyed[0], unkeyed[1]);
			}
		}
	}

	for (size_t file_index = 0; file_index < files.size(); ++file_index) {
		auto const& f = files[file_index];
		if (!f.ok) continue;
		PreparedFile const& prepared = EnsurePrepared(f, options,
			split_regex.get());
		size_t position = 0;
		for (size_t i = 0; i < f.items.size(); ++i) {
			auto const& item = f.items[i];
			auto const& prepared_item = prepared.items[i];
			size_t idx = items.size();
			NormalizedItem normalized;
			normalized.item = &item;
			normalized.file_index = file_index;
			normalized.position = position;
			normalized.prepared = &prepared_item;
			if (prepared_item.norm_key.empty() &&
				!position_prefix[file_index].empty())
				normalized.key_override = position_prefix[file_index] +
					std::to_string(position);
			if (!prepared_item.norm_text.empty())
				exact_index.emplace(prepared_item.norm_text, idx);
			if (!normalized.Key().empty())
				key_map[normalized.Key()].push_back(idx);
			items.push_back(std::move(normalized));
			++position;
		}
		size_t pair_position = 0;
		for (size_t i = 0; i < f.pairs.size(); ++i) {
			pairs.push_back({
				&f.pairs[i], file_index, pair_position, &prepared.pairs[i]
			});
			++pair_position;
		}
	}

	QueryProfile line_profile = MakeProfile(query);

	struct ExtraKey {
		std::string raw;
		QueryProfile profile;
	};
	std::vector<ExtraKey> extra_profiles;
	for (auto const& raw : extra_keys) {
		std::string norm = Normalize(raw, options);
		if (norm.empty()) continue;
		extra_profiles.push_back({raw, MakeProfile(std::move(norm))});
	}

	// Score a normalized string against a query profile, or -1 when below the
	// threshold. Exact equality returns 1.0 so callers can flag exact matches.
	auto score_against = [&](std::string const& norm,
		QueryProfile const& q) -> double {
		if (norm.empty()) return -1.0;
		if (norm == q.norm) return 1.0;
		if (!options.fuzzy || options.threshold >= 1.0) return -1.0;

		auto nc = Utf8ToCodepoints(norm);
		size_t nlen = nc.size();
		size_t qlen = q.cps.size();
		size_t maxlen = std::max(qlen, nlen);
		size_t minlen = std::min(qlen, nlen);
		size_t max_dist = static_cast<size_t>(
			std::floor((1.0 - options.threshold) * static_cast<double>(maxlen)));

		bool contained = q.norm.find(norm) != std::string::npos ||
			norm.find(q.norm) != std::string::npos;

		// Guard against meaningless matches of very short strings.
		if (minlen <= 2 && qlen != nlen && !contained) return -1.0;

		double score = -1.0;

		// A query contained in a longer text is a strong signal that the
		// global edit distance alone misses.
		if (contained && minlen >= 3) {
			double coverage = static_cast<double>(minlen) /
				static_cast<double>(maxlen);
			if (coverage >= 0.45)
				score = std::max(score, 0.85 + 0.10 * coverage);
		}

		double length_ratio = static_cast<double>(minlen) /
			static_cast<double>(maxlen);
		if (length_ratio >= 0.35) {
			if (q.tokens.size() >= 2) {
				auto tokens = SplitTokens(norm);
				if (tokens.size() >= 2)
					score = std::max(score,
						0.97 * TokenDice(q.tokens, tokens));
			}
			if (minlen >= 3)
				score = std::max(score, 0.95 * BigramDice(q.cps, nc));
		}

		// Run the banded edit distance only when the multiset overlap proves
		// the threshold is still reachable.
		size_t common = CommonCount(q.hist, nc);
		if (maxlen - common <= max_dist) {
			size_t dist = 0;
			if (DistanceWithin(q.cps, nc, max_dist, dist)) {
				double ratio = 1.0 - static_cast<double>(dist) /
					static_cast<double>(maxlen);
				score = std::max(score, ratio);
			}
		}

		if (score + 1e-9 < options.threshold) return -1.0;
		return std::min(score, 0.999999);
	};

	auto score_norm = [&](std::string const& norm) {
		return score_against(norm, line_profile);
	};

	std::set<std::string> seen;
	auto emit = [&](MatchResult const& r) {
		if (r.replacement.empty()) return;
		std::string dedup_key = std::to_string(r.file_index) + "\x1F" +
			r.key + "\x1F" + r.replacement;
		if (!seen.insert(dedup_key).second) return;
		results.push_back(r);
	};

	auto emit_candidates = [&](NormalizedItem const& ni,
		std::string const& matched, int seg_index,
		std::string const& direct_replacement, std::string const& direct_norm,
		std::string const& direct_origin, bool from_key, double score, bool exact) {
		TextItem const& item = *ni.item;
		MatchResult base;
		base.score = score;
		base.exact = exact;
		base.from_key = from_key;
		base.file = item.file;
		base.file_index = ni.file_index;
		base.key = item.key;
		base.position = ni.position;
		base.matched = matched;

		// A source-side entry is only a way to find the key; it can never be
		// the localized text. A translation identical to the line after
		// normalization is noise; plain entries still offer raw variants that
		// differ only in tags or punctuation (e.g. a cleaner version).
		bool same_as_line = item.role == TextRole::Translation
			? direct_norm == query
			: direct_replacement == subtitle_text;
		bool source_file = files[ni.file_index].role == FileRole::Source;
		if (!source_file && item.role != TextRole::Source && !same_as_line) {
			MatchResult self = base;
			self.replacement = direct_replacement;
			self.origin = direct_origin;
			emit(self);
		}

		if (ni.Key().empty()) return;
		auto it = key_map.find(ni.Key());
		if (it == key_map.end()) return;
		for (size_t idx : it->second) {
			NormalizedItem const& entry = items[idx];
			if (entry.file_index == ni.file_index) continue;
			if (entry.item->role == TextRole::Source) continue;
			if (files[entry.file_index].role == FileRole::Source) continue;
			// For auto-role files, do not offer the source-language side of a
			// pair as a translation.
			if (!query_language.empty() &&
				files[entry.file_index].language == query_language &&
				files[ni.file_index].language != query_language)
				continue;
			// When the line is already in the preferred (target) language,
			// do not offer the source-language side as a translation. Source
			// items of bilingual files still need this cross-file path.
			if (!options.preferred_language.empty() &&
				ni.item->role != TextRole::Source &&
				files[ni.file_index].language == options.preferred_language &&
				files[entry.file_index].language != options.preferred_language)
				continue;

			std::string replacement;
			std::string origin;
			if (seg_index >= 0) {
				if (entry.prepared->segments.empty()) continue;
				std::vector<int> map = MapSegments(ni.prepared->segments,
					entry.prepared->segments);
				if (static_cast<size_t>(seg_index) >= map.size()) continue;
				int target = map[static_cast<size_t>(seg_index)];
				if (target < 0) {
					// No aligned segment. A single-segment entry still holds
					// the whole translation, but never insert an unrelated
					// sentence from a multi-segment entry.
					if (entry.prepared->segments.size() == 1)
						target = 0;
					else
						continue;
				}
				replacement =
					entry.prepared->segments[static_cast<size_t>(target)].raw;
				origin = "segment " + std::to_string(target + 1) + " of " +
					entry.item->file + " (key \"" + entry.item->key + "\")";
			}
			else {
				replacement = entry.item->text;
				origin = "key \"" + entry.item->key + "\" in " + entry.item->file;
			}
			if (replacement.empty() || replacement == direct_replacement) continue;

			MatchResult cross = base;
			cross.file = entry.item->file;
			cross.file_index = entry.file_index;
			cross.key = entry.item->key;
			cross.position = entry.position;
			cross.replacement = replacement;
			cross.origin = origin;
			emit(cross);
		}
	};

	auto add_exact = [&](size_t index) {
		NormalizedItem const& ni = items[index];
		emit_candidates(ni, ni.item->text, -1, ni.item->text,
			ni.prepared->norm_text, ni.item->file, false, 1.0, true);
	};

	auto add_fuzzy = [&](size_t index, double score) {
		NormalizedItem const& ni = items[index];
		emit_candidates(ni, ni.item->text, -1, ni.item->text,
			ni.prepared->norm_text, ni.item->file, false, score, false);
	};

	auto add_key_match = [&](size_t index, std::string const& matched,
		double score, bool exact) {
		NormalizedItem const& ni = items[index];
		emit_candidates(ni, matched, -1, ni.item->text,
			ni.prepared->norm_text,
			"key \"" + matched + "\" -> value in " + ni.item->file,
			true, score, exact);
	};

	// Exact text matches
	auto exact_range = exact_index.equal_range(query);
	for (auto it = exact_range.first; it != exact_range.second; ++it)
		add_exact(it->second);

	// Exact key matches, including keys extracted from the line fields.
	for (size_t i = 0; i < items.size(); ++i) {
		std::string const& norm_key = items[i].Key();
		if (CodepointLength(norm_key) < 2) continue;
		if (norm_key == query)
			add_key_match(i, items[i].item->key, 1.0, true);
		for (auto const& extra : extra_profiles)
			if (norm_key == extra.profile.norm)
				add_key_match(i, extra.raw, 1.0, true);
	}

	// Fuzzy whole-string matches
	for (size_t i = 0; i < items.size(); ++i) {
		std::string const& norm_text = items[i].prepared->norm_text;
		if (norm_text.empty() || norm_text == query) continue;
		double score = score_norm(norm_text);
		if (score >= 0)
			add_fuzzy(i, score);
	}

	// Fuzzy key matches (longer keys only), including extracted keys.
	for (size_t i = 0; i < items.size(); ++i) {
		std::string const& norm_key = items[i].Key();
		if (CodepointLength(norm_key) < 4) continue;
		if (norm_key != query) {
			double score = score_norm(norm_key);
			if (score >= 0)
				add_key_match(i, items[i].item->key, score, score == 1.0);
		}
		for (auto const& extra : extra_profiles) {
			if (norm_key == extra.profile.norm) continue;
			double score = score_against(norm_key, extra.profile);
			if (score >= 0)
				add_key_match(i, extra.raw, score, score == 1.0);
		}
	}

	// Bilingual record sources (whole string)
	for (auto const& np : pairs) {
		double score = score_norm(np.prepared->norm_source);
		if (score < 0) continue;
		MatchResult r;
		r.score = score;
		r.exact = score == 1.0;
		r.file = np.record->file;
		r.file_index = np.file_index;
		r.key = np.record->key;
		r.position = np.position;
		r.matched = np.record->source;
		r.replacement = np.record->translation;
		r.origin = "inline translation in " + np.record->file;
		emit(r);
	}

	// Segment matches on item text; align the matched segment with the
	// corresponding segment of same-key entries in other files.
	for (size_t idx = 0; idx < items.size(); ++idx) {
		NormalizedItem const& ni = items[idx];
		int best_k = -1;
		double best_score = -1.0;
		bool best_exact = false;
		for (size_t k = 0; k < ni.prepared->segments.size(); ++k) {
			double s = score_norm(ni.prepared->segments[k].norm);
			if (s < 0) continue;
			if (s > best_score) {
				best_score = s;
				best_k = static_cast<int>(k);
				best_exact = s == 1.0;
			}
		}
		if (best_k < 0) continue;
		auto const& seg =
			ni.prepared->segments[static_cast<size_t>(best_k)];
		emit_candidates(ni, seg.raw, best_k, seg.raw, seg.norm,
			"segment " + std::to_string(best_k + 1) + " of " + ni.item->file,
			false, best_score, best_exact);
	}

	// Segment matches on bilingual record sources; align with the matching
	// segment of the inline translation.
	for (auto const& np : pairs) {
		std::vector<int> segment_map;
		bool segment_map_ready = false;
		for (size_t k = 0; k < np.prepared->source_segments.size(); ++k) {
			double score = score_norm(np.prepared->source_segments[k].norm);
			if (score < 0) continue;

			if (!segment_map_ready) {
				segment_map = MapSegments(np.prepared->source_segments,
					np.prepared->translation_segments);
				segment_map_ready = true;
			}
			int target = k < segment_map.size() ? segment_map[k] : -1;
			if (target < 0) {
				if (np.prepared->translation_segments.size() == 1)
					target = 0;
				else
					continue;
			}

			MatchResult r;
			r.score = score;
			r.exact = score == 1.0;
			r.file = np.record->file;
			r.file_index = np.file_index;
			r.key = np.record->key;
			r.position = np.position;
			r.matched = np.prepared->source_segments[k].raw;
			r.replacement =
				np.prepared->translation_segments[static_cast<size_t>(target)].raw;
			r.origin = "segment " + std::to_string(target + 1) +
				" of inline translation in " + np.record->file;
			emit(r);
		}
	}

	// Files are indexed so two files with the same display name (e.g.
	// en/strings.json and zh/strings.json) keep distinct languages.
	auto file_language = [&](MatchResult const& r) -> std::string {
		if (r.file_index < files.size())
			return files[r.file_index].language;
		return {};
	};

	// Same-language text can only be a reference, never a translation. Hide it
	// only when the result list actually contains another language: otherwise
	// there is nothing to replace it with, and a Latin-script target that the
	// detector folds into "English" must not be deleted as if it were the
	// source. Results in the preferred (target) language always stay, so a
	// translator editing the localized script can still use them.
	if (options.hide_source_language && !options.preferred_language.empty()) {
		if (!query_language.empty()) {
			bool has_other_language = false;
			for (auto const& r : results) {
				std::string language = file_language(r);
				if (!language.empty() && language != query_language) {
					has_other_language = true;
					break;
				}
			}
			if (has_other_language) {
				results.erase(std::remove_if(results.begin(), results.end(),
					[&](MatchResult const& r) {
						std::string language = file_language(r);
						return !language.empty() &&
							language == query_language &&
							language != options.preferred_language;
					}), results.end());
			}
		}
	}

	// Anchor and actor context only re-order existing candidates; they never
	// add results below the threshold. Game scripts are usually sequential, so
	// entries near the last accepted one are more likely correct.
	if (context.anchor_file != static_cast<size_t>(-1) ||
		!context.actor.empty()) {
		std::string actor_norm = context.actor.empty()
			? std::string()
			: Normalize(context.actor, options);
		for (auto& r : results) {
			double bonus = 0.0;
			if (r.file_index == context.anchor_file) {
				size_t distance = r.position > context.anchor_position
					? r.position - context.anchor_position
					: context.anchor_position - r.position;
				if (distance <= 3) bonus += 0.05;
			}
			if (!actor_norm.empty()) {
				std::string key_norm = Normalize(r.key, options);
				if (!key_norm.empty() &&
					key_norm.find(actor_norm) != std::string::npos)
					bonus += 0.03;
			}
			if (bonus > 0.0) {
				double cap = r.exact ? 1.0 : 0.999999;
				r.score = std::min(cap, r.score + bonus);
			}
		}
	}

	auto compare = [&](MatchResult const& a, MatchResult const& b) {
		// The preferred language intentionally ranks above exactness: a
		// fuzzy match in the target language is more useful than an exact
		// match in some other language.
		if (!options.preferred_language.empty()) {
			bool pa = file_language(a) == options.preferred_language;
			bool pb = file_language(b) == options.preferred_language;
			if (pa != pb) return pa;
		}
		if (a.exact != b.exact) return a.exact;
		if (a.score != b.score) return a.score > b.score;
		// Prefer results whose localized text no longer contains text the
		// split regex would remove (e.g. unsplit {TA7} tags).
		if (options.prefer_without_split_regex && split_regex) {
			bool a_clean = !std::regex_search(a.replacement, *split_regex);
			bool b_clean = !std::regex_search(b.replacement, *split_regex);
			if (a_clean != b_clean) return a_clean;
		}
		if (a.from_key != b.from_key) return !a.from_key;
		if (a.file != b.file) return a.file < b.file;
		if (a.file_index != b.file_index) return a.file_index < b.file_index;
		if (a.key != b.key) return a.key < b.key;
		return a.replacement < b.replacement;
	};

	constexpr size_t max_results = 300;
	if (results.size() > max_results) {
		std::partial_sort(results.begin(), results.begin() + max_results,
			results.end(), compare);
		results.resize(max_results);
	}
	else {
		std::sort(results.begin(), results.end(), compare);
	}
	return results;
}

} // namespace localization
