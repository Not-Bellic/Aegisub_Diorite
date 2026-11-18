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

#include "hardsub_scan.h"

#include "format.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <map>

#include <wx/intl.h>

namespace hardsub {

RegionImage CropRegion(VideoFrame const& frame, int x, int y, int width, int height) {
	RegionImage out;
	if (frame.width == 0 || frame.height == 0 || frame.pitch < frame.width)
		return out;
	if (frame.pix_fmt == 0) {
		if (frame.pitch < frame.width * 4
		    || frame.data.size() < frame.pitch * frame.height)
			return out;
	}
	else {
		// YUV 4:2:0: luma plane plus two half-resolution chroma planes.
		size_t chroma_rows = frame.height / 2;
		size_t chroma_stride = frame.pix_fmt == 1 ? frame.pitch / 2 : frame.pitch;
		if (frame.pitch < frame.width
		    || frame.data.size() < frame.pitch * frame.height + chroma_stride * chroma_rows * (frame.pix_fmt == 1 ? 2 : 1))
			return out;
	}

	int x0 = std::max(0, x);
	int y0 = std::max(0, y);
	int x1 = std::min<int>(static_cast<int>(frame.width), x + width);
	int y1 = std::min<int>(static_cast<int>(frame.height), y + height);
	if (x1 <= x0 || y1 <= y0)
		return out;

	out.width = x1 - x0;
	out.height = y1 - y0;
	out.rgb.resize(size_t(out.width) * out.height * 3);

	// The scan decoder can hand us raw YUV 4:2:0 frames (8- or 10-bit) to
	// avoid the full-frame BGRA conversion that dominates the cost of the
	// normal provider path on 4K video. Convert only the requested region.
	if (frame.pix_fmt == 1 || frame.pix_fmt == 2 || frame.pix_fmt == 3) {
		const bool is_planar = frame.pix_fmt == 1;
		const bool is_10bit = frame.pix_fmt == 3;
		const size_t y_plane = 0;
		const size_t u_plane = frame.pitch * frame.height;
		const size_t v_plane = u_plane + (frame.pitch / 2) * (frame.height / 2);
		const size_t y_stride = frame.pitch;
		const size_t uv_stride = is_planar ? frame.pitch / 2 : frame.pitch;

		size_t out_pos = 0;
		for (int row = y0; row < y1; ++row) {
			int src_row = frame.flipped ? int(frame.height) - 1 - row : row;
			const unsigned char *y_row = frame.data.data() + y_plane
				+ size_t(src_row) * y_stride;
			for (int col = x0; col < x1; ++col) {
				int Y;
				if (is_10bit) {
					uint16_t yv = *reinterpret_cast<uint16_t const*>(
						y_row + size_t(col) * 2);
					Y = static_cast<int>((yv >> 6) * 255 / 1023);
				}
				else {
					Y = y_row[col];
				}
				unsigned char U, V;
				if (is_planar) {
					int cu = ((src_row / 2) * static_cast<int>(uv_stride) + (col / 2));
					U = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(
						    frame.data.data() + u_plane + cu * 2) >> 6) * 255 / 1023)
						: frame.data[u_plane + cu];
					V = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(
						    frame.data.data() + v_plane + cu * 2) >> 6) * 255 / 1023)
						: frame.data[v_plane + cu];
				}
				else {
					int cu = ((src_row / 2) * static_cast<int>(uv_stride)
					          + (col / 2) * (is_10bit ? 4 : 2));
					const unsigned char *uv_src = frame.data.data() + u_plane + cu;
					U = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(uv_src) >> 6) * 255 / 1023)
						: uv_src[0];
					V = is_10bit
						? static_cast<unsigned char>((*reinterpret_cast<uint16_t const*>(uv_src + 2) >> 6) * 255 / 1023)
						: uv_src[1];
				}
				// BT.709 limited-range YCbCr -> RGB
				double yf = (Y - 16) * 1.164;
				double cb = U - 128.0;
				double cr = V - 128.0;
				int r = static_cast<int>(yf + 1.575 * cr + 0.5);
				int g = static_cast<int>(yf - 0.187 * cb - 0.468 * cr + 0.5);
				int b = static_cast<int>(yf + 1.793 * cb + 0.5);
				out.rgb[out_pos++] = static_cast<unsigned char>(std::clamp(r, 0, 255));
				out.rgb[out_pos++] = static_cast<unsigned char>(std::clamp(g, 0, 255));
				out.rgb[out_pos++] = static_cast<unsigned char>(std::clamp(b, 0, 255));
			}
		}
		return out;
	}

	size_t out_pos = 0;
	for (int row = y0; row < y1; ++row) {
		int src_row = frame.flipped ? int(frame.height) - 1 - row : row;
		const unsigned char *src = frame.data.data() + size_t(src_row) * frame.pitch + size_t(x0) * 4;
		for (int col = x0; col < x1; ++col) {
			out.rgb[out_pos++] = src[2]; // R
			out.rgb[out_pos++] = src[1]; // G
			out.rgb[out_pos++] = src[0]; // B
			src += 4;
		}
	}
	return out;
}

RegionImage CropRegionScaled(VideoFrame const& frame, int x, int y, int width, int height,
                             int out_width, int out_height) {
	RegionImage out;
	if (out_width <= 0 || out_height <= 0)
		return out;
	if (frame.width == 0 || frame.height == 0 || frame.pitch < frame.width)
		return out;
	if (frame.pix_fmt == 0) {
		if (frame.pitch < frame.width * 4
		    || frame.data.size() < frame.pitch * frame.height)
			return out;
	}
	else {
		size_t chroma_rows = frame.height / 2;
		size_t chroma_stride = frame.pix_fmt == 1 ? frame.pitch / 2 : frame.pitch;
		if (frame.pitch < frame.width
		    || frame.data.size() < frame.pitch * frame.height + chroma_stride * chroma_rows * (frame.pix_fmt == 1 ? 2 : 1))
			return out;
	}

	int x0 = std::max(0, x);
	int y0 = std::max(0, y);
	int x1 = std::min<int>(static_cast<int>(frame.width), x + width);
	int y1 = std::min<int>(static_cast<int>(frame.height), y + height);
	if (x1 <= x0 || y1 <= y0)
		return out;

	const int src_w = x1 - x0;
	const int src_h = y1 - y0;
	if (out_width == src_w && out_height == src_h)
		return CropRegion(frame, x, y, width, height);

	out.width = out_width;
	out.height = out_height;
	out.rgb.assign(size_t(out_width) * out_height * 3, 0);

	// Map an output index to the half-open source range it averages. Uses wide
	// arithmetic so a very large source rectangle cannot overflow.
	auto block_begin = [](int index, int out_n, int src_n) {
		return static_cast<int>(static_cast<long long>(index) * src_n / out_n);
	};
	auto block_end = [](int index, int out_n, int src_n) {
		return static_cast<int>(static_cast<long long>(index + 1) * src_n / out_n);
	};

	if (frame.pix_fmt == 0) {
		for (int oy = 0; oy < out_height; ++oy) {
			int sy0 = y0 + block_begin(oy, out_height, src_h);
			int sy1 = y0 + block_end(oy, out_height, src_h);
			if (sy1 <= sy0)
				sy1 = sy0 + 1;
			for (int ox = 0; ox < out_width; ++ox) {
				int sx0 = x0 + block_begin(ox, out_width, src_w);
				int sx1 = x0 + block_end(ox, out_width, src_w);
				if (sx1 <= sx0)
					sx1 = sx0 + 1;

				unsigned long long r = 0, g = 0, b = 0;
				unsigned long long count = 0;
				for (int sy = sy0; sy < sy1; ++sy) {
					int src_row = frame.flipped ? int(frame.height) - 1 - sy : sy;
					const unsigned char *src = frame.data.data()
						+ size_t(src_row) * frame.pitch + size_t(sx0) * 4;
					for (int sx = sx0; sx < sx1; ++sx, src += 4) {
						b += src[0];
						g += src[1];
						r += src[2];
						++count;
					}
				}
				size_t pos = (size_t(oy) * out_width + ox) * 3;
				if (count > 0) {
					out.rgb[pos] = static_cast<unsigned char>(r / count);
					out.rgb[pos + 1] = static_cast<unsigned char>(g / count);
					out.rgb[pos + 2] = static_cast<unsigned char>(b / count);
				}
			}
		}
		return out;
	}

	// YUV 4:2:0 (8-bit planar, NV12 or 10-bit P010): average luma over the
	// source block and chroma over the chroma samples that block covers, then
	// run a single BT.709 limited-range conversion per output pixel.
	const bool is_planar = frame.pix_fmt == 1;
	const bool is_10bit = frame.pix_fmt == 3;
	const size_t y_plane = 0;
	const size_t u_plane = frame.pitch * frame.height;
	const size_t v_plane = u_plane + (frame.pitch / 2) * (frame.height / 2);
	const size_t y_stride = frame.pitch;
	const size_t uv_stride = is_planar ? frame.pitch / 2 : frame.pitch;

	for (int oy = 0; oy < out_height; ++oy) {
		int sy0 = y0 + block_begin(oy, out_height, src_h);
		int sy1 = y0 + block_end(oy, out_height, src_h);
		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		// Chroma rows follow the same vertical flip as the luma rows.
		int cy0, cy1;
		if (frame.flipped) {
			int fy0 = int(frame.height) - sy1;
			int fy1 = int(frame.height) - sy0;
			cy0 = fy0 / 2;
			cy1 = (fy1 - 1) / 2 + 1;
		}
		else {
			cy0 = sy0 / 2;
			cy1 = (sy1 - 1) / 2 + 1;
		}

		for (int ox = 0; ox < out_width; ++ox) {
			int sx0 = x0 + block_begin(ox, out_width, src_w);
			int sx1 = x0 + block_end(ox, out_width, src_w);
			if (sx1 <= sx0)
				sx1 = sx0 + 1;
			int cx0 = sx0 / 2;
			int cx1 = (sx1 - 1) / 2 + 1;

			// Average luma in the native bit depth, then expand to 8 bits.
			unsigned long long y_sum = 0;
			unsigned long long y_count = 0;
			for (int sy = sy0; sy < sy1; ++sy) {
				int src_row = frame.flipped ? int(frame.height) - 1 - sy : sy;
				const unsigned char *y_row = frame.data.data() + y_plane
					+ size_t(src_row) * y_stride;
				for (int sx = sx0; sx < sx1; ++sx) {
					if (is_10bit)
						y_sum += (*reinterpret_cast<uint16_t const*>(y_row + size_t(sx) * 2)) >> 6;
					else
						y_sum += y_row[sx];
					++y_count;
				}
			}
			unsigned long long y_avg = y_count > 0 ? y_sum / y_count : 0;
			int Y = is_10bit
				? static_cast<int>(y_avg * 255 / 1023)
				: static_cast<int>(y_avg);

			unsigned long long u_sum = 0, v_sum = 0;
			unsigned long long c_count = 0;
			for (int cy = cy0; cy < cy1; ++cy) {
				for (int cx = cx0; cx < cx1; ++cx) {
					int U, V;
					if (is_planar) {
						size_t idx = size_t(cy) * uv_stride + cx;
						if (is_10bit) {
							U = static_cast<int>(((*reinterpret_cast<uint16_t const*>(
								frame.data.data() + u_plane + idx * 2)) >> 6) * 255 / 1023);
							V = static_cast<int>(((*reinterpret_cast<uint16_t const*>(
								frame.data.data() + v_plane + idx * 2)) >> 6) * 255 / 1023);
						}
						else {
							U = frame.data[u_plane + idx];
							V = frame.data[v_plane + idx];
						}
					}
					else {
						size_t idx = u_plane + size_t(cy) * uv_stride
							+ size_t(cx) * (is_10bit ? 4 : 2);
						if (is_10bit) {
							U = static_cast<int>(((*reinterpret_cast<uint16_t const*>(
								frame.data.data() + idx)) >> 6) * 255 / 1023);
							V = static_cast<int>(((*reinterpret_cast<uint16_t const*>(
								frame.data.data() + idx + 2)) >> 6) * 255 / 1023);
						}
						else {
							U = frame.data[idx];
							V = frame.data[idx + 1];
						}
					}
					u_sum += U;
					v_sum += V;
					++c_count;
				}
			}
			int U = c_count > 0 ? static_cast<int>(u_sum / c_count) : 128;
			int V = c_count > 0 ? static_cast<int>(v_sum / c_count) : 128;

			double yf = (Y - 16) * 1.164;
			double cb = U - 128.0;
			double cr = V - 128.0;
			int r = static_cast<int>(yf + 1.575 * cr + 0.5);
			int g = static_cast<int>(yf - 0.187 * cb - 0.468 * cr + 0.5);
			int b = static_cast<int>(yf + 1.793 * cb + 0.5);
			size_t pos = (size_t(oy) * out_width + ox) * 3;
			out.rgb[pos] = static_cast<unsigned char>(std::clamp(r, 0, 255));
			out.rgb[pos + 1] = static_cast<unsigned char>(std::clamp(g, 0, 255));
			out.rgb[pos + 2] = static_cast<unsigned char>(std::clamp(b, 0, 255));
		}
	}
	return out;
}

double MeanAbsDiff(RegionImage const& a, RegionImage const& b) {
	if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height)
		return 255.0;

	unsigned long long total = 0;
	for (size_t i = 0; i < a.rgb.size(); ++i) {
		int d = int(a.rgb[i]) - int(b.rgb[i]);
		if (d < 0) d = -d;
		total += d;
	}
	return double(total) / (a.rgb.size() * 255.0) * 255.0;
}

LumaImage CropRegionLuma(VideoFrame const& frame, int x, int y, int width, int height,
                         int out_width, int out_height) {
	LumaImage out;
	if (out_width <= 0 || out_height <= 0)
		return out;
	if (frame.width == 0 || frame.height == 0 || frame.pitch < frame.width)
		return out;
	if (frame.pix_fmt == 0) {
		if (frame.pitch < frame.width * 4
		    || frame.data.size() < frame.pitch * frame.height)
			return out;
	}
	else {
		// YUV 4:2:0: the luma plane covers the whole frame, so a luma-only crop
		// needs no chroma sanity check beyond the plane itself.
		if (frame.data.size() < frame.pitch * frame.height)
			return out;
	}

	int x0 = std::max(0, x);
	int y0 = std::max(0, y);
	int x1 = std::min<int>(static_cast<int>(frame.width), x + width);
	int y1 = std::min<int>(static_cast<int>(frame.height), y + height);
	if (x1 <= x0 || y1 <= y0)
		return out;

	const int src_w = x1 - x0;
	const int src_h = y1 - y0;
	out.width = out_width;
	out.height = out_height;
	out.data.assign(static_cast<size_t>(out_width) * out_height, 0);

	auto block_begin = [](int index, int out_n, int src_n) {
		return static_cast<int>(static_cast<long long>(index) * src_n / out_n);
	};
	auto block_end = [](int index, int out_n, int src_n) {
		return static_cast<int>(static_cast<long long>(index + 1) * src_n / out_n);
	};

	const bool is_10bit = frame.pix_fmt == 3;
	const size_t y_stride = frame.pitch;

	// The output-to-source block map is the same for every row (columns) and
	// every column (rows). Computing it once per axis keeps the per-pixel loop
	// free of the 64-bit multiply-and-divide it used to run twice per output
	// pixel.
	std::vector<int> col_begin(out_width), col_end(out_width);
	for (int ox = 0; ox < out_width; ++ox) {
		col_begin[ox] = x0 + block_begin(ox, out_width, src_w);
		col_end[ox] = x0 + block_end(ox, out_width, src_w);
		if (col_end[ox] <= col_begin[ox])
			col_end[ox] = col_begin[ox] + 1;
	}
	std::vector<int> row_begin(out_height), row_end(out_height);
	for (int oy = 0; oy < out_height; ++oy) {
		row_begin[oy] = y0 + block_begin(oy, out_height, src_h);
		row_end[oy] = y0 + block_end(oy, out_height, src_h);
		if (row_end[oy] <= row_begin[oy])
			row_end[oy] = row_begin[oy] + 1;
	}

	// The average is one division per output pixel, and on a 4K region that is
	// hundreds of thousands of divisions per frame. Every scan scale the UI
	// offers is an exact fraction of the source, so in the common case all
	// blocks have the same pixel count: divide by a reciprocal instead.
	bool const uniform = (src_w % out_width == 0) && (src_h % out_height == 0);
	unsigned const uniform_count = uniform
		? static_cast<unsigned>(src_w / out_width) * static_cast<unsigned>(src_h / out_height)
		: 0;
	unsigned const uniform_recip = uniform_count > 1
		? static_cast<unsigned>((1ULL << 32) / uniform_count)
		: 0;

	for (int oy = 0; oy < out_height; ++oy) {
		int const sy0 = row_begin[oy];
		int const sy1 = row_end[oy];
		int const rows = sy1 - sy0;
		unsigned char *out_row = out.data.data() + static_cast<size_t>(oy) * out_width;

		for (int ox = 0; ox < out_width; ++ox) {
			int const sx0 = col_begin[ox];
			int const sx1 = col_end[ox];

			unsigned sum = 0;
			for (int sy = sy0; sy < sy1; ++sy) {
				int const src_row = frame.flipped ? int(frame.height) - 1 - sy : sy;
				const unsigned char *row = frame.data.data() + static_cast<size_t>(src_row) * y_stride;
				if (frame.pix_fmt == 0) {
					const unsigned char *p = row + static_cast<size_t>(sx0) * 4;
					for (int sx = sx0; sx < sx1; ++sx, p += 4)
						sum += (static_cast<unsigned>(p[0]) + p[1] + p[2]) / 3;
				}
				else if (is_10bit) {
					for (int sx = sx0; sx < sx1; ++sx) {
						uint16_t const v = *reinterpret_cast<uint16_t const*>(row + static_cast<size_t>(sx) * 2);
						sum += static_cast<unsigned>((v >> 6) * 255 / 1023);
					}
				}
				else {
					for (int sx = sx0; sx < sx1; ++sx)
						sum += row[sx];
				}
			}

			unsigned value = 0;
			if (uniform && uniform_count == 1) {
				// 1x1 blocks (an unscaled region): the average is the sample.
				value = sum;
			}
			else if (uniform && uniform_count > 1) {
				value = static_cast<unsigned>(
					(static_cast<unsigned long long>(sum) * uniform_recip) >> 32);
				// The reciprocal is truncated, so the quotient can be one low.
				if (static_cast<unsigned long long>(value + 1) * uniform_count <= sum)
					++value;
			}
			else if (rows > 0) {
				size_t const count = static_cast<size_t>(rows) * (sx1 - sx0);
				if (count > 0)
					value = sum / static_cast<unsigned>(count);
			}
			out_row[ox] = static_cast<unsigned char>(value);
		}
	}
	return out;
}

double LumaChangedRatio(LumaImage const& a, LumaImage const& b, int threshold) {
	if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height)
		return 1.0;

	size_t changed = 0;
	size_t const n = a.data.size();
	for (size_t i = 0; i < n; ++i) {
		int const d = static_cast<int>(a.data[i]) - static_cast<int>(b.data[i]);
		if (std::abs(d) > threshold)
			++changed;
	}
	return n > 0 ? double(changed) / n : 0.0;
}

unsigned long long LumaSignature(LumaImage const& img) {
	// 16x8 cells: coarse enough to ignore grain, fine enough to notice a
	// subtitle appearing or a cut.
	constexpr int kGridW = 16;
	constexpr int kGridH = 8;
	std::vector<unsigned char> grid(static_cast<size_t>(kGridW) * kGridH, 0);
	if (img.Valid()) {
		for (int cy = 0; cy < kGridH; ++cy) {
			int const ys = cy * img.height / kGridH;
			int const ye = std::max(ys + 1, std::min(img.height, (cy + 1) * img.height / kGridH));
			for (int cx = 0; cx < kGridW; ++cx) {
				int const xs = cx * img.width / kGridW;
				int const xe = std::max(xs + 1, std::min(img.width, (cx + 1) * img.width / kGridW));
				unsigned long long sum = 0;
				unsigned count = 0;
				for (int y = ys; y < ye; ++y) {
					const unsigned char *row = img.data.data() + static_cast<size_t>(y) * img.width;
					for (int x = xs; x < xe; ++x) {
						sum += row[x];
						++count;
					}
				}
				if (count > 0)
					grid[static_cast<size_t>(cy) * kGridW + cx] =
						static_cast<unsigned char>(sum / count);
			}
		}
	}

	unsigned long long h = 1469598103934665603ULL;
	for (unsigned char b : grid) {
		h ^= b;
		h *= 1099511628211ULL;
	}
	return h;
}

LumaImage CropLumaRegion(LumaImage const& src, int x, int y, int width, int height) {
	LumaImage out;
	if (!src.Valid() || width <= 0 || height <= 0)
		return out;
	if (x < 0 || y < 0 || x + width > src.width || y + height > src.height)
		return out;

	out.width = width;
	out.height = height;
	out.data.resize(static_cast<size_t>(width) * height);
	for (int row = 0; row < height; ++row)
		std::memcpy(out.data.data() + static_cast<size_t>(row) * width,
		            src.data.data() + static_cast<size_t>(y + row) * src.width + x,
		            static_cast<size_t>(width));
	return out;
}

std::vector<int> TemplateTextPixelsLuma(LumaImage const& tpl, int deviation) {
	std::vector<int> out;
	if (!tpl.Valid())
		return out;

	// Dominant background luminance: the mode of a coarse histogram, the same
	// shape as the RGB template mask.
	constexpr int kNumBins = 32;
	constexpr int kBinWidth = 256 / kNumBins;
	int bins[kNumBins] = {0};
	for (unsigned char value : tpl.data)
		++bins[std::min(kNumBins - 1, value / kBinWidth)];

	int mode_bin = 0;
	for (int b = 1; b < kNumBins; ++b)
		if (bins[b] > bins[mode_bin])
			mode_bin = b;

	int const bin_lo = mode_bin * kBinWidth;
	int const bin_hi = std::min(255, bin_lo + kBinWidth);
	long long bg_sum = 0;
	int bg_count = 0;
	for (unsigned char value : tpl.data) {
		if (value >= bin_lo && value < bin_hi) {
			bg_sum += value;
			++bg_count;
		}
	}
	int const bg_lum = bg_count > 0 ? static_cast<int>(bg_sum / bg_count) : 0;

	// Subtitle ink is near-white or near-black against the scene. Selecting by
	// luminance extremes relative to the background keeps scene texture out of
	// the comparison, so a moving camera does not move the coverage.
	int const bright_thresh = std::clamp(bg_lum + 80, 195, 235);
	int const dark_thresh = std::clamp(bg_lum - 80, 25, 60);
	bool const use_bright = bg_lum < 140;
	bool const use_dark = bg_lum > 200;

	for (size_t i = 0; i < tpl.data.size(); ++i) {
		int const l = tpl.data[i];
		bool text = false;
		if (use_bright)
			text = l >= bright_thresh;
		else if (use_dark)
			text = l <= dark_thresh;
		else
			text = l >= bright_thresh || l <= dark_thresh;
		if (text)
			out.push_back(static_cast<int>(i));
	}

	// Coloured text on a mid-luminance scene may not reach the extremes; fall
	// back to the deviation test, like the RGB mask does.
	if (out.size() < tpl.data.size() / 64) {
		out.clear();
		for (size_t i = 0; i < tpl.data.size(); ++i)
			if (std::abs(static_cast<int>(tpl.data[i]) - bg_lum) >= deviation)
				out.push_back(static_cast<int>(i));
	}
	return out;
}

double LumaTemplateCoverage(LumaImage const& frame, int x, int y,
                            LumaImage const& tpl,
                            std::vector<int> const& text_pixels,
                            int match_threshold) {
	if (!frame.Valid() || !tpl.Valid() || text_pixels.empty())
		return 0.0;
	if (x < 0 || y < 0 || x + tpl.width > frame.width || y + tpl.height > frame.height)
		return 0.0;

	int matched = 0;
	for (int px : text_pixels) {
		int const tx = px % tpl.width;
		int const ty = px / tpl.width;
		int const diff = std::abs(static_cast<int>(tpl.data[px]) -
		                          static_cast<int>(frame.data[static_cast<size_t>(y + ty) * frame.width + x + tx]));
		if (diff <= match_threshold)
			++matched;
	}
	return double(matched) / double(text_pixels.size());
}

double RegionChangedRatio(RegionImage const& a, RegionImage const& b, int pixel_threshold) {
	if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height)
		return 1.0;

	size_t changed = 0;
	for (size_t i = 0; i < a.rgb.size(); i += 3) {
		int dr = int(a.rgb[i]) - int(b.rgb[i]);
		int dg = int(a.rgb[i + 1]) - int(b.rgb[i + 1]);
		int db = int(a.rgb[i + 2]) - int(b.rgb[i + 2]);
		int d = std::max({std::abs(dr), std::abs(dg), std::abs(db)});
		if (d > pixel_threshold)
			++changed;
	}
	return double(changed) / (a.rgb.size() / 3);
}

std::vector<int> TemplateTextPixels(RegionImage const& tpl, int deviation) {
	std::vector<int> out;
	if (!tpl.Valid())
		return out;

	auto luminance = [](unsigned char r, unsigned char g, unsigned char b) {
		return (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) / 3;
	};

	// Dominant background luminance: the mode of a coarse histogram.
	constexpr int kNumBins = 32;
	constexpr int kBinWidth = 256 / kNumBins;
	std::vector<int> bins(kNumBins, 0);
	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int l = luminance(tpl.rgb[i], tpl.rgb[i + 1], tpl.rgb[i + 2]);
		++bins[std::min(kNumBins - 1, l / kBinWidth)];
	}

	int mode_bin = 0;
	for (int b = 1; b < kNumBins; ++b) {
		if (bins[b] > bins[mode_bin])
			mode_bin = b;
	}

	// Estimate the background color from the pixels in the dominant luminance
	// bin, then keep every pixel that differs from it strongly in any channel.
	// A per-channel test catches saturated text (e.g. yellow) that a pure
	// luminance test would miss.
	int bg_r = 0, bg_g = 0, bg_b = 0, bg_count = 0;
	int bin_lo = mode_bin * kBinWidth;
	int bin_hi = std::min(255, bin_lo + kBinWidth);
	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int l = luminance(tpl.rgb[i], tpl.rgb[i + 1], tpl.rgb[i + 2]);
		if (l >= bin_lo && l < bin_hi) {
			bg_r += tpl.rgb[i];
			bg_g += tpl.rgb[i + 1];
			bg_b += tpl.rgb[i + 2];
			++bg_count;
		}
	}
	if (bg_count > 0) {
		bg_r /= bg_count;
		bg_g /= bg_count;
		bg_b /= bg_count;
	}

	// Subtitle ink is usually near-white or near-black, standing out from the
	// scene. Selecting pixels by luminance extremes (relative to the
	// background) instead of "differs from the background" avoids picking up
	// scene texture, which would make the coverage unstable under camera
	// motion: scene pixels move with the camera, while burned-in text stays
	// put. A moving scene can otherwise make the coverage fluctuate so much
	// that high text-match thresholds split the run.
	int bg_lum = bg_count > 0 ? (bg_r + bg_g + bg_b) / 3 : 0;
	int bright_thresh = std::clamp(bg_lum + 80, 195, 235);
	int dark_thresh = std::clamp(bg_lum - 80, 25, 60);
	bool use_bright = bg_lum < 140;
	bool use_dark = bg_lum > 200;

	for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
		int l = luminance(tpl.rgb[i], tpl.rgb[i + 1], tpl.rgb[i + 2]);
		bool text = false;
		if (use_bright)
			text = l >= bright_thresh;
		else if (use_dark)
			text = l <= dark_thresh;
		else
			text = l >= bright_thresh || l <= dark_thresh;
		if (text)
			out.push_back(static_cast<int>(i / 3));
	}

	// Colored text on a mid-luminance scene may not reach the luminance
	// extremes; fall back to the background-deviation test in that case.
	if (out.size() < tpl.rgb.size() / 3 / 64) {
		out.clear();
		for (size_t i = 0; i < tpl.rgb.size(); i += 3) {
			int dr = std::abs(static_cast<int>(tpl.rgb[i]) - bg_r);
			int dg = std::abs(static_cast<int>(tpl.rgb[i + 1]) - bg_g);
			int db = std::abs(static_cast<int>(tpl.rgb[i + 2]) - bg_b);
			if (std::max({dr, dg, db}) >= deviation)
				out.push_back(static_cast<int>(i / 3));
		}
	}
	return out;
}

double TemplateTextCoverage(RegionImage const& tpl, RegionImage const& frame,
                            std::vector<int> const& text_pixels, int match_threshold) {
	if (!tpl.Valid() || !frame.Valid() || tpl.width != frame.width || tpl.height != frame.height
	    || text_pixels.empty())
		return 0.0;

	int matched = 0;
	for (int px : text_pixels) {
		size_t off = static_cast<size_t>(px) * 3;
		int d = 0;
		for (int c = 0; c < 3; ++c) {
			int diff = std::abs(static_cast<int>(tpl.rgb[off + c]) - static_cast<int>(frame.rgb[off + c]));
			d = std::max(d, diff);
		}
		if (d <= match_threshold)
			++matched;
	}
	return double(matched) / double(text_pixels.size());
}

std::vector<bool> ComputePresence(std::vector<double> const& coverage,
                                  std::vector<bool> const& ocr_box,
                                  double threshold, double threshold_exit) {
	size_t n = coverage.size();
	if (!ocr_box.empty() && ocr_box.size() != n)
		return {};

	std::vector<bool> presence(n, false);
	bool state = false;
	for (size_t i = 0; i < n; ++i) {
		double cov = coverage[i];
		if (cov < 0.0) {
			// Unevaluated frame: keep the previous state.
			presence[i] = state;
			continue;
		}
		bool cov_present;
		if (cov >= threshold)
			cov_present = true;
		else if (cov < threshold_exit)
			cov_present = false;
		else
			cov_present = state; // hysteresis zone: hold the current state
		state = cov_present && (ocr_box.empty() || ocr_box[i]);
		presence[i] = state;
	}
	return presence;
}

std::vector<bool> FillShortDips(std::vector<bool> const& present, int confirm_frames) {
	if (confirm_frames <= 0)
		return present;

	size_t n = present.size();
	std::vector<bool> filled = present;
	size_t i = 0;
	while (i < n) {
		if (filled[i]) {
			++i;
			continue;
		}
		size_t j = i;
		while (j < n && !filled[j])
			++j;
		// Only interior runs are filled; the window edges stay absent so a
		// run can never extend past the evaluated evidence.
		if (i > 0 && j < n && j - i < size_t(confirm_frames))
			std::fill(filled.begin() + i, filled.begin() + j, true);
		i = j;
	}
	return filled;
}

int RunStart(std::vector<bool> const& present, int anchor) {
	if (anchor < 0 || anchor >= static_cast<int>(present.size()) || !present[anchor])
		return anchor;
	int start = anchor;
	while (start > 0 && present[start - 1])
		--start;
	return start;
}

int RunEnd(std::vector<bool> const& present, int anchor) {
	if (anchor < 0 || anchor >= static_cast<int>(present.size()) || !present[anchor])
		return anchor;
	int end = anchor;
	while (end + 1 < static_cast<int>(present.size()) && present[end + 1])
		++end;
	return end;
}

int SnapBoundaryFromEvidence(std::vector<double> const& coverage,
                             std::vector<bool> const& ocr_box,
                             double threshold_exit, bool find_earliest, int fallback) {
	if (coverage.size() != ocr_box.size() || coverage.empty())
		return fallback;

	if (find_earliest) {
		for (size_t i = 0; i < coverage.size(); ++i) {
			if (coverage[i] >= threshold_exit && ocr_box[i])
				return static_cast<int>(i);
		}
	}
	else {
		for (size_t i = coverage.size(); i-- > 0;) {
			if (coverage[i] >= threshold_exit && ocr_box[i])
				return static_cast<int>(i);
		}
	}
	return fallback;
}

bool BoundaryAmbiguous(ScanResult const& result, double threshold_exit, bool start) {
	if (!result.ok)
		return true;

	int boundary = start ? result.start_frame : result.end_frame;
	auto it = std::lower_bound(result.frames.begin(), result.frames.end(), boundary);
	if (it == result.frames.end() || *it != boundary)
		return true; // the series is not dense enough to tell

	size_t idx = static_cast<size_t>(it - result.frames.begin());
	for (size_t off = 1; off <= 2; ++off) {
		if (start && off > idx)
			break;
		size_t at = start ? idx - off : idx + off;
		if (!start && at >= result.frames.size())
			break;
		int f = start ? boundary - static_cast<int>(off) : boundary + static_cast<int>(off);
		// A frame that should sit just outside the boundary is missing from
		// the evidence series: the series is not dense enough to rule out a
		// fade, so report the boundary as ambiguous.
		if (result.frames[at] != f)
			return true;
		if (result.diffs[at] >= threshold_exit)
			return true;
	}
	return false;
}

ScanResult ScanBoundaries(RegionImage const& tpl, int tpl_x, int tpl_y,
                          int base_frame, int frame_count,
                          ScanOptions const& opts,
                          FrameLoader const& loader,
                          CancelFn const& cancel,
                          ProgressFn const& progress,
                          std::vector<int> const& keyframes) {
	ScanResult out;
	if (!tpl.Valid() || frame_count <= 0 || base_frame < 0 || base_frame >= frame_count || !loader)
		return out;

	auto text_pixels = TemplateTextPixels(tpl);
	if (text_pixels.empty()) {
		out.error = agi::format(_("The selected region has no text-like pixels. Draw the box tighter around the subtitle."));
		return out;
	}

	// The template may be a downscaled crop (whole-video scan). In that case
	// crop the larger source rectangle from each frame and resample it to the
	// template's size so every coverage comparison is 1:1 with the template.
	const int source_width = opts.tpl_source_width > 0 ? opts.tpl_source_width : tpl.width;
	const int source_height = opts.tpl_source_height > 0 ? opts.tpl_source_height : tpl.height;
	if (source_width <= 0 || source_height <= 0)
		return out;
	const bool resample_source = source_width != tpl.width || source_height != tpl.height;

	double enter = std::clamp(opts.threshold, 0.0, 1.0);
	double exit = opts.threshold_exit > 0.0
		? std::clamp(opts.threshold_exit, 0.0, enter)
		: enter * 0.75;
	int confirm = std::max(0, opts.confirm_frames);

	out.base_frame = base_frame;
	int lo_bound = opts.max_frames > 0 ? std::max(0, base_frame - opts.max_frames) : 0;
	int hi_bound = opts.max_frames > 0 ? std::min(frame_count - 1, base_frame + opts.max_frames) : frame_count - 1;
	if (hi_bound < lo_bound)
		return out;

	std::map<int, double> coverage_cache;
	auto frame_coverage = [&](int f) -> double {
		auto it = coverage_cache.find(f);
		if (it != coverage_cache.end())
			return it->second;
		auto frame = loader(f);
		if (!frame)
			return -1.0;
		auto crop = resample_source
			? CropRegionScaled(*frame, tpl_x, tpl_y, source_width, source_height,
			                   tpl.width, tpl.height)
			: CropRegion(*frame, tpl_x, tpl_y, source_width, source_height);
		if (!crop.Valid())
			return -1.0;
		double coverage = TemplateTextCoverage(tpl, crop, text_pixels);
		coverage_cache.emplace(f, coverage);
		return coverage;
	};

	// ---- Locate a pass start before the run ---------------------------------
	// Random frame access on modern codecs (e.g. 4K/AV1) can cost about a
	// second per seek because a whole GOP must be decoded, while sequential
	// decoding is a few milliseconds per frame. The scan therefore avoids
	// arbitrary-frame probing entirely: it starts one contiguous ascending
	// pass at a keyframe before the run and extends that start backward
	// through keyframes only while the coverage there still looks like the
	// subtitle (a fade-in, or a run that began before the initial start).
	constexpr int kFallbackPreRoll = 120; // frames, when no keyframe list is given
	int pass_start;
	if (!keyframes.empty()) {
		auto it = std::upper_bound(keyframes.begin(), keyframes.end(), base_frame);
		if (it == keyframes.begin())
			pass_start = base_frame < keyframes.front() ? lo_bound : keyframes.front();
		else
			pass_start = *std::prev(it);
	}
	else {
		pass_start = std::max(lo_bound, base_frame - kFallbackPreRoll);
	}
	if (pass_start > base_frame)
		pass_start = base_frame;

	int evaluated = 0;
	auto progress_step = [&] {
		++evaluated;
		// Throttle UI progress events; a sequential pass evaluates frames
		// far too quickly to post one event per frame.
		if (progress && (evaluated % 16) == 0)
			progress(evaluated, evaluated);
	};

	// Extend the pass start backward while the coverage there is still
	// subtitle-like. Every probe lands on a keyframe, which is a cheap seek
	// (decode one frame) rather than a full GOP decode.
	for (;;) {
		if (cancel && cancel())
			return out;
		double cov = frame_coverage(pass_start);
		if (cov < 0.0)
			return out;
		progress_step();
		if (cov < exit)
			break; // clearly absent: the run starts after this frame
		int next;
		if (!keyframes.empty()) {
			auto it = std::lower_bound(keyframes.begin(), keyframes.end(), pass_start);
			next = it == keyframes.begin() ? -1 : *std::prev(it);
		}
		else {
			next = std::max(lo_bound, pass_start - kFallbackPreRoll);
		}
		if (next < 0 || next >= pass_start) {
			pass_start = lo_bound;
			break;
		}
		pass_start = next;
	}

	// ---- Sequential pass over the run ---------------------------------------
	// Decode forward from the pass start, recording coverage for every frame,
	// until the run has clearly ended after the base frame. The dense series
	// feeds the state machine below, which yields exact boundaries and splits
	// the run at interior gaps.
	const int kConfirmTail = std::max(4, confirm + 2);
	int absent_run = 0;
	int pass_end = pass_start;
	for (int f = pass_start; f <= hi_bound; ++f) {
		if (cancel && cancel())
			return out;
		double cov = frame_coverage(f);
		if (cov < 0.0)
			return out;
		progress_step();
		pass_end = f;
		if (cov < exit) {
			++absent_run;
			if (f > base_frame && absent_run >= kConfirmTail)
				break;
		}
		else {
			// Present or in the hysteresis zone: not a confirmed gap, so the
			// pass keeps going (a fade-out tail is not cut short, and a brief
			// occlusion does not end the run).
			absent_run = 0;
		}
	}

	// ---- Boundaries from the pass series ------------------------------------
	std::vector<double> cov;
	cov.reserve(pass_end - pass_start + 1);
	for (int f = pass_start; f <= pass_end; ++f)
		cov.push_back(coverage_cache.at(f));
	auto present = ComputePresence(cov, {}, enter, exit);
	auto filled = FillShortDips(present, confirm);
	int anchor = base_frame - pass_start;
	if (anchor < 0 || anchor >= static_cast<int>(filled.size()) || !filled[anchor])
		return out;
	int start = pass_start + RunStart(filled, anchor);
	int end = pass_start + RunEnd(filled, anchor);

	if (start < lo_bound || end > hi_bound || start > end || base_frame < start || base_frame > end)
		return out;

	int duration = end - start + 1;
	if (opts.min_duration_frames > 0 && duration < opts.min_duration_frames)
		return out;

	out.ok = true;
	out.start_frame = start;
	out.end_frame = end;
	// Report only the dense pass range so callers can rely on a contiguous
	// evidence series (interior-dip detection, boundary ambiguity checks).
	out.frames.reserve(pass_end - pass_start + 1);
	out.diffs.reserve(pass_end - pass_start + 1);
	for (int f = pass_start; f <= pass_end; ++f) {
		out.frames.push_back(f);
		out.diffs.push_back(coverage_cache.at(f));
	}
	return out;
}

} // namespace hardsub
