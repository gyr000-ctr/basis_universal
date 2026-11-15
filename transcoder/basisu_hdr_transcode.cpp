// basisu_hdr_transcode.cpp
// Copyright (C) 2019-2026 Binomial LLC. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// See basisu_hdr_transcode.h for the design contract (explicit errors, bounded
// memory, no threads, SDR paths untouched).
#include "basisu_hdr_transcode.h"
#include "basisu_astc_helpers.h"

#include <cmath>
#include <cstring>

#ifndef BASISD_SUPPORT_UASTC_HDR
	#define BASISD_SUPPORT_UASTC_HDR 1
#endif

namespace basist
{
	const char* hdr_transcode_error_to_string(hdr_transcode_error err)
	{
		switch (err)
		{
		case hdr_transcode_error::cSuccess: return "Success";
		case hdr_transcode_error::cInvalidArgument: return "Invalid argument (null pointer, bad dimensions, or bad pitch)";
		case hdr_transcode_error::cDataTooSmall: return "Input buffer too small for the declared block dimensions";
		case hdr_transcode_error::cUnsupportedTargetFormat: return "Unsupported target format for checked HDR transcoding";
		case hdr_transcode_error::cUnsupportedOperation: return "Unsupported operation for this HDR target format";
		case hdr_transcode_error::cInvalidExposure: return "Invalid exposure (must be finite, > 0, and bounded)";
		case hdr_transcode_error::cNanInfInput: return "NaN or Inf texel in HDR source data";
		case hdr_transcode_error::cBlockUnpackFailed: return "Source block is corrupted or not valid UASTC HDR 4x4";
		case hdr_transcode_error::cBlockTranscodeFailed: return "All BC6H encoding candidates failed for a block";
		case hdr_transcode_error::cErrorTooHigh: return "Best block candidate error exceeded the configured threshold";
		default: return "Unknown HDR transcode error";
		}
	}

	float hdr_exposure_stops_to_scale(float stops)
	{
		if (!std::isfinite(stops))
			return 0.0f;

		const float scale = std::exp2(stops);
		if (!std::isfinite(scale) || (scale <= 0.0f))
			return 0.0f;

		return scale;
	}

	// Validates the options that are cheap to check once per call.
	static hdr_transcode_error validate_hdr_options(const hdr_transcode_options& options)
	{
		if (!std::isfinite(options.m_exposure_scale) ||
			(options.m_exposure_scale <= 0.0f) ||
			(options.m_exposure_scale > HDR_TRANSCODE_MAX_EXPOSURE))
		{
			return hdr_transcode_error::cInvalidExposure;
		}

		if ((uint32_t)options.m_error_metric > (uint32_t)hdr_error_metric::cLog2)
			return hdr_transcode_error::cInvalidArgument;

		if (!std::isnan(options.m_max_block_error) && (options.m_max_block_error < 0.0))
			return hdr_transcode_error::cInvalidArgument;

		return hdr_transcode_error::cSuccess;
	}

	// Local half-float Inf/NaN check (exponent all 1's). Kept local because the
	// transcoder's equivalent helper lives in the astc_helpers implementation
	// section, which is only compiled into basisu_transcoder.cpp.
	static inline bool hdr_half_is_inf_or_nan(half_float v)
	{
		return (v & 0x7C00) == 0x7C00;
	}

	hdr_transcode_error sanitize_hdr_texels(
		half_float* pTexels, uint32_t num_texels, uint32_t num_comps,
		const hdr_transcode_options& options, hdr_transcode_stats* pStats)
	{
		if (!pTexels || !num_texels || (num_comps < 1) || (num_comps > 4))
			return hdr_transcode_error::cInvalidArgument;

		const hdr_transcode_error options_err = validate_hdr_options(options);
		if (options_err != hdr_transcode_error::cSuccess)
			return options_err;

		const uint32_t total_values = num_texels * num_comps;

		for (uint32_t i = 0; i < total_values; i++)
		{
			half_float h = pTexels[i];

			if (hdr_half_is_inf_or_nan(h))
			{
				if (options.m_fail_on_nan_inf)
					return hdr_transcode_error::cNanInfInput;

				// Deterministic sanitization: NaN -> 0, +Inf -> max, -Inf -> 0.
				const bool is_nan = (h & 0x7FFF) > 0x7C00;
				const bool is_negative = (h & 0x8000) != 0;

				h = (is_nan || is_negative) ? (half_float)0 : (half_float)MAX_BC6H_HALF_FLOAT_AS_UINT;

				if (pStats)
					pStats->m_num_nan_inf_sanitized++;
			}

			float v = astc_helpers::half_to_float(h);

			if (v < 0.0f)
			{
				if (!options.m_clamp_negatives)
				{
					// Pass through, but the value must still be finite and in half range.
					v = v * options.m_exposure_scale;
					if (!std::isfinite(v) || (v < -HDR_TRANSCODE_MAX_HALF_VALUE))
					{
						v = -HDR_TRANSCODE_MAX_HALF_VALUE;
						if (pStats)
							pStats->m_num_values_clamped++;
					}
					pTexels[i] = astc_helpers::float_to_half(v, true);
					continue;
				}

				v = 0.0f;
				if (pStats)
					pStats->m_num_negatives_clamped++;
			}

			v = v * options.m_exposure_scale;

			// v can only be Inf here if exposure overflowed a huge input; clamp it.
			if (!std::isfinite(v) || (v > HDR_TRANSCODE_MAX_HALF_VALUE))
			{
				v = HDR_TRANSCODE_MAX_HALF_VALUE;
				if (pStats)
					pStats->m_num_values_clamped++;
			}

			pTexels[i] = astc_helpers::float_to_half(v, true);
		}

		return hdr_transcode_error::cSuccess;
	}

#if BASISD_SUPPORT_UASTC_HDR

	hdr_transcode_error decode_uastc_hdr_4x4_block_checked(
		const astc_blk& src_blk, half_float* pDst_rgba_halfs,
		const hdr_transcode_options& options, hdr_transcode_stats* pStats)
	{
		if (!pDst_rgba_halfs)
			return hdr_transcode_error::cInvalidArgument;

		const hdr_transcode_error options_err = validate_hdr_options(options);
		if (options_err != hdr_transcode_error::cSuccess)
			return options_err;

		astc_helpers::log_astc_block log_blk;
		if (!astc_helpers::unpack_block(&src_blk, log_blk, HDR_TRANSCODE_BLOCK_WIDTH, HDR_TRANSCODE_BLOCK_HEIGHT))
			return hdr_transcode_error::cBlockUnpackFailed;

		// Fixed-size stack buffer: 4*4*4 halfs = 128 bytes.
		half_float blk_texels[HDR_TRANSCODE_BLOCK_TEXELS][4];
		if (!astc_helpers::decode_block(log_blk, blk_texels, HDR_TRANSCODE_BLOCK_WIDTH, HDR_TRANSCODE_BLOCK_HEIGHT, astc_helpers::cDecodeModeHDR16))
			return hdr_transcode_error::cBlockUnpackFailed;

		// Sanitize all 4 channels (alpha from RGB-only ASTC HDR modes decodes to
		// 1.0, but sanitize anyway so corrupted alpha can't leak through).
		hdr_transcode_error err = sanitize_hdr_texels(&blk_texels[0][0], HDR_TRANSCODE_BLOCK_TEXELS, 4, options, pStats);
		if (err != hdr_transcode_error::cSuccess)
			return err;

		const half_float opaque_alpha = astc_helpers::float_to_half(1.0f, true);

		for (uint32_t i = 0; i < HDR_TRANSCODE_BLOCK_TEXELS; i++)
		{
			pDst_rgba_halfs[i * 4 + 0] = blk_texels[i][0];
			pDst_rgba_halfs[i * 4 + 1] = blk_texels[i][1];
			pDst_rgba_halfs[i * 4 + 2] = blk_texels[i][2];
			pDst_rgba_halfs[i * 4 + 3] = options.m_force_opaque_alpha ? opaque_alpha : blk_texels[i][3];
		}

		return hdr_transcode_error::cSuccess;
	}

	// Extracts the 48 RGB halfs from 64 RGBA halfs.
	static void extract_rgb_from_rgba(const half_float* pRGBA, half_float* pRGB)
	{
		for (uint32_t i = 0; i < HDR_TRANSCODE_BLOCK_TEXELS; i++)
		{
			pRGB[i * 3 + 0] = pRGBA[i * 4 + 0];
			pRGB[i * 3 + 1] = pRGBA[i * 4 + 1];
			pRGB[i * 3 + 2] = pRGBA[i * 4 + 2];
		}
	}

	// RMSE between two 16-texel RGB half blocks, in the requested metric space.
	static double compute_hdr_block_error(const half_float* pSrc_rgb, const half_float* pCand_rgb, hdr_error_metric metric)
	{
		const uint32_t total_values = HDR_TRANSCODE_BLOCK_TEXELS * 3;
		double total_err = 0.0;

		for (uint32_t i = 0; i < total_values; i++)
		{
			double a = astc_helpers::half_to_float(pSrc_rgb[i]);
			double b = astc_helpers::half_to_float(pCand_rgb[i]);

			if (metric == hdr_error_metric::cLog2)
			{
				// Perceptual space; clamp to a small positive floor. Source
				// texels were already sanitized to [0, MAX_HALF].
				const double floor_v = 1.0 / 65504.0;
				a = log2(a > floor_v ? a : floor_v);
				b = log2(b > floor_v ? b : floor_v);
			}

			const double d = a - b;
			total_err += d * d;
		}

		return sqrt(total_err / (double)total_values);
	}

	hdr_transcode_error transcode_uastc_hdr_4x4_block_to_bc6h(
		const astc_blk& src_blk, bc6h_block& dst_blk,
		const hdr_transcode_options& options, hdr_transcode_stats* pStats)
	{
		// Decode + sanitize + exposure. Explicit errors propagate.
		half_float rgba_texels[HDR_TRANSCODE_BLOCK_TEXELS * 4];
		hdr_transcode_error err = decode_uastc_hdr_4x4_block_checked(src_blk, rgba_texels, options, pStats);
		if (err != hdr_transcode_error::cSuccess)
			return err;

		half_float src_rgb[HDR_TRANSCODE_BLOCK_TEXELS * 3];
		extract_rgb_from_rgba(rgba_texels, src_rgb);

		// BC6H is unsigned: clamp the candidate source to [0, MAX_HALF] even if
		// the options allowed negatives through to the half-float outputs.
		for (uint32_t i = 0; i < HDR_TRANSCODE_BLOCK_TEXELS * 3; i++)
		{
			if (src_rgb[i] & 0x8000)
				src_rgb[i] = 0;
		}

		// Bounded candidate set: direct endpoint transcode, texel re-encode, solid fallback.
		const uint32_t MAX_CANDIDATES = 3;
		bc6h_block candidates[MAX_CANDIDATES];
		uint32_t num_candidates = 0;
		bool direct_valid = false, reencode_valid = false, solid_valid = false;

		// Candidate 1: direct ASTC->BC6H endpoint transcode. Only meaningful when
		// the exposure is identity (the endpoint transcode cannot apply exposure).
		if (options.m_exposure_scale == 1.0f)
		{
			astc_helpers::log_astc_block log_blk;
			if (astc_helpers::unpack_block(&src_blk, log_blk, HDR_TRANSCODE_BLOCK_WIDTH, HDR_TRANSCODE_BLOCK_HEIGHT))
			{
				if (astc_hdr_transcode_to_bc6h(log_blk, candidates[num_candidates]))
				{
					direct_valid = true;
					num_candidates++;
				}
			}
		}

		// Candidate 2: re-encode the sanitized/exposed texels. Always available.
		{
			astc_6x6_hdr::fast_bc6h_params params;
			astc_6x6_hdr::fast_encode_bc6h(src_rgb, &candidates[num_candidates], params);
			reencode_valid = true;
			num_candidates++;
		}

		// Candidate 3: solid-color fallback from the block average.
		if (options.m_allow_solid_fallback)
		{
			float avg[3] = { 0.0f, 0.0f, 0.0f };
			for (uint32_t i = 0; i < HDR_TRANSCODE_BLOCK_TEXELS; i++)
				for (uint32_t c = 0; c < 3; c++)
					avg[c] += astc_helpers::half_to_float(src_rgb[i * 3 + c]);

			half_float avg_h[3];
			for (uint32_t c = 0; c < 3; c++)
			{
				float v = avg[c] * (1.0f / (float)HDR_TRANSCODE_BLOCK_TEXELS);
				if (!std::isfinite(v) || (v > HDR_TRANSCODE_MAX_HALF_VALUE))
					v = HDR_TRANSCODE_MAX_HALF_VALUE;
				if (v < 0.0f)
					v = 0.0f;
				avg_h[c] = astc_helpers::float_to_half(v, true);
			}

			if (bc6h_enc_block_solid_color(&candidates[num_candidates], avg_h))
			{
				solid_valid = true;
				num_candidates++;
			}
		}

		if (!num_candidates)
			return hdr_transcode_error::cBlockTranscodeFailed;

		// Block-level error selection: decode each candidate and score it
		// against the sanitized source texels.
		uint32_t best_index = 0;
		double best_err = 0.0;
		bool best_valid = false;

		for (uint32_t i = 0; i < num_candidates; i++)
		{
			half_float cand_rgb[HDR_TRANSCODE_BLOCK_TEXELS * 3];
			if (!unpack_bc6h_checked(&candidates[i], cand_rgb, false))
				continue;

			const double err_i = compute_hdr_block_error(src_rgb, cand_rgb, options.m_error_metric);
			if (!best_valid || (err_i < best_err))
			{
				best_valid = true;
				best_err = err_i;
				best_index = i;
			}
		}

		if (!best_valid)
			return hdr_transcode_error::cBlockTranscodeFailed;

		if ((options.m_max_block_error > 0.0) && (best_err > options.m_max_block_error))
			return hdr_transcode_error::cErrorTooHigh;

		dst_blk = candidates[best_index];

		if (pStats)
		{
			// Candidate slot order tracks the *_valid flags above.
			uint32_t slot = 0;
			if (direct_valid)
			{
				if (best_index == slot)
					pStats->m_num_direct_transcodes++;
				slot++;
			}
			if (reencode_valid)
			{
				if (best_index == slot)
					pStats->m_num_reencoded++;
				slot++;
			}
			if (solid_valid)
			{
				if (best_index == slot)
					pStats->m_num_solid_fallbacks++;
				slot++;
			}

			pStats->m_total_error += best_err;
			if (best_err > pStats->m_max_error)
				pStats->m_max_error = best_err;
		}

		return hdr_transcode_error::cSuccess;
	}

	hdr_transcode_error transcode_uastc_hdr_4x4_slice_checked(
		void* pDst_blocks,
		uint32_t num_blocks_x, uint32_t num_blocks_y,
		const uint8_t* pImage_data, uint32_t image_data_size,
		transcoder_texture_format fmt,
		uint32_t orig_width, uint32_t orig_height,
		uint32_t output_row_pitch_in_blocks_or_pixels,
		const hdr_transcode_options* pOptions,
		hdr_transcode_stats* pStats)
	{
		hdr_transcode_options default_options;
		const hdr_transcode_options& options = pOptions ? *pOptions : default_options;

		if (!pDst_blocks || !pImage_data)
			return hdr_transcode_error::cInvalidArgument;

		if (!num_blocks_x || !num_blocks_y ||
			(num_blocks_x > HDR_TRANSCODE_MAX_BLOCKS_PER_DIM) ||
			(num_blocks_y > HDR_TRANSCODE_MAX_BLOCKS_PER_DIM))
		{
			return hdr_transcode_error::cInvalidArgument;
		}

		const hdr_transcode_error options_err = validate_hdr_options(options);
		if (options_err != hdr_transcode_error::cSuccess)
			return options_err;

		// Overflow-safe required input size (dims are bounded above).
		const uint64_t total_blocks = (uint64_t)num_blocks_x * (uint64_t)num_blocks_y;
		const uint64_t total_expected_bytes = total_blocks * sizeof(astc_blk);
		if ((uint64_t)image_data_size < total_expected_bytes)
			return hdr_transcode_error::cDataTooSmall;

		// Validate target and set up strides.
		bool is_uncompressed = false;
		uint32_t output_stride_in_bytes = 0;
		switch (fmt)
		{
		case transcoder_texture_format::cTFASTC_HDR_4x4_RGBA:
		case transcoder_texture_format::cTFBC6H:
			output_stride_in_bytes = 16;
			break;
		case transcoder_texture_format::cTFRGB_HALF:
			output_stride_in_bytes = 6;
			is_uncompressed = true;
			break;
		case transcoder_texture_format::cTFRGBA_HALF:
			output_stride_in_bytes = 8;
			is_uncompressed = true;
			break;
		default:
			return hdr_transcode_error::cUnsupportedTargetFormat;
		}

		uint32_t row_pitch = output_row_pitch_in_blocks_or_pixels;
		if (!row_pitch)
			row_pitch = is_uncompressed ? orig_width : num_blocks_x;

		if (is_uncompressed)
		{
			if (!orig_width || !orig_height || (row_pitch < orig_width))
				return hdr_transcode_error::cInvalidArgument;

			// The block dimensions must be consistent with the pixel dimensions,
			// otherwise edge clipping cannot bound the output writes.
			if ((num_blocks_x != (orig_width + HDR_TRANSCODE_BLOCK_WIDTH - 1) / HDR_TRANSCODE_BLOCK_WIDTH) ||
				(num_blocks_y != (orig_height + HDR_TRANSCODE_BLOCK_HEIGHT - 1) / HDR_TRANSCODE_BLOCK_HEIGHT))
			{
				return hdr_transcode_error::cInvalidArgument;
			}
		}
		else
		{
			if (row_pitch < num_blocks_x)
				return hdr_transcode_error::cInvalidArgument;
		}

		// ASTC HDR passthrough cannot apply exposure (that would require a full
		// ASTC re-encode); fail explicitly instead of silently ignoring it.
		if ((fmt == transcoder_texture_format::cTFASTC_HDR_4x4_RGBA) && (options.m_exposure_scale != 1.0f))
			return hdr_transcode_error::cUnsupportedOperation;

		const astc_blk* pSrc_block = reinterpret_cast<const astc_blk*>(pImage_data);

		for (uint32_t block_y = 0; block_y < num_blocks_y; block_y++)
		{
			for (uint32_t block_x = 0; block_x < num_blocks_x; block_x++, pSrc_block++)
			{
				hdr_transcode_error err = hdr_transcode_error::cSuccess;

				switch (fmt)
				{
				case transcoder_texture_format::cTFASTC_HDR_4x4_RGBA:
				{
					// Validated passthrough: the block must unpack cleanly.
					astc_helpers::log_astc_block log_blk;
					if (!astc_helpers::unpack_block(pSrc_block, log_blk, HDR_TRANSCODE_BLOCK_WIDTH, HDR_TRANSCODE_BLOCK_HEIGHT))
						return hdr_transcode_error::cBlockUnpackFailed;

					uint8_t* pDst = (uint8_t*)pDst_blocks + ((uint64_t)block_y * row_pitch + block_x) * output_stride_in_bytes;
					memcpy(pDst, pSrc_block, sizeof(astc_blk));
					break;
				}
				case transcoder_texture_format::cTFBC6H:
				{
					uint8_t* pDst = (uint8_t*)pDst_blocks + ((uint64_t)block_y * row_pitch + block_x) * output_stride_in_bytes;

					err = transcode_uastc_hdr_4x4_block_to_bc6h(*pSrc_block, *(bc6h_block*)pDst, options, pStats);
					if (err != hdr_transcode_error::cSuccess)
						return err;
					break;
				}
				case transcoder_texture_format::cTFRGB_HALF:
				case transcoder_texture_format::cTFRGBA_HALF:
				{
					half_float rgba_texels[HDR_TRANSCODE_BLOCK_TEXELS * 4];
					err = decode_uastc_hdr_4x4_block_checked(*pSrc_block, rgba_texels, options, pStats);
					if (err != hdr_transcode_error::cSuccess)
						return err;

					const uint32_t comps = (fmt == transcoder_texture_format::cTFRGBA_HALF) ? 4 : 3;

					// Clip edge blocks to the image bounds.
					const uint32_t max_x = basisu::minimum<uint32_t>(HDR_TRANSCODE_BLOCK_WIDTH, orig_width - block_x * HDR_TRANSCODE_BLOCK_WIDTH);
					const uint32_t max_y = basisu::minimum<uint32_t>(HDR_TRANSCODE_BLOCK_HEIGHT, orig_height - block_y * HDR_TRANSCODE_BLOCK_HEIGHT);

					for (uint32_t y = 0; y < max_y; y++)
					{
						half_float* pDst = (half_float*)((uint8_t*)pDst_blocks +
							((uint64_t)(block_y * HDR_TRANSCODE_BLOCK_HEIGHT + y) * row_pitch + block_x * HDR_TRANSCODE_BLOCK_WIDTH) * comps * sizeof(half_float));

						for (uint32_t x = 0; x < max_x; x++)
						{
							const half_float* pSrc_texel = &rgba_texels[(y * HDR_TRANSCODE_BLOCK_WIDTH + x) * 4];
							pDst[x * comps + 0] = pSrc_texel[0];
							pDst[x * comps + 1] = pSrc_texel[1];
							pDst[x * comps + 2] = pSrc_texel[2];
							if (comps == 4)
								pDst[x * comps + 3] = pSrc_texel[3];
						}
					}
					break;
				}
				default:
					// Unreachable: validated above.
					return hdr_transcode_error::cUnsupportedTargetFormat;
				}

				if (pStats)
					pStats->m_num_blocks++;
			}
		}

		return hdr_transcode_error::cSuccess;
	}

	// ------------------------------------------------------------------
	// Bounded BC6H decoder (all 14 modes). Ported to the transcoder's shared
	// mode/layout tables so it cannot diverge from the encoder.
	// ------------------------------------------------------------------
	namespace
	{
		inline int hdr_bc6h_sign_extend(int val, int bits)
		{
			if (bits >= 16)
				return val;
			const int sign_bit = 1 << (bits - 1);
			if (val & sign_bit)
				val -= (1 << bits);
			return val;
		}

		inline int hdr_bc6h_apply_delta(int base, int delta, int num_bits, bool is_signed)
		{
			const int bitmask = (1 << num_bits) - 1;
			const int v = (base + delta) & bitmask;
			return is_signed ? hdr_bc6h_sign_extend(v, num_bits) : v;
		}

		int hdr_bc6h_dequantize(int val, int bits, bool is_signed)
		{
			if (is_signed)
			{
				if (bits >= 16)
					return val;

				int s_flag = 0;
				if (val < 0)
				{
					s_flag = 1;
					val = -val;
				}

				int result;
				if (val == 0)
					result = 0;
				else if (val >= ((1 << (bits - 1)) - 1))
					result = 0x7FFF;
				else
					result = ((val << 15) + 0x4000) >> (bits - 1);

				return s_flag ? -result : result;
			}
			else
			{
				if (bits >= 15)
					return val;
				else if (!val)
					return 0;
				else if (val == ((1 << bits) - 1))
					return 0xFFFF;
				else
					return ((val << 16) + 0x8000) >> bits;
			}
		}

		inline int hdr_bc6h_interpolate(int a, int b, const uint8_t* pWeights, int index)
		{
			return (a * (64 - (int)pWeights[index]) + b * (int)pWeights[index] + 32) >> 6;
		}

		inline half_float hdr_bc6h_convert_to_half(int val, bool is_signed)
		{
			if (!is_signed)
			{
				// scale by 31/64
				return (half_float)((val * 31) >> 6);
			}

			// scale by 31/32
			val = (val < 0) ? -(((-val) * 31) >> 5) : (val * 31) >> 5;

			int s = 0;
			if (val < 0)
			{
				s = 0x8000;
				val = -val;
			}

			return (half_float)(s | val);
		}

		inline uint32_t hdr_bc6h_get_bits(uint32_t num_bits, uint64_t& l, uint64_t& h, uint32_t& total_bits)
		{
			const uint32_t v = (uint32_t)(l & ((1U << num_bits) - 1U));

			l >>= num_bits;
			l |= (h << (64U - num_bits));
			h >>= num_bits;

			total_bits += num_bits;

			return v;
		}

		inline uint32_t hdr_bc6h_reverse_bits(uint32_t v, uint32_t num_bits)
		{
			uint32_t res = 0;
			for (uint32_t i = 0; i < num_bits; i++)
			{
				const uint32_t bit = (v & (1u << i)) != 0u;
				res |= (bit << (num_bits - 1u - i));
			}
			return res;
		}
	} // anonymous namespace

	bool unpack_bc6h_checked(const void* pSrc_block, half_float* pDst_rgb_halfs, bool is_signed)
	{
		if (!pSrc_block || !pDst_rgb_halfs)
			return false;

		const uint8_t* pSrc = static_cast<const uint8_t*>(pSrc_block);

		uint64_t blo = (uint64_t)basisu::read_le_dword(pSrc) | ((uint64_t)basisu::read_le_dword(pSrc + 4) << 32U);
		uint64_t bhi = (uint64_t)basisu::read_le_dword(pSrc + 8) | ((uint64_t)basisu::read_le_dword(pSrc + 12) << 32U);

		// Unpack mode
		const int mode = g_bc6h_mode_lookup[blo & 31];
		if (mode < 0)
			return false;

		// Skip mode bits
		uint32_t total_bits_read = 0;
		hdr_bc6h_get_bits((mode < 2) ? 2 : 5, blo, bhi, total_bits_read);

		const uint32_t num_subsets = (mode >= (int)BC6H_FIRST_1SUBSET_MODE_INDEX) ? 1 : 2;
		const bool is_mode_9_or_10 = (mode == 9) || (mode == 10);

		// Unpack endpoint components
		int comps[2][3][2] = { { { 0 } } };	// [subset][comp][l/h]
		int part_index = 0;

		for (uint32_t layout_index = 0; layout_index < MAX_BC6H_LAYOUT_INDEX; layout_index++)
		{
			const bc6h_bit_layout& layout = g_bc6h_bit_layouts[mode][layout_index];
			if (layout.m_comp < 0)
				break;

			const int subset = layout.m_index >> 1, lh_index = layout.m_index & 1;

			const int last_bit = layout.m_last_bit, first_bit = layout.m_first_bit;

			int& res = (layout.m_comp == 3) ? part_index : comps[subset][layout.m_comp][lh_index];

			if (first_bit < 0)
			{
				res |= (hdr_bc6h_get_bits(1, blo, bhi, total_bits_read) << last_bit);
			}
			else
			{
				const int total_bits = basisu::iabs(last_bit - first_bit) + 1;
				const int bit_shift = basisu::minimum(first_bit, last_bit);

				int b = hdr_bc6h_get_bits(total_bits, blo, bhi, total_bits_read);

				if (last_bit < first_bit)
					b = hdr_bc6h_reverse_bits(b, total_bits);

				res |= (b << bit_shift);
			}
		}

		// Sign extend/dequantize endpoints
		const int num_sig_bits = g_bc6h_mode_sig_bits[mode][0];
		if (is_signed)
		{
			for (uint32_t comp = 0; comp < 3; comp++)
				comps[0][comp][0] = hdr_bc6h_sign_extend(comps[0][comp][0], num_sig_bits);
		}

		if (is_signed || !is_mode_9_or_10)
		{
			for (uint32_t subset = 0; subset < num_subsets; subset++)
				for (uint32_t comp = 0; comp < 3; comp++)
					for (uint32_t lh = (subset ? 0 : 1); lh < 2; lh++)
						comps[subset][comp][lh] = hdr_bc6h_sign_extend(comps[subset][comp][lh], g_bc6h_mode_sig_bits[mode][1 + comp]);
		}

		if (!is_mode_9_or_10)
		{
			for (uint32_t subset = 0; subset < num_subsets; subset++)
				for (uint32_t comp = 0; comp < 3; comp++)
					for (uint32_t lh = (subset ? 0 : 1); lh < 2; lh++)
						comps[subset][comp][lh] = hdr_bc6h_apply_delta(comps[0][comp][0], comps[subset][comp][lh], num_sig_bits, is_signed);
		}

		for (uint32_t subset = 0; subset < num_subsets; subset++)
			for (uint32_t comp = 0; comp < 3; comp++)
				for (uint32_t lh = 0; lh < 2; lh++)
					comps[subset][comp][lh] = hdr_bc6h_dequantize(comps[subset][comp][lh], num_sig_bits, is_signed);

		// Unpack weights and output texels
		const int weight_bits = (num_subsets == 1) ? 4 : 3;
		const uint8_t* pWeights = (num_subsets == 1) ? g_bc6h_weight4 : g_bc6h_weight3;

		half_float* pDst = pDst_rgb_halfs;

		for (uint32_t y = 0; y < 4; y++)
		{
			for (uint32_t x = 0; x < 4; x++)
			{
				int subset = (num_subsets == 1) ? ((x | y) ? 0 : 0x80) : g_bc6h_2subset_patterns[part_index][y][x];
				const int num_bits = weight_bits + ((subset & 0x80) ? -1 : 0);

				subset &= 1;

				const int weight_index = hdr_bc6h_get_bits(num_bits, blo, bhi, total_bits_read);

				pDst[0] = hdr_bc6h_convert_to_half(hdr_bc6h_interpolate(comps[subset][0][0], comps[subset][0][1], pWeights, weight_index), is_signed);
				pDst[1] = hdr_bc6h_convert_to_half(hdr_bc6h_interpolate(comps[subset][1][0], comps[subset][1][1], pWeights, weight_index), is_signed);
				pDst[2] = hdr_bc6h_convert_to_half(hdr_bc6h_interpolate(comps[subset][2][0], comps[subset][2][1], pWeights, weight_index), is_signed);

				pDst += 3;
			}
		}

		return true;
	}

#else

	// UASTC HDR support compiled out: every entry point fails explicitly.
	hdr_transcode_error decode_uastc_hdr_4x4_block_checked(const astc_blk&, half_float*, const hdr_transcode_options&, hdr_transcode_stats*)
	{
		return hdr_transcode_error::cUnsupportedTargetFormat;
	}

	hdr_transcode_error transcode_uastc_hdr_4x4_block_to_bc6h(const astc_blk&, bc6h_block&, const hdr_transcode_options&, hdr_transcode_stats*)
	{
		return hdr_transcode_error::cUnsupportedTargetFormat;
	}

	hdr_transcode_error transcode_uastc_hdr_4x4_slice_checked(
		void*, uint32_t, uint32_t, const uint8_t*, uint32_t, transcoder_texture_format,
		uint32_t, uint32_t, uint32_t, const hdr_transcode_options*, hdr_transcode_stats*)
	{
		return hdr_transcode_error::cUnsupportedTargetFormat;
	}

	bool unpack_bc6h_checked(const void*, half_float*, bool)
	{
		return false;
	}

#endif // BASISD_SUPPORT_UASTC_HDR

} // namespace basist
