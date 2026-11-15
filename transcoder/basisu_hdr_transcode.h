// basisu_hdr_transcode.h
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
// Unified, checked HDR transcoding policy layer for UASTC HDR 4x4 source data.
//
// This module adds a single coherent strategy for the HDR transcode paths that
// were previously scattered across the low-level transcoders:
//
//   - UASTC HDR 4x4 input -> ASTC HDR 4x4 (validated passthrough), BC6H
//     (block-level error-selected), RGB_HALF/RGBA_HALF (sanitized decode).
//   - Exposure (linear scale) and error-metric colorspace (linear vs. log2
//     perceptual) are explicit options, not hidden globals.
//   - Alpha is handled uniformly: UASTC HDR 4x4 carries no alpha, so alpha is
//     forced to opaque 1.0 by default instead of leaking decoder garbage.
//   - NaN/Inf, negative values and out-of-range magnitudes are either
//     sanitized deterministically or reported as explicit errors. This module
//     NEVER silently outputs black/garbage on failure: every failure path
//     returns a distinct hdr_transcode_error.
//   - Unsupported targets (e.g. any SDR/BC1-7/PVRTC format, or RGB9E5) return
//     cUnsupportedTargetFormat.
//
// Resource bounds (hard guarantees):
//   - No heap allocation, no threads, no mutable global state beyond the
//     one-time transcoder tables initialized by basisu_transcoder_init().
//   - Per-block working storage is fixed-size and stack allocated
//     (well under 1 KiB per call: 64 RGBA halfs + 3 BC6H candidates + decode
//     scratch).
//   - Slice dimensions are sanity-bounded (HDR_TRANSCODE_MAX_BLOCKS_PER_DIM)
//     so output sizes computed here cannot overflow.
//
// The existing SDR (LDR) transcode paths and bitstreams are completely
// untouched by this module; it only adds new entry points.
#pragma once

#include "basisu_transcoder.h"
#include "basisu_astc_hdr_core.h"

namespace basist
{
	// ------------------------------------------------------------------
	// Constants
	// ------------------------------------------------------------------

	// UASTC HDR 4x4 block geometry.
	const uint32_t HDR_TRANSCODE_BLOCK_WIDTH = 4;
	const uint32_t HDR_TRANSCODE_BLOCK_HEIGHT = 4;
	const uint32_t HDR_TRANSCODE_BLOCK_TEXELS = 16;

	// Maximum exposure (linear multiplier) accepted by the checked paths.
	// Bounds the dynamic range expansion applied during transcoding.
	const float HDR_TRANSCODE_MAX_EXPOSURE = 256.0f;

	// Sanity bound on slice dimensions in blocks (64K x 64K blocks = 256K x 256K texels).
	const uint32_t HDR_TRANSCODE_MAX_BLOCKS_PER_DIM = 65536;

	// Maximum half-float value emitted by the checked paths (0x7BFF = 65504.0).
	// This is also the maximum magnitude BC6H can encode (MAX_BC6H_HALF_FLOAT_AS_UINT).
	const float HDR_TRANSCODE_MAX_HALF_VALUE = 65504.0f;

	// ------------------------------------------------------------------
	// Error codes: every failure is explicit, never a silent black image.
	// ------------------------------------------------------------------
	enum class hdr_transcode_error : uint32_t
	{
		cSuccess = 0,

		cInvalidArgument,			// null pointer, zero/invalid dimensions, invalid pitch
		cDataTooSmall,				// input buffer smaller than the required block data
		cUnsupportedTargetFormat,	// target is not ASTC HDR 4x4 / BC6H / RGB_HALF / RGBA_HALF
		cUnsupportedOperation,		// valid format, but the requested operation is not supported (e.g. exposure != 1.0 with ASTC HDR passthrough)
		cInvalidExposure,			// exposure is NaN/Inf, <= 0, or above HDR_TRANSCODE_MAX_EXPOSURE
		cNanInfInput,				// NaN/Inf texel encountered and m_fail_on_nan_inf was set
		cBlockUnpackFailed,			// source block is corrupted / not valid UASTC HDR 4x4
		cBlockTranscodeFailed,		// all BC6H encoding candidates failed for a block
		cErrorTooHigh,				// best candidate's block error exceeded m_max_block_error

		cTotal
	};

	const char* hdr_transcode_error_to_string(hdr_transcode_error err);

	// ------------------------------------------------------------------
	// Error metric colorspace used for block-level candidate selection.
	// ------------------------------------------------------------------
	enum class hdr_error_metric : uint32_t
	{
		cLinear = 0,	// RMSE in linear half-float space
		cLog2 = 1		// RMSE in log2 space (perceptual; matches the encoder's HDR log2 PSNR philosophy)
	};

	// ------------------------------------------------------------------
	// Unified HDR transcode options.
	// ------------------------------------------------------------------
	struct hdr_transcode_options
	{
		// Linear exposure multiplier applied to decoded texels before output.
		// Must be finite and in (0, HDR_TRANSCODE_MAX_EXPOSURE]. 1.0 = unchanged.
		// Compose with file-level metadata (e.g. basisu_transcoder::get_ldr_hdr_upconversion_nit_multiplier())
		// by simple multiplication if desired.
		float m_exposure_scale;

		// Colorspace of the error metric used for block-level BC6H candidate selection.
		hdr_error_metric m_error_metric;

		// If true (default), NaN/Inf texels in the source fail with cNanInfInput.
		// If false, they are sanitized deterministically (NaN->0, +Inf->max, -Inf->0).
		bool m_fail_on_nan_inf;

		// If true (default), negative texel values are clamped to 0 (all supported
		// targets are unsigned). If false, negatives are passed through to the
		// half-float outputs (BC6H candidate selection still clamps them).
		bool m_clamp_negatives;

		// If true (default), output alpha is forced to opaque 1.0 because
		// UASTC HDR 4x4 has no alpha channel.
		bool m_force_opaque_alpha;

		// If true (default), a solid-color BC6H block derived from the block
		// average is considered as a last-resort candidate, so a block that
		// cannot be endpoint-transcoded still produces valid output instead of
		// failing the whole slice.
		bool m_allow_solid_fallback;

		// If > 0, blocks whose best candidate RMSE (in m_error_metric space)
		// exceeds this threshold fail with cErrorTooHigh instead of emitting
		// low-quality output. 0 (default) disables the check.
		double m_max_block_error;

		hdr_transcode_options()
		{
			init();
		}

		void init()
		{
			m_exposure_scale = 1.0f;
			m_error_metric = hdr_error_metric::cLog2;
			m_fail_on_nan_inf = true;
			m_clamp_negatives = true;
			m_force_opaque_alpha = true;
			m_allow_solid_fallback = true;
			m_max_block_error = 0.0;
		}
	};

	// Converts exposure in photographic stops to a linear scale (2^stops).
	// Returns 0.0f for non-finite input (which will fail validation as cInvalidExposure).
	float hdr_exposure_stops_to_scale(float stops);

	// ------------------------------------------------------------------
	// Bounded statistics. Caller-provided, never allocated by this module.
	// ------------------------------------------------------------------
	struct hdr_transcode_stats
	{
		uint32_t m_num_blocks;				// blocks processed
		uint32_t m_num_nan_inf_sanitized;	// NaN/Inf texels replaced (only when !m_fail_on_nan_inf)
		uint32_t m_num_negatives_clamped;	// negative texels clamped to 0
		uint32_t m_num_values_clamped;		// texels clamped to HDR_TRANSCODE_MAX_HALF_VALUE
		uint32_t m_num_direct_transcodes;	// blocks where the direct endpoint transcode won
		uint32_t m_num_reencoded;			// blocks where texel re-encoding won
		uint32_t m_num_solid_fallbacks;		// blocks where the solid-color fallback won
		double m_total_error;				// sum of per-block best RMSE
		double m_max_error;					// max per-block best RMSE

		void clear()
		{
			m_num_blocks = 0;
			m_num_nan_inf_sanitized = 0;
			m_num_negatives_clamped = 0;
			m_num_values_clamped = 0;
			m_num_direct_transcodes = 0;
			m_num_reencoded = 0;
			m_num_solid_fallbacks = 0;
			m_total_error = 0.0;
			m_max_error = 0.0;
		}

		hdr_transcode_stats()
		{
			clear();
		}
	};

	// ------------------------------------------------------------------
	// Texel sanitization (exposure, NaN/Inf, negatives, range clamp).
	// pTexels: num_texels * num_comps half floats, modified in place.
	// Bounded: no allocation, O(num_texels * num_comps).
	// ------------------------------------------------------------------
	hdr_transcode_error sanitize_hdr_texels(
		half_float* pTexels, uint32_t num_texels, uint32_t num_comps,
		const hdr_transcode_options& options, hdr_transcode_stats* pStats);

	// ------------------------------------------------------------------
	// Decodes and validates one UASTC HDR 4x4 block to 16 RGBA half-float
	// texels (64 halfs), applying sanitization, exposure and the alpha policy.
	// pDst_rgba_halfs must point to 64 half floats.
	// ------------------------------------------------------------------
	hdr_transcode_error decode_uastc_hdr_4x4_block_checked(
		const astc_blk& src_blk, half_float* pDst_rgba_halfs,
		const hdr_transcode_options& options, hdr_transcode_stats* pStats);

	// ------------------------------------------------------------------
	// Transcodes one UASTC HDR 4x4 block to BC6H with block-level error
	// selection. Candidates: direct endpoint transcode (exposure == 1.0 only),
	// texel re-encode, and (optionally) a solid-color fallback. The candidate
	// with the lowest error vs. the sanitized source texels wins.
	// ------------------------------------------------------------------
	hdr_transcode_error transcode_uastc_hdr_4x4_block_to_bc6h(
		const astc_blk& src_blk, bc6h_block& dst_blk,
		const hdr_transcode_options& options, hdr_transcode_stats* pStats);

	// ------------------------------------------------------------------
	// Checked slice-level transcode of UASTC HDR 4x4 data.
	//
	// Supported targets (anything else -> cUnsupportedTargetFormat):
	//   cTFASTC_HDR_4x4_RGBA - validated passthrough (exposure must be 1.0)
	//   cTFBC6H              - block-level error-selected BC6H
	//   cTFRGB_HALF          - sanitized decode, 3 halfs/pixel
	//   cTFRGBA_HALF         - sanitized decode, 4 halfs/pixel (alpha per policy)
	//
	// pImage_data: num_blocks_x * num_blocks_y * 16 bytes of UASTC HDR 4x4 blocks.
	// orig_width/orig_height: source image dimensions in pixels (used to clip
	//   edge blocks for the uncompressed pixel targets; ignored for block targets).
	// output_row_pitch_in_blocks_or_pixels: row pitch of the destination in
	//   blocks (compressed targets) or pixels (uncompressed targets); 0 = tightly packed.
	//
	// On any failure the function returns an explicit error code immediately;
	// it never fabricates black output to hide a failure.
	// ------------------------------------------------------------------
	hdr_transcode_error transcode_uastc_hdr_4x4_slice_checked(
		void* pDst_blocks,
		uint32_t num_blocks_x, uint32_t num_blocks_y,
		const uint8_t* pImage_data, uint32_t image_data_size,
		transcoder_texture_format fmt,
		uint32_t orig_width, uint32_t orig_height,
		uint32_t output_row_pitch_in_blocks_or_pixels = 0,
		const hdr_transcode_options* pOptions = nullptr,
		hdr_transcode_stats* pStats = nullptr);

	// ------------------------------------------------------------------
	// Bounded BC6H decoder (all 14 modes, unsigned and signed), exposed so
	// callers/tests can evaluate transcoded BC6H output. Outputs 16 RGB
	// texels (48 half floats) in raster order. Returns false on invalid mode.
	// ------------------------------------------------------------------
	bool unpack_bc6h_checked(const void* pSrc_block, half_float* pDst_rgb_halfs, bool is_signed);

} // namespace basist
