#include "rvk2_ContextImpl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <Log.h>
#include <Graphics/Parameters.h>
#include <N64.h>

#include "rvk2_Env.h"
#include "rvk2_Runtime.h"

namespace {

bool envFlagEnabled(const char * _key, bool _defaultValue)
{
	return rvk2::envFlagEnabled(_key, _defaultValue);
}

bool shouldFlipPresentedFrameY()
{
	// Default to dumpfb-aligned orientation for emulator output.
	return envFlagEnabled("REALITYVK_RVK2_PRESENT_FLIP_Y", true);
}

bool shouldForwardShadowDrawCalls()
{
	// Debug path: forward no-op draw/state calls through base Vulkan renderer.
	return envFlagEnabled("REALITYVK_RVK2_SHADOW_DRAW", false);
}

bool shouldPresentShadowPath()
{
	// Debug path: present forwarded Vulkan output instead of executor output.
	return envFlagEnabled("REALITYVK_RVK2_SHADOW_PRESENT", false);
}

bool debugDisableVIHistoryPresentRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_DISABLE_VI_HISTORY_PRESENT", false);
}

bool debugPreferLiveSurfaceOverHistoryRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_PREFER_LIVE_SURFACE_OVER_HISTORY", true);
}

bool debugEnableSurfaceHistoryBootstrapRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_ENABLE_SURFACE_HISTORY_BOOTSTRAP", false);
}

bool debugEnableCrossSurfaceBootstrapRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_ENABLE_CROSS_SURFACE_BOOTSTRAP", false);
}

bool debugCrossSurfaceBootstrapCopyAllRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_CROSS_SURFACE_BOOTSTRAP_COPY_ALL", false);
}

bool debugDisableSameFramePresentAccumRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_DISABLE_SAME_FRAME_PRESENT_ACCUM", false);
}

bool debugDisableTMEMSnapshotsRequested()
{
	return envFlagEnabled("REALITYVK_RVK2_DEBUG_DISABLE_TMEM_SNAPSHOTS", false);
}

const char * debugExecutorPresentDumpPath()
{
	static const char * path = []() -> const char * {
		return rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_EXECUTOR_PRESENT_DUMP");
	}();
	return path;
}

u64 debugExecutorPresentDumpFrame()
{
	static const u64 frame = []() -> u64 {
		uint64_t parsed = 0ULL;
		if (!rvk2::envUnsigned("REALITYVK_RVK2_DEBUG_EXECUTOR_PRESENT_DUMP_FRAME", parsed))
			return 0ULL;
		return static_cast<u64>(parsed);
	}();
	return frame;
}

void dumpExecutorPresentFrameIfRequested(
	u64 _frameId,
	const rvk2::ExecutorOutput & _output)
{
	const char * dumpPath = debugExecutorPresentDumpPath();
	if (dumpPath == nullptr)
		return;
	const u64 frameFilter = debugExecutorPresentDumpFrame();
	if (frameFilter != 0ULL && frameFilter != _frameId)
		return;
	if (_output.presentFrame.width == 0U
		|| _output.presentFrame.height == 0U
		|| _output.presentFrame.pixels.empty())
		return;
	const size_t requiredPixelCount =
		static_cast<size_t>(_output.presentFrame.width)
		* static_cast<size_t>(_output.presentFrame.height);
	if (_output.presentFrame.pixels.size() < requiredPixelCount)
		return;

	std::FILE * file = std::fopen(dumpPath, "wb");
	if (file == nullptr)
		return;
	std::fprintf(file, "P6\n%u %u\n255\n", _output.presentFrame.width, _output.presentFrame.height);
	for (size_t i = 0; i < requiredPixelCount; ++i) {
		const u32 pixel = _output.presentFrame.pixels[i];
		const u8 rgb[3] = {
			static_cast<u8>((pixel >> 24U) & 0xFFU),
			static_cast<u8>((pixel >> 16U) & 0xFFU),
			static_cast<u8>((pixel >> 8U) & 0xFFU),
		};
		std::fwrite(rgb, 1U, 3U, file);
	}
	std::fclose(file);
}

struct ForensicsIngressSnapshot {
	u64 frameId = 0ULL;
	u64 stateCallCount = 0ULL;
	u64 triangleCallCount = 0ULL;
	u64 triangleVertexCount = 0ULL;
	bool triangleBoundsValid = false;
	f32 triangleMinX = 0.0f;
	f32 triangleMinY = 0.0f;
	f32 triangleMaxX = 0.0f;
	f32 triangleMaxY = 0.0f;
	u64 rectCallCount = 0ULL;
	u64 rectTexrectCallCount = 0ULL;
	u64 rectVertexCount = 0ULL;
	bool rectBoundsValid = false;
	f32 rectMinX = 0.0f;
	f32 rectMinY = 0.0f;
	f32 rectMaxX = 0.0f;
	f32 rectMaxY = 0.0f;
	u64 lineCallCount = 0ULL;
	u64 lineVertexCount = 0ULL;
	bool lineBoundsValid = false;
	f32 lineMinX = 0.0f;
	f32 lineMinY = 0.0f;
	f32 lineMaxX = 0.0f;
	f32 lineMaxY = 0.0f;
	bool shadowDrawForwarding = false;
	bool shadowPresent = false;
	u64 sameFramePresentPass = 0ULL;
	u64 sameFramePresentRetainedNonBlackOverBlackCount = 0ULL;
	u64 sameFramePresentPromotedBlackToNonBlackCount = 0ULL;
	u64 sameFramePresentReplacedNonBlackCount = 0ULL;
	u64 sameFramePresentOutputNonBlackCount = 0ULL;
	bool sameFramePresentMerged = false;
};

inline void expandBounds(
	bool & _valid,
	f32 & _minX,
	f32 & _minY,
	f32 & _maxX,
	f32 & _maxY,
	f32 _x,
	f32 _y)
{
	if (!_valid) {
		_valid = true;
		_minX = _x;
		_minY = _y;
		_maxX = _x;
		_maxY = _y;
		return;
	}
	_minX = std::min(_minX, _x);
	_minY = std::min(_minY, _y);
	_maxX = std::max(_maxX, _x);
	_maxY = std::max(_maxY, _y);
}

void buildFullscreenRect(RectVertex (&_vertices)[4], bool _flipY)
{
	const f32 bottomT = _flipY ? 0.0f : 1.0f;
	const f32 topT = _flipY ? 1.0f : 0.0f;
	_vertices[0] = RectVertex{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, bottomT, 0.0f, bottomT, 1.0f, 1.0f};
	_vertices[1] = RectVertex{1.0f, -1.0f, 0.0f, 1.0f, 1.0f, bottomT, 1.0f, bottomT, 1.0f, 1.0f};
	_vertices[2] = RectVertex{-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, topT, 0.0f, topT, 1.0f, 1.0f};
	_vertices[3] = RectVertex{1.0f, 1.0f, 0.0f, 1.0f, 1.0f, topT, 1.0f, topT, 1.0f, 1.0f};
}

u32 readRegValue(const u32 * _reg)
{
	return _reg != nullptr ? *_reg : 0U;
}

inline bool pixelHasVisibleColor(u32 _pixel)
{
	return ((_pixel >> 8U) & 0x00FFFFFFU) != 0U;
}

rvk2::ExecutorConfig buildExecutorConfigFromVIRegisters(u64 _frameId)
{
	rvk2::ExecutorConfig config = rvk2::loadExecutorConfigFromEnv();
	config.frameId = _frameId;
	if (REG.VI_STATUS == nullptr || REG.VI_ORIGIN == nullptr)
		return config;

	config.viRegistersValid = true;
	config.viStatus = readRegValue(REG.VI_STATUS);
	config.viOrigin = readRegValue(REG.VI_ORIGIN);
	config.viWidth = readRegValue(REG.VI_WIDTH);
	config.viVCurrentLine = readRegValue(REG.VI_V_CURRENT_LINE);
	config.viVSync = readRegValue(REG.VI_V_SYNC);
	config.viHStart = readRegValue(REG.VI_H_START);
	config.viVStart = readRegValue(REG.VI_V_START);
	config.viXScale = readRegValue(REG.VI_X_SCALE);
	config.viYScale = readRegValue(REG.VI_Y_SCALE);
	return config;
}

void writeTextureReplacementSummaryFile(
	const rvk2::ExecutorConfig & _config,
	const rvk2::ExecutorSummary & _summary)
{
	if (_config.textureReplacementSummaryPath.empty())
		return;

	std::FILE * file = std::fopen(_config.textureReplacementSummaryPath.c_str(), "wb");
	if (file == nullptr)
		return;

	const double hitRate = _summary.textureReplacementSampleCount > 0ULL
		? static_cast<double>(_summary.textureReplacementHitCount)
			/ static_cast<double>(_summary.textureReplacementSampleCount)
		: 0.0;
	std::fprintf(
		file,
		"enabled=%u\nentries=%llu\npixels=%llu\nsamples=%llu\nhits=%llu\nmisses=%llu\nhit_rate=%.6f\npresent_hash=0x%016llX\npresent_width=%u\npresent_height=%u\n",
		_summary.textureReplacementEnabled ? 1U : 0U,
		static_cast<unsigned long long>(_summary.textureReplacementEntryCount),
		static_cast<unsigned long long>(_summary.textureReplacementPixelCount),
		static_cast<unsigned long long>(_summary.textureReplacementSampleCount),
		static_cast<unsigned long long>(_summary.textureReplacementHitCount),
		static_cast<unsigned long long>(_summary.textureReplacementMissCount),
		hitRate,
		static_cast<unsigned long long>(_summary.presentHash),
		_summary.presentWidth,
		_summary.presentHeight);
	std::fclose(file);
}

void appendFrameForensicsRecord(
	const rvk2::ExecutorOutput & _output,
	const ForensicsIngressSnapshot & _ingress)
{
	const char * path = rvk2::envStringOrNull("REALITYVK_RVK2_FRAME_FORENSICS_FILE");
	if (path == nullptr)
		return;

	std::FILE * file = std::fopen(path, "a");
	if (file == nullptr)
		return;

	const rvk2::ExecutorSummary & summary = _output.summary;
	const u64 outputLumaAvgX1000 = summary.colorWriteCount != 0ULL
		? (summary.outputLumaSum * 1000ULL) / summary.colorWriteCount
		: 0ULL;
	const u64 viSourceLumaAvgX1000 = summary.viSourceSampleCount != 0ULL
		? (summary.viSourceLumaSum * 1000ULL) / summary.viSourceSampleCount
		: 0ULL;
	const u64 viOutputLumaAvgX1000 =
		(summary.presentWidth != 0U && summary.presentHeight != 0U)
		? (summary.viOutputLumaSum * 1000ULL)
			/ (static_cast<u64>(summary.presentWidth) * static_cast<u64>(summary.presentHeight))
		: 0ULL;
	std::fprintf(
		file,
		"frame=%llu\twork=%llu\tbatches=%llu\twrites=%llu\tsurfaces=%llu\tpresent_surface=0x%08X\tpresent_select=%u\tpresent_hash=0x%016llX\tpresent_w=%u\tpresent_h=%u\tvi_valid=%u\tvi_origin=0x%08X\tvi_status=0x%08X\tvi_width=%u\tvi_vcurrent=%u\tvi_vsync=%u\tvi_hstart=0x%08X\tvi_vstart=0x%08X\tvi_xscale=0x%08X\tvi_yscale=0x%08X\tvi_origin_match=%u\tvi_reject=%u\tvi_type=%u\tvi_use_regs=%u\tvi_src_w=%u\tvi_src_h=%u\tvi_out_w=%u\tvi_out_h=%u\tvi_stride=%u\tvi_hash_decode=0x%016llX\tvi_hash_filter=0x%016llX\tvi_hash_gdither=0x%016llX\tselected_surface_writes=%llu\tselected_surface_works=%llu\tselected_surface_size=%u\tselected_surface_w=%u\tselected_surface_h=%u\tselected_surface_hash=0x%016llX\ttx_samples=%llu\ttx_tmem=%llu\ttx_tmem_try=%llu\ttx_tmem_reject_fmt=%llu\ttx_tmem_reject_size=%llu\ttx_tmem_reject_coord=%llu\ttx_tmem32_cmp=%llu\ttx_tmem32_mismatch=%llu\ttx_tmem32_alt_noxor_mismatch=%llu\ttx_tmem32_alt_abs_mismatch=%llu\ttx_tmem32_alt_direct_mismatch=%llu\ttx_tmem32_alt_direct_swap_mismatch=%llu\ttx_tmem32_alt_tileline_xor_mismatch=%llu\ttx_tmem32_alt_tileline_evenodd_mismatch=%llu\ttx_tmem32_alt_direct_evenodd_mismatch=%llu\ttx_tmem32_alt_loadkind_mismatch=%llu\ttx_rdram=%llu\ttx_synth=%llu\ttx_lut=%llu\ttx_mask_allow=%llu\ttx_mask_reject=%llu\tcomb_ops=%llu\tcomb_cycle2_ops=%llu\tblend_ops=%llu\tblend_enabled_ops=%llu\tblend_force_ops=%llu\tblend_aa_ops=%llu\tblend_p_mem_ops=%llu\tblend_m_mem_ops=%llu\tblend_divide_ops=%llu\tblend_nodivide_ops=%llu\talpha_tests=%llu\talpha_rejects=%llu\tcvg_tests=%llu\tcvg_rejects=%llu\tblend_cvg_eval=%llu\tblend_cvg_zero=%llu\tblend_cvg_overflow=%llu\tcvg_write_eval=%llu\tcvg_write_zero=%llu\tcvg_write_overflow=%llu\tdepth_eval=%llu\tdepth_reject=%llu\tdepth_update=%llu\tdither_color=%llu\tdither_alpha=%llu\ttexedge_promote=%llu\tconvert_one_force=%llu\tblend_a_sel0=%llu\tblend_a_sel1=%llu\tblend_a_sel2=%llu\tblend_a_sel3=%llu\tblend_b_sel0=%llu\tblend_b_sel1=%llu\tblend_b_sel2=%llu\tblend_b_sel3=%llu\tblend_p_sel0=%llu\tblend_p_sel1=%llu\tblend_p_sel2=%llu\tblend_p_sel3=%llu\tblend_m_sel0=%llu\tblend_m_sel1=%llu\tblend_m_sel2=%llu\tblend_m_sel3=%llu\tstage_t2c_delta=%llu\tstage_c2b_delta=%llu\tstage_b2f_delta=%llu\tstage_t2f_delta=%llu\tstage_textured_writes=%llu\tstage_textured_rect=%llu\tstage_textured_tri=%llu\tstage_imread=%llu\tstage_tx_repl=%llu\tstage_tx_tmem=%llu\tstage_tx_rdram=%llu\tstage_tx_synth=%llu\twork_fill=%llu\twork_texrect=%llu\twork_tri=%llu\twork_textured=%llu\twrite_fill=%llu\twrite_texrect=%llu\twrite_tri=%llu\tout_luma_sum=%llu\tout_luma_avg_x1000=%llu\tvi_src_samples=%llu\tvi_src_invalid=%llu\tvi_src_luma_avg_x1000=%llu\tvi_out_luma_avg_x1000=%llu\tvi_out_nonblack=%llu",
		static_cast<unsigned long long>(rvk2::runtime().commandStream().frameId()),
		static_cast<unsigned long long>(summary.executedWorkCount),
		static_cast<unsigned long long>(summary.executedBatchCount),
		static_cast<unsigned long long>(summary.colorWriteCount),
		static_cast<unsigned long long>(summary.surfaceCount),
		summary.selectedPresentSurfaceAddress,
		static_cast<u32>(summary.presentSelectionReason),
		static_cast<unsigned long long>(summary.presentHash),
		summary.presentWidth,
			summary.presentHeight,
			static_cast<u32>(summary.viRegistersValid),
			summary.viOriginAddress,
			summary.viStatus,
			summary.viWidth,
			summary.viVCurrentLine,
			summary.viVSync,
			summary.viHStart,
			summary.viVStart,
			summary.viXScale,
			summary.viYScale,
			static_cast<u32>(summary.viOriginMatchedSurface),
			static_cast<u32>(summary.viRejectReason),
			static_cast<u32>(summary.viResolvedType),
		static_cast<u32>(summary.viResolvedUsesRegisters),
		summary.viResolvedSourceWidth,
		summary.viResolvedSourceHeight,
		summary.viResolvedOutputWidth,
		summary.viResolvedOutputHeight,
		summary.viResolvedLineStride,
		static_cast<unsigned long long>(summary.viHashDecode),
		static_cast<unsigned long long>(summary.viHashFilter),
		static_cast<unsigned long long>(summary.viHashGammaDither),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceWriteCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceWorkCount),
		static_cast<u32>(summary.selectedPresentSurfaceSize),
		summary.selectedPresentSurfaceWidth,
		summary.selectedPresentSurfaceHeight,
		static_cast<unsigned long long>(summary.selectedPresentSurfaceHash),
		static_cast<unsigned long long>(summary.textureSampleCount),
		static_cast<unsigned long long>(summary.textureTmemSampleCount),
		static_cast<unsigned long long>(summary.textureTmemAttemptCount),
		static_cast<unsigned long long>(summary.textureTmemRejectFormatCount),
		static_cast<unsigned long long>(summary.textureTmemRejectSizeCount),
		static_cast<unsigned long long>(summary.textureTmemRejectCoordCount),
		static_cast<unsigned long long>(summary.textureTmem32CompareCount),
		static_cast<unsigned long long>(summary.textureTmem32CompareMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltNoXorMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltAbsCoordMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltDirectMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltDirectSwappedMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltTileLineXorMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltTileLineEvenOddMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltDirectEvenOddMismatchCount),
		static_cast<unsigned long long>(summary.textureTmem32AltLoadKindAwareMismatchCount),
		static_cast<unsigned long long>(summary.textureRdramSampleCount),
		static_cast<unsigned long long>(summary.textureSyntheticSampleCount),
		static_cast<unsigned long long>(summary.textureLUTSampleCount),
		static_cast<unsigned long long>(summary.textureBucketMaskAllowCount),
		static_cast<unsigned long long>(summary.textureBucketMaskRejectCount),
		static_cast<unsigned long long>(summary.combinerOpCount),
		static_cast<unsigned long long>(summary.combinerCycle2SelectorOpCount),
		static_cast<unsigned long long>(summary.blenderOpCount),
		static_cast<unsigned long long>(summary.blenderEnabledOpCount),
		static_cast<unsigned long long>(summary.blenderForceOpCount),
		static_cast<unsigned long long>(summary.blenderAAOpCount),
		static_cast<unsigned long long>(summary.blenderColorPMemorySelectorCount),
		static_cast<unsigned long long>(summary.blenderColorMMemorySelectorCount),
		static_cast<unsigned long long>(summary.blenderDivideOpCount),
		static_cast<unsigned long long>(summary.blenderNoDivideOpCount),
		static_cast<unsigned long long>(summary.alphaCompareTestCount),
		static_cast<unsigned long long>(summary.alphaCompareRejectCount),
		static_cast<unsigned long long>(summary.coverageWriteTestCount),
		static_cast<unsigned long long>(summary.coverageWriteRejectCount),
		static_cast<unsigned long long>(summary.blendCoverageEvalCount),
		static_cast<unsigned long long>(summary.blendCoverageZeroCount),
		static_cast<unsigned long long>(summary.blendCoverageOverflowCount),
		static_cast<unsigned long long>(summary.coverageWriteEvalCount),
		static_cast<unsigned long long>(summary.coverageWriteZeroCount),
		static_cast<unsigned long long>(summary.coverageWriteOverflowCount),
		static_cast<unsigned long long>(summary.depthEvalCount),
		static_cast<unsigned long long>(summary.depthRejectCount),
		static_cast<unsigned long long>(summary.depthUpdateCount),
		static_cast<unsigned long long>(summary.colorDitherApplyCount),
		static_cast<unsigned long long>(summary.alphaDitherApplyCount),
		static_cast<unsigned long long>(summary.textureEdgeAlphaPromoteCount),
		static_cast<unsigned long long>(summary.convertOneAlphaForceCount),
		static_cast<unsigned long long>(summary.blendAlphaASelectorCount[0]),
		static_cast<unsigned long long>(summary.blendAlphaASelectorCount[1]),
		static_cast<unsigned long long>(summary.blendAlphaASelectorCount[2]),
		static_cast<unsigned long long>(summary.blendAlphaASelectorCount[3]),
		static_cast<unsigned long long>(summary.blendAlphaBSelectorCount[0]),
		static_cast<unsigned long long>(summary.blendAlphaBSelectorCount[1]),
		static_cast<unsigned long long>(summary.blendAlphaBSelectorCount[2]),
		static_cast<unsigned long long>(summary.blendAlphaBSelectorCount[3]),
		static_cast<unsigned long long>(summary.blendColorPSelectorCount[0]),
		static_cast<unsigned long long>(summary.blendColorPSelectorCount[1]),
		static_cast<unsigned long long>(summary.blendColorPSelectorCount[2]),
		static_cast<unsigned long long>(summary.blendColorPSelectorCount[3]),
		static_cast<unsigned long long>(summary.blendColorMSelectorCount[0]),
		static_cast<unsigned long long>(summary.blendColorMSelectorCount[1]),
		static_cast<unsigned long long>(summary.blendColorMSelectorCount[2]),
		static_cast<unsigned long long>(summary.blendColorMSelectorCount[3]),
		static_cast<unsigned long long>(summary.stageTexelToCombinerDeltaCount),
		static_cast<unsigned long long>(summary.stageCombinerToBlenderDeltaCount),
		static_cast<unsigned long long>(summary.stageBlenderToFinalDeltaCount),
		static_cast<unsigned long long>(summary.stageTexelToFinalDeltaCount),
		static_cast<unsigned long long>(summary.stageTexturedWriteCount),
		static_cast<unsigned long long>(summary.stageTexturedRectWriteCount),
		static_cast<unsigned long long>(summary.stageTexturedTriangleWriteCount),
		static_cast<unsigned long long>(summary.stageImageReadWriteCount),
		static_cast<unsigned long long>(summary.stageTexelSourceReplacementWriteCount),
		static_cast<unsigned long long>(summary.stageTexelSourceTMEMWriteCount),
		static_cast<unsigned long long>(summary.stageTexelSourceRdramWriteCount),
		static_cast<unsigned long long>(summary.stageTexelSourceSyntheticWriteCount),
		static_cast<unsigned long long>(summary.workKindFillCount),
		static_cast<unsigned long long>(summary.workKindTexRectCount),
		static_cast<unsigned long long>(summary.workKindTriangleCount),
		static_cast<unsigned long long>(summary.workTexturedCount),
		static_cast<unsigned long long>(summary.writeKindFillCount),
		static_cast<unsigned long long>(summary.writeKindTexRectCount),
		static_cast<unsigned long long>(summary.writeKindTriangleCount),
		static_cast<unsigned long long>(summary.outputLumaSum),
		static_cast<unsigned long long>(outputLumaAvgX1000),
		static_cast<unsigned long long>(summary.viSourceSampleCount),
		static_cast<unsigned long long>(summary.viSourceInvalidSampleCount),
		static_cast<unsigned long long>(viSourceLumaAvgX1000),
		static_cast<unsigned long long>(viOutputLumaAvgX1000),
		static_cast<unsigned long long>(summary.viOutputNonBlackCount));
	std::fprintf(
		file,
		"\tselected_surface_live_writes=%llu\tselected_surface_live_works=%llu\tselected_surface_from_history=%u\tselected_surface_history_age=%llu\thistory_surface_count=%u\thistory_vi_candidate_found=%u\thistory_vi_candidate_addr=0x%08X\thistory_vi_candidate_exact=%u\thistory_vi_candidate_age=%llu\thistory_vi_candidate_reject_age=%u\tselected_surface_overwrite_black=%llu\tselected_surface_overwrite_black_texrect=%llu\tselected_surface_overwrite_black_triangle=%llu\tselected_surface_quantized_black=%llu\tselected_surface_quantized_black_texrect=%llu\tselected_surface_quantized_black_triangle=%llu\tselected_surface_triangle_preserve_non_black=%llu\tselected_surface_texrect_nonblack=%llu\tselected_surface_triangle_nonblack=%llu\tselected_surface_untouched_carry=%llu\tselected_surface_history_merge_candidates=%llu\tselected_surface_history_merge_potential_black_fill=%llu\tselected_surface_history_merge_potential_nonblack_diff=%llu\tselected_surface_history_merge_copied=%llu\tselected_surface_vi_history_carry_applied=%u\tselected_surface_vi_history_carry_src=0x%08X\tselected_surface_vi_history_carry_potential_black_fill=%llu\tselected_surface_vi_history_carry_potential_nonblack_diff=%llu\tselected_surface_vi_history_carry_potential_unwritten_diff=%llu\tselected_surface_vi_history_carry_copied=%llu\tselected_surface_vi_history_carry_copied_unwritten=%llu\tselected_surface_self_history_carry_applied=%u\tselected_surface_self_history_carry_src=0x%08X\tselected_surface_self_history_carry_potential_black_fill=%llu\tselected_surface_self_history_carry_potential_nonblack_diff=%llu\tselected_surface_self_history_carry_potential_unwritten_diff=%llu\tselected_surface_self_history_carry_copied=%llu\tselected_surface_self_history_carry_copied_unwritten=%llu\tselected_surface_untouched_carry_src=0x%08X\tdbg_disable_vi_history_present=%u\tdbg_prefer_live_surface_over_history=%u\tdbg_enable_surface_history_bootstrap=%u\tdbg_enable_cross_surface_bootstrap=%u\tdbg_cross_surface_bootstrap_copy_all=%u\tdbg_disable_same_frame_present_accum=%u\tboot_same_attempt=%llu\tboot_same_success=%llu\tboot_same_pixels=%llu\tboot_fallback_success=%llu\tboot_fallback_pixels=%llu\tboot_cross_candidates=%llu\tboot_cross_pixels=%llu\tboot_cross_copy_all_pixels=%llu\toverwrite_black_total=%llu\toverwrite_black_texrect=%llu\toverwrite_black_triangle=%llu\toverwrite_black_triangle_preserved=%llu\tquantized_black_total=%llu\tquantized_black_texrect=%llu\tquantized_black_triangle=%llu\tblend_eq_bypass_ops=%llu\tblend_mem_alpha_shift_ops=%llu",
		static_cast<unsigned long long>(summary.selectedPresentSurfaceLiveWriteCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceLiveWorkCount),
		static_cast<u32>(summary.selectedPresentSurfaceFromHistory),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceHistoryAge),
		static_cast<u32>(summary.historySurfaceCount),
		static_cast<u32>(summary.historyVIOriginCandidateFound),
		summary.historyVIOriginCandidateAddress,
		static_cast<u32>(summary.historyVIOriginCandidateExact),
		static_cast<unsigned long long>(summary.historyVIOriginCandidateAge),
		static_cast<u32>(summary.historyVIOriginCandidateRejectedByAge),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceOverwriteBlackCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceOverwriteBlackTexRectCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceOverwriteBlackTriangleCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceQuantizedToBlackCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceQuantizedToBlackTexRectCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceQuantizedToBlackTriangleCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceTrianglePreserveNonBlackCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceTexRectNonBlackWriteCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceTriangleNonBlackWriteCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceUntouchedCarryCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceHistoryMergeCandidateCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceHistoryMergePotentialBlackFillCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceHistoryMergePotentialNonBlackDiffCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceHistoryMergeCopiedCount),
		static_cast<u32>(summary.selectedPresentSurfaceVIHistoryCarryApplied),
		summary.selectedPresentSurfaceVIHistoryCarrySourceAddress,
		static_cast<unsigned long long>(summary.selectedPresentSurfaceVIHistoryCarryPotentialBlackFillCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceVIHistoryCarryPotentialNonBlackDiffCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceVIHistoryCarryPotentialUnwrittenDiffCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceVIHistoryCarryCopiedCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceVIHistoryCarryCopiedUnwrittenCount),
		static_cast<u32>(summary.selectedPresentSurfaceSelfHistoryCarryApplied),
		summary.selectedPresentSurfaceSelfHistoryCarrySourceAddress,
		static_cast<unsigned long long>(summary.selectedPresentSurfaceSelfHistoryCarryPotentialBlackFillCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceSelfHistoryCarryPotentialNonBlackDiffCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceSelfHistoryCarryPotentialUnwrittenDiffCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceSelfHistoryCarryCopiedCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceSelfHistoryCarryCopiedUnwrittenCount),
		summary.selectedPresentSurfaceUntouchedCarrySourceAddress,
		static_cast<u32>(debugDisableVIHistoryPresentRequested()),
		static_cast<u32>(debugPreferLiveSurfaceOverHistoryRequested()),
		static_cast<u32>(debugEnableSurfaceHistoryBootstrapRequested()),
		static_cast<u32>(debugEnableCrossSurfaceBootstrapRequested()),
		static_cast<u32>(debugCrossSurfaceBootstrapCopyAllRequested()),
		static_cast<u32>(debugDisableSameFramePresentAccumRequested()),
		static_cast<unsigned long long>(summary.surfaceBootstrapSameAddressAttemptCount),
		static_cast<unsigned long long>(summary.surfaceBootstrapSameAddressSuccessCount),
		static_cast<unsigned long long>(summary.surfaceBootstrapSameAddressCopiedPixels),
		static_cast<unsigned long long>(summary.surfaceBootstrapFallbackRestoreSuccessCount),
		static_cast<unsigned long long>(summary.surfaceBootstrapFallbackRestoreCopiedPixels),
		static_cast<unsigned long long>(summary.surfaceBootstrapCrossMergeCandidateCount),
		static_cast<unsigned long long>(summary.surfaceBootstrapCrossMergeCopiedPixels),
		static_cast<unsigned long long>(summary.surfaceBootstrapCrossMergeCopyAllPixels),
		static_cast<unsigned long long>(summary.writeOverwriteToBlackCount),
		static_cast<unsigned long long>(summary.writeTexRectOverwriteToBlackCount),
		static_cast<unsigned long long>(summary.writeTriangleOverwriteToBlackCount),
		static_cast<unsigned long long>(summary.writeTrianglePreserveNonBlackCount),
		static_cast<unsigned long long>(summary.writeQuantizedToBlackCount),
		static_cast<unsigned long long>(summary.writeTexRectQuantizedToBlackCount),
		static_cast<unsigned long long>(summary.writeTriangleQuantizedToBlackCount),
		static_cast<unsigned long long>(summary.blenderEquationBypassCount),
		static_cast<unsigned long long>(summary.blenderMemoryAlphaShiftApplyCount));
	for (u32 i = 0U; i < summary.textureFilterModeSampleCount.size(); ++i) {
		std::fprintf(
			file,
			"\ttx_filter_mode%u=%llu",
			i,
			static_cast<unsigned long long>(summary.textureFilterModeSampleCount[i]));
	}
	for (u32 i = 0U; i < summary.textureLUTModeSampleCount.size(); ++i) {
		std::fprintf(
			file,
			"\ttx_lut_mode%u=%llu",
			i,
			static_cast<unsigned long long>(summary.textureLUTModeSampleCount[i]));
	}
	for (u32 i = 0U; i < summary.textureFormatSampleCount.size(); ++i) {
		std::fprintf(
			file,
			"\ttx_fmt%u_samples=%llu",
			i,
			static_cast<unsigned long long>(summary.textureFormatSampleCount[i]));
	}
	for (u32 i = 0U; i < summary.textureSizeSampleCount.size(); ++i) {
		std::fprintf(
			file,
			"\ttx_size%u_samples=%llu",
			i,
			static_cast<unsigned long long>(summary.textureSizeSampleCount[i]));
	}
	for (u32 fmt = 0U; fmt < rvk2::kExecutorTextureFormatBuckets; ++fmt) {
		for (u32 size = 0U; size < rvk2::kExecutorTextureSizeBuckets; ++size) {
			const u32 index = fmt * rvk2::kExecutorTextureSizeBuckets + size;
			std::fprintf(
				file,
				"\ttx_fs_f%u_s%u=%llu\ttx_fs_lut_f%u_s%u=%llu\ttx_fs_t0_f%u_s%u=%llu\ttx_fs_t1_f%u_s%u=%llu\ttx_fs_t0n_f%u_s%u=%llu\ttx_fs_lut_t0_f%u_s%u=%llu\ttx_fs_lut_t1_f%u_s%u=%llu\ttx_fs_lut_t0n_f%u_s%u=%llu",
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeSampleCount[index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeLUTSampleCount[index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeSampleCountBySlot[rvk2::kExecutorTextureSampleSlotTexel0][index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeSampleCountBySlot[rvk2::kExecutorTextureSampleSlotTexel1][index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeSampleCountBySlot[rvk2::kExecutorTextureSampleSlotTexel0Next][index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeLUTSampleCountBySlot[rvk2::kExecutorTextureSampleSlotTexel0][index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeLUTSampleCountBySlot[rvk2::kExecutorTextureSampleSlotTexel1][index]),
				fmt,
				size,
				static_cast<unsigned long long>(summary.textureFormatSizeLUTSampleCountBySlot[rvk2::kExecutorTextureSampleSlotTexel0Next][index]));
		}
	}
	for (u32 i = 0U; i < rvk2::kExecutorStageDeltaClassBuckets; ++i) {
		std::fprintf(
			file,
			"\tstage_cls%u_writes=%llu\tstage_cls%u_t2f=%llu\tstage_cls%u_c2b=%llu\tstage_cls%u_bpmem=%llu\tstage_cls%u_bmmem=%llu\tstage_cls%u_imrd=%llu\tstage_cls%u_txrepl=%llu\tstage_cls%u_txtmem=%llu\tstage_cls%u_txrdram=%llu\tstage_cls%u_txsynth=%llu",
			i,
			static_cast<unsigned long long>(summary.stageWriteClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageTexelToFinalDeltaClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageCombinerToBlenderDeltaClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageBlendPUsesMemoryClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageBlendMUsesMemoryClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageImageReadClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageTexelSourceReplacementClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageTexelSourceTMEMClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageTexelSourceRdramClassCount[i]),
			i,
			static_cast<unsigned long long>(summary.stageTexelSourceSyntheticClassCount[i]));
	}
	for (u32 i = 0U; i < summary.debugSurfaceSlotCount; ++i) {
		std::fprintf(
			file,
			"\ts%u_addr=0x%08X\ts%u_writes=%llu\ts%u_works=%llu\ts%u_hash=0x%016llX",
			i,
			summary.debugSurfaceAddress[i],
			i,
			static_cast<unsigned long long>(summary.debugSurfaceWriteCount[i]),
			i,
			static_cast<unsigned long long>(summary.debugSurfaceWorkCount[i]),
			i,
			static_cast<unsigned long long>(summary.debugSurfaceHash[i]));
	}
	std::fprintf(
		file,
		"\tci_switches=%llu\tci_first=0x%08X\tci_last=0x%08X\ttri_deg_reject=%llu\ttri_bounds_reject=%llu\ttri_scissor_reject=%llu\ttri_samples=%llu\ttri_alpha_reject=%llu\ttri_cvg_reject=%llu\ttri_depth_reject=%llu\ttri_nonblack=%llu\ttexrect_nonblack=%llu\ttri_luma_sum=%llu\ttexrect_luma_sum=%llu",
		static_cast<unsigned long long>(summary.colorImageSwitchCount),
		summary.colorImageFirstAddress,
		summary.colorImageLastAddress,
		static_cast<unsigned long long>(summary.triangleDegenerateRejectCount),
		static_cast<unsigned long long>(summary.triangleBoundsRejectCount),
		static_cast<unsigned long long>(summary.triangleScissorFieldRejectCount),
		static_cast<unsigned long long>(summary.triangleSampleCandidateCount),
		static_cast<unsigned long long>(summary.triangleAlphaRejectCount),
		static_cast<unsigned long long>(summary.triangleCoverageRejectCount),
		static_cast<unsigned long long>(summary.triangleDepthRejectCount),
		static_cast<unsigned long long>(summary.writeTriangleNonBlackCount),
		static_cast<unsigned long long>(summary.writeTexRectNonBlackCount),
		static_cast<unsigned long long>(summary.writeTriangleLumaSum),
		static_cast<unsigned long long>(summary.writeTexRectLumaSum));
	for (u32 i = 0U; i < summary.colorImageEventCount; ++i) {
		std::fprintf(
			file,
			"\tci_evt%u_addr=0x%08X\tci_evt%u_work=%llu",
			i,
			summary.colorImageEventAddress[i],
			i,
			static_cast<unsigned long long>(summary.colorImageEventWorkOrdinal[i]));
	}
	const long long triWorkGap =
		static_cast<long long>(summary.workKindTriangleCount)
		- static_cast<long long>(_ingress.triangleCallCount);
	const long long texrectWorkGap =
		static_cast<long long>(summary.workKindTexRectCount)
		- static_cast<long long>(_ingress.rectCallCount);
	std::fprintf(
		file,
		"\ting_frame=%llu\ting_state_calls=%llu\ting_tri_calls=%llu\ting_tri_verts=%llu\ting_rect_calls=%llu\ting_rect_texrect_calls=%llu\ting_rect_verts=%llu\ting_line_calls=%llu\ting_line_verts=%llu\ting_shadow_draw=%u\ting_shadow_present=%u\ting_same_frame_pass=%llu\ting_same_frame_merged=%u\ting_same_frame_keep_nonblack=%llu\ting_same_frame_promote_nonblack=%llu\ting_same_frame_replace_nonblack=%llu\ting_same_frame_out_nonblack=%llu\ting_gap_tri_work=%lld\ting_gap_texrect_work=%lld\ting_tri_bounds_valid=%u\ting_tri_min_x=%.3f\ting_tri_min_y=%.3f\ting_tri_max_x=%.3f\ting_tri_max_y=%.3f\ting_rect_bounds_valid=%u\ting_rect_min_x=%.3f\ting_rect_min_y=%.3f\ting_rect_max_x=%.3f\ting_rect_max_y=%.3f\ting_line_bounds_valid=%u\ting_line_min_x=%.3f\ting_line_min_y=%.3f\ting_line_max_x=%.3f\ting_line_max_y=%.3f",
		static_cast<unsigned long long>(_ingress.frameId),
		static_cast<unsigned long long>(_ingress.stateCallCount),
		static_cast<unsigned long long>(_ingress.triangleCallCount),
		static_cast<unsigned long long>(_ingress.triangleVertexCount),
		static_cast<unsigned long long>(_ingress.rectCallCount),
		static_cast<unsigned long long>(_ingress.rectTexrectCallCount),
		static_cast<unsigned long long>(_ingress.rectVertexCount),
		static_cast<unsigned long long>(_ingress.lineCallCount),
		static_cast<unsigned long long>(_ingress.lineVertexCount),
		static_cast<unsigned int>(_ingress.shadowDrawForwarding),
		static_cast<unsigned int>(_ingress.shadowPresent),
		static_cast<unsigned long long>(_ingress.sameFramePresentPass),
		static_cast<unsigned int>(_ingress.sameFramePresentMerged),
		static_cast<unsigned long long>(_ingress.sameFramePresentRetainedNonBlackOverBlackCount),
		static_cast<unsigned long long>(_ingress.sameFramePresentPromotedBlackToNonBlackCount),
		static_cast<unsigned long long>(_ingress.sameFramePresentReplacedNonBlackCount),
		static_cast<unsigned long long>(_ingress.sameFramePresentOutputNonBlackCount),
		triWorkGap,
		texrectWorkGap,
		static_cast<unsigned int>(_ingress.triangleBoundsValid),
		static_cast<double>(_ingress.triangleMinX),
		static_cast<double>(_ingress.triangleMinY),
		static_cast<double>(_ingress.triangleMaxX),
		static_cast<double>(_ingress.triangleMaxY),
		static_cast<unsigned int>(_ingress.rectBoundsValid),
		static_cast<double>(_ingress.rectMinX),
		static_cast<double>(_ingress.rectMinY),
		static_cast<double>(_ingress.rectMaxX),
		static_cast<double>(_ingress.rectMaxY),
		static_cast<unsigned int>(_ingress.lineBoundsValid),
		static_cast<double>(_ingress.lineMinX),
		static_cast<double>(_ingress.lineMinY),
		static_cast<double>(_ingress.lineMaxX),
		static_cast<double>(_ingress.lineMaxY));
	std::fprintf(file, "\n");
	std::fclose(file);
}

} // namespace

namespace rvk2 {

ContextImpl::ContextImpl()
	: vulkan::ContextImpl()
	, m_windowInfo()
	, m_presentTexture(graphics::ObjectHandle::null)
	, m_presentTextureWidth(0U)
	, m_presentTextureHeight(0U)
	, m_presentUploadBytes()
	, m_executor(loadExecutorConfigFromEnv())
{
}

ContextImpl::~ContextImpl() = default;

void ContextImpl::resetIngressCounters(u64 _frameId)
{
	m_ingressCounters = FrontendIngressCounters{};
	m_ingressCounters.frameId = _frameId;
}

void ContextImpl::syncIngressFrame()
{
	const u64 frameId = rvk2::runtime().commandStream().frameId();
	if (frameId != m_ingressCounters.frameId)
		resetIngressCounters(frameId);
}

bool ContextImpl::shadowDrawForwardingEnabled() const
{
	return shouldForwardShadowDrawCalls();
}

bool ContextImpl::shadowPresentEnabled() const
{
	return shouldPresentShadowPath();
}

void ContextImpl::setPresentationWindowInfo(const graphics::Context::PresentationWindowInfo & _info)
{
	m_windowInfo = _info;
	vulkan::ContextImpl::setPresentationWindowInfo(_info);
}

void ContextImpl::init()
{
	vulkan::ContextImpl::init();
}

void ContextImpl::destroy()
{
	if (m_presentTexture.isNotNull()) {
		vulkan::ContextImpl::deleteTexture(m_presentTexture);
		m_presentTexture = graphics::ObjectHandle::null;
	}
	m_presentTextureWidth = 0U;
	m_presentTextureHeight = 0U;
	m_presentUploadBytes.clear();
	m_sameFramePresent = SameFramePresentAccumulator{};
	vulkan::ContextImpl::destroy();
}

void ContextImpl::enable(graphics::EnableParam _parameter, bool _enable)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::enable(_parameter, _enable);
}

u32 ContextImpl::isEnabled(graphics::EnableParam _parameter)
{
	if (shadowDrawForwardingEnabled())
		return vulkan::ContextImpl::isEnabled(_parameter);
	(void)_parameter;
	return 0U;
}

void ContextImpl::cullFace(graphics::CullModeParam _mode)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::cullFace(_mode);
}

void ContextImpl::enableDepthWrite(bool _enable)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::enableDepthWrite(_enable);
}

void ContextImpl::setDepthCompare(graphics::CompareParam _mode)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::setDepthCompare(_mode);
}

void ContextImpl::setViewport(s32 _x, s32 _y, s32 _width, s32 _height)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::setViewport(_x, _y, _width, _height);
}

void ContextImpl::setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::setScissor(_x, _y, _width, _height);
}

void ContextImpl::setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::setBlending(_sfactor, _dfactor);
}

void ContextImpl::setBlendingSeparate(
	graphics::BlendParam _sfactorcolor,
	graphics::BlendParam _dfactorcolor,
	graphics::BlendParam _sfactoralpha,
	graphics::BlendParam _dfactoralpha)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled()) {
		vulkan::ContextImpl::setBlendingSeparate(
			_sfactorcolor,
			_dfactorcolor,
			_sfactoralpha,
			_dfactoralpha);
	}
}

void ContextImpl::setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::setBlendColor(_red, _green, _blue, _alpha);
}

void ContextImpl::setPolygonOffset(f32 _factor, f32 _units)
{
	syncIngressFrame();
	++m_ingressCounters.stateCallCount;
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::setPolygonOffset(_factor, _units);
}

void ContextImpl::drawTriangles(const graphics::Context::DrawTriangleParameters & _params)
{
	syncIngressFrame();
	++m_ingressCounters.triangleCallCount;
	m_ingressCounters.triangleVertexCount += static_cast<u64>(_params.verticesCount);
	if (_params.vertices != nullptr) {
		for (u32 i = 0U; i < _params.verticesCount; ++i) {
			const SPVertex & vertex = _params.vertices[i];
			expandBounds(
				m_ingressCounters.triangleBoundsValid,
				m_ingressCounters.triangleMinX,
				m_ingressCounters.triangleMinY,
				m_ingressCounters.triangleMaxX,
				m_ingressCounters.triangleMaxY,
				vertex.x,
				vertex.y);
		}
	}
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::drawTriangles(_params);
}

void ContextImpl::drawRects(const graphics::Context::DrawRectParameters & _params)
{
	syncIngressFrame();
	++m_ingressCounters.rectCallCount;
	if (_params.texrect)
		++m_ingressCounters.rectTexrectCallCount;
	m_ingressCounters.rectVertexCount += static_cast<u64>(_params.verticesCount);
	if (_params.vertices != nullptr) {
		for (u32 i = 0U; i < _params.verticesCount; ++i) {
			const RectVertex & vertex = _params.vertices[i];
			expandBounds(
				m_ingressCounters.rectBoundsValid,
				m_ingressCounters.rectMinX,
				m_ingressCounters.rectMinY,
				m_ingressCounters.rectMaxX,
				m_ingressCounters.rectMaxY,
				vertex.x,
				vertex.y);
		}
	}
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::drawRects(_params);
}

void ContextImpl::drawLine(f32 _width, SPVertex * _vertices)
{
	syncIngressFrame();
	++m_ingressCounters.lineCallCount;
	m_ingressCounters.lineVertexCount += 2ULL;
	if (_vertices != nullptr) {
		expandBounds(
			m_ingressCounters.lineBoundsValid,
			m_ingressCounters.lineMinX,
			m_ingressCounters.lineMinY,
			m_ingressCounters.lineMaxX,
			m_ingressCounters.lineMaxY,
			_vertices[0].x,
			_vertices[0].y);
		expandBounds(
			m_ingressCounters.lineBoundsValid,
			m_ingressCounters.lineMinX,
			m_ingressCounters.lineMinY,
			m_ingressCounters.lineMaxX,
			m_ingressCounters.lineMaxY,
			_vertices[1].x,
			_vertices[1].y);
	}
	if (shadowDrawForwardingEnabled())
		vulkan::ContextImpl::drawLine(_width, _vertices);
}

void ContextImpl::convertToRgbaBytes(const std::vector<u32> & _srcPixels, std::vector<u8> & _dstBytes)
{
	_dstBytes.resize(_srcPixels.size() * 4U);
	for (size_t i = 0; i < _srcPixels.size(); ++i) {
		const u32 pixel = _srcPixels[i];
		const size_t base = i * 4U;
		_dstBytes[base + 0U] = static_cast<u8>((pixel >> 24U) & 0xFFU);
		_dstBytes[base + 1U] = static_cast<u8>((pixel >> 16U) & 0xFFU);
		_dstBytes[base + 2U] = static_cast<u8>((pixel >> 8U) & 0xFFU);
		_dstBytes[base + 3U] = static_cast<u8>((pixel >> 0U) & 0xFFU);
	}
}

void ContextImpl::ensurePresenterTexture(u32 _width, u32 _height)
{
	if (!m_presentTexture.isNotNull()) {
		m_presentTexture = vulkan::ContextImpl::createTexture(graphics::textureTarget::TEXTURE_2D);
		graphics::Context::TexParameters texParams{};
		texParams.handle = m_presentTexture;
		texParams.textureUnitIndex = graphics::textureIndices::Tex[0];
		texParams.target = graphics::textureTarget::TEXTURE_2D;
		texParams.magFilter = graphics::textureParameters::FILTER_NEAREST;
		texParams.minFilter = graphics::textureParameters::FILTER_NEAREST;
		texParams.wrapS = graphics::textureParameters::WRAP_CLAMP_TO_EDGE;
		texParams.wrapT = graphics::textureParameters::WRAP_CLAMP_TO_EDGE;
		texParams.maxMipmapLevel = graphics::Parameter(0U);
		texParams.maxAnisotropy = graphics::Parameter(0U);
		vulkan::ContextImpl::setTextureParameters(texParams);
	}

	if (_width == m_presentTextureWidth && _height == m_presentTextureHeight)
		return;

	graphics::Context::InitTextureParams initParams{};
	initParams.handle = m_presentTexture;
	initParams.textureUnitIndex = graphics::textureIndices::Tex[0];
	initParams.target = graphics::textureTarget::TEXTURE_2D;
	initParams.width = _width;
	initParams.height = _height;
	initParams.mipMapLevel = 0U;
	initParams.mipMapLevels = 1U;
	initParams.format = graphics::colorFormat::RGBA;
	initParams.internalFormat =
		vulkan::ContextImpl::convertInternalTextureFormat(static_cast<u32>(graphics::internalcolorFormat::RGBA8));
	initParams.dataType = graphics::datatype::UNSIGNED_BYTE;
	initParams.data = nullptr;
	vulkan::ContextImpl::init2DTexture(initParams);
	m_presentTextureWidth = _width;
	m_presentTextureHeight = _height;
}

void ContextImpl::uploadPresenterTexture(const ExecutorPresentFrame & _frame)
{
	if (_frame.width == 0U || _frame.height == 0U || _frame.pixels.empty())
		return;
	ensurePresenterTexture(_frame.width, _frame.height);
	convertToRgbaBytes(_frame.pixels, m_presentUploadBytes);

	graphics::Context::UpdateTextureDataParams updateParams{};
	updateParams.handle = m_presentTexture;
	updateParams.textureUnitIndex = graphics::textureIndices::Tex[0];
	updateParams.x = 0U;
	updateParams.y = 0U;
	updateParams.width = _frame.width;
	updateParams.height = _frame.height;
	updateParams.mipMapLevel = 0U;
	updateParams.format = graphics::colorFormat::RGBA;
	updateParams.internalFormat =
		vulkan::ContextImpl::convertInternalTextureFormat(static_cast<u32>(graphics::internalcolorFormat::RGBA8));
	updateParams.dataType = graphics::datatype::UNSIGNED_BYTE;
	updateParams.data = m_presentUploadBytes.data();
	vulkan::ContextImpl::update2DTexture(updateParams);
}

void ContextImpl::renderPresentedFrame(const ExecutorOutput & _output)
{
	if (_output.presentFrame.width == 0U
		|| _output.presentFrame.height == 0U
		|| _output.presentFrame.pixels.empty()) {
		const u32 viewportWidth = std::max<u32>(1U, m_windowInfo.width > 0U ? m_windowInfo.width : 1U);
		const u32 viewportHeight = std::max<u32>(1U, m_windowInfo.height > 0U ? m_windowInfo.height : 1U);
		vulkan::ContextImpl::bindFramebuffer(graphics::bufferTarget::DRAW_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
		vulkan::ContextImpl::setViewport(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
		vulkan::ContextImpl::setScissor(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
		vulkan::ContextImpl::enable(graphics::enable::SCISSOR_TEST, false);
		vulkan::ContextImpl::clearColorBuffer(0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}

	uploadPresenterTexture(_output.presentFrame);

	graphics::Context::BindTextureParameters bindParams{};
	bindParams.texture = m_presentTexture;
	bindParams.textureUnitIndex = graphics::textureIndices::Tex[0];
	bindParams.target = graphics::textureTarget::TEXTURE_2D;
	vulkan::ContextImpl::bindTexture(bindParams);

	const u32 viewportWidth = std::max<u32>(1U, m_windowInfo.width > 0U ? m_windowInfo.width : _output.presentFrame.width);
	const u32 viewportHeight = std::max<u32>(1U, m_windowInfo.height > 0U ? m_windowInfo.height : _output.presentFrame.height);
	vulkan::ContextImpl::bindFramebuffer(graphics::bufferTarget::DRAW_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
	vulkan::ContextImpl::setViewport(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
	vulkan::ContextImpl::setScissor(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
	vulkan::ContextImpl::enable(graphics::enable::SCISSOR_TEST, true);
	vulkan::ContextImpl::enable(graphics::enable::CULL_FACE, false);
	vulkan::ContextImpl::enable(graphics::enable::BLEND, false);
	vulkan::ContextImpl::enable(graphics::enable::DEPTH_TEST, false);
	vulkan::ContextImpl::enableDepthWrite(false);
	vulkan::ContextImpl::setDepthCompare(graphics::compare::ALWAYS);

	RectVertex vertices[4]{};
	buildFullscreenRect(vertices, shouldFlipPresentedFrameY());

	graphics::Context::DrawRectParameters drawParams{};
	drawParams.mode = graphics::drawmode::TRIANGLE_STRIP;
	drawParams.texrect = true;
	drawParams.verticesCount = 4U;
	drawParams.vertices = vertices;
	drawParams.combiner = nullptr;
	vulkan::ContextImpl::drawRects(drawParams);
}

bool ContextImpl::present()
{
	syncIngressFrame();
	const u64 frameId = m_ingressCounters.frameId;
	if (!m_sameFramePresent.active || m_sameFramePresent.frameId != frameId)
		m_sameFramePresent = SameFramePresentAccumulator{};

	ForensicsIngressSnapshot ingress{};
	ingress.frameId = frameId;
	ingress.stateCallCount = m_ingressCounters.stateCallCount;
	ingress.triangleCallCount = m_ingressCounters.triangleCallCount;
	ingress.triangleVertexCount = m_ingressCounters.triangleVertexCount;
	ingress.triangleBoundsValid = m_ingressCounters.triangleBoundsValid;
	ingress.triangleMinX = m_ingressCounters.triangleMinX;
	ingress.triangleMinY = m_ingressCounters.triangleMinY;
	ingress.triangleMaxX = m_ingressCounters.triangleMaxX;
	ingress.triangleMaxY = m_ingressCounters.triangleMaxY;
	ingress.rectCallCount = m_ingressCounters.rectCallCount;
	ingress.rectTexrectCallCount = m_ingressCounters.rectTexrectCallCount;
	ingress.rectVertexCount = m_ingressCounters.rectVertexCount;
	ingress.rectBoundsValid = m_ingressCounters.rectBoundsValid;
	ingress.rectMinX = m_ingressCounters.rectMinX;
	ingress.rectMinY = m_ingressCounters.rectMinY;
	ingress.rectMaxX = m_ingressCounters.rectMaxX;
	ingress.rectMaxY = m_ingressCounters.rectMaxY;
	ingress.lineCallCount = m_ingressCounters.lineCallCount;
	ingress.lineVertexCount = m_ingressCounters.lineVertexCount;
	ingress.lineBoundsValid = m_ingressCounters.lineBoundsValid;
	ingress.lineMinX = m_ingressCounters.lineMinX;
	ingress.lineMinY = m_ingressCounters.lineMinY;
	ingress.lineMaxX = m_ingressCounters.lineMaxX;
	ingress.lineMaxY = m_ingressCounters.lineMaxY;
	ingress.shadowDrawForwarding = shadowDrawForwardingEnabled();
	const bool shadowPresentRequested = shadowPresentEnabled();
	ingress.shadowPresent = ingress.shadowDrawForwarding && shadowPresentRequested;
	if (shadowPresentRequested && !ingress.shadowDrawForwarding) {
		static bool warnedMissingShadowDraw = false;
		if (!warnedMissingShadowDraw) {
			LOG(
				LOG_WARNING,
				"REALITYVK_RVK2_SHADOW_PRESENT requested without REALITYVK_RVK2_SHADOW_DRAW; falling back to executor-present.");
			warnedMissingShadowDraw = true;
		}
	}

	const ExecutorConfig config = buildExecutorConfigFromVIRegisters(frameId);
	m_executor.updateConfig(config);
	const std::vector<TMEMWordsSnapshot> * tmemSnapshots = &runtime().tmemSnapshots();
	const std::vector<u32> * workTMEMSnapshotIndices = &runtime().renderWorkTMEMSnapshotIndices();
	if (debugDisableTMEMSnapshotsRequested()) {
		tmemSnapshots = nullptr;
		workTMEMSnapshotIndices = nullptr;
	}
	ExecutorOutput output =
		m_executor.executeWithOutput(
			runtime().renderPlan(),
			runtime().submissionPlan(),
			tmemSnapshots,
			workTMEMSnapshotIndices);

	const bool allowSameFramePresentAccum = !debugDisableSameFramePresentAccumRequested();
	if (allowSameFramePresentAccum
		&& output.presentFrame.width != 0U
		&& output.presentFrame.height != 0U
		&& !output.presentFrame.pixels.empty()) {
		const size_t requiredPixelCount =
			static_cast<size_t>(output.presentFrame.width)
			* static_cast<size_t>(output.presentFrame.height);
		if (output.presentFrame.pixels.size() >= requiredPixelCount) {
			const bool sameFrameCompatible =
				m_sameFramePresent.active
				&& m_sameFramePresent.frameId == frameId
				&& m_sameFramePresent.width == output.presentFrame.width
				&& m_sameFramePresent.height == output.presentFrame.height
				&& m_sameFramePresent.pixels.size() >= requiredPixelCount;
			if (!sameFrameCompatible || m_sameFramePresent.passCount == 0ULL) {
				m_sameFramePresent = SameFramePresentAccumulator{};
				m_sameFramePresent.active = true;
				m_sameFramePresent.frameId = frameId;
				m_sameFramePresent.passCount = 1ULL;
				m_sameFramePresent.width = output.presentFrame.width;
				m_sameFramePresent.height = output.presentFrame.height;
				m_sameFramePresent.pixels.assign(
					output.presentFrame.pixels.begin(),
					output.presentFrame.pixels.begin() + requiredPixelCount);
				u64 nonBlackCount = 0ULL;
				for (size_t i = 0; i < requiredPixelCount; ++i) {
					if (pixelHasVisibleColor(m_sameFramePresent.pixels[i]))
						++nonBlackCount;
				}
				m_sameFramePresent.nonBlackCount = nonBlackCount;
			} else {
				++m_sameFramePresent.passCount;
				u64 retainedNonBlackOverBlackCount = 0ULL;
				u64 promotedBlackToNonBlackCount = 0ULL;
				u64 replacedNonBlackCount = 0ULL;
				u64 mergedNonBlackCount = 0ULL;
				for (size_t i = 0; i < requiredPixelCount; ++i) {
					const u32 previousPixel = m_sameFramePresent.pixels[i];
					const u32 candidatePixel = output.presentFrame.pixels[i];
					const bool previousNonBlack = pixelHasVisibleColor(previousPixel);
					const bool candidateNonBlack = pixelHasVisibleColor(candidatePixel);
					u32 mergedPixel = candidatePixel;
					if (previousNonBlack && !candidateNonBlack) {
						mergedPixel = previousPixel;
						++retainedNonBlackOverBlackCount;
					} else if (!previousNonBlack && candidateNonBlack) {
						++promotedBlackToNonBlackCount;
					} else if (previousNonBlack && candidateNonBlack && previousPixel != candidatePixel) {
						++replacedNonBlackCount;
					}
					output.presentFrame.pixels[i] = mergedPixel;
					m_sameFramePresent.pixels[i] = mergedPixel;
					if (pixelHasVisibleColor(mergedPixel))
						++mergedNonBlackCount;
				}
				m_sameFramePresent.retainedNonBlackOverBlackCount += retainedNonBlackOverBlackCount;
				m_sameFramePresent.promotedBlackToNonBlackCount += promotedBlackToNonBlackCount;
				m_sameFramePresent.replacedNonBlackCount += replacedNonBlackCount;
				m_sameFramePresent.nonBlackCount = mergedNonBlackCount;
				ingress.sameFramePresentMerged = true;
			}
			ingress.sameFramePresentPass = m_sameFramePresent.passCount;
			ingress.sameFramePresentRetainedNonBlackOverBlackCount = m_sameFramePresent.retainedNonBlackOverBlackCount;
			ingress.sameFramePresentPromotedBlackToNonBlackCount = m_sameFramePresent.promotedBlackToNonBlackCount;
			ingress.sameFramePresentReplacedNonBlackCount = m_sameFramePresent.replacedNonBlackCount;
			ingress.sameFramePresentOutputNonBlackCount = m_sameFramePresent.nonBlackCount;
		}
	}
	dumpExecutorPresentFrameIfRequested(frameId, output);

	if (config.textureReplacementLogSummary && output.summary.textureReplacementEnabled) {
		LOG(
			LOG_WARNING,
			"rvk2 tx summary: enabled=1 entries=%llu pixels=%llu samples=%llu hits=%llu misses=%llu",
			static_cast<unsigned long long>(output.summary.textureReplacementEntryCount),
			static_cast<unsigned long long>(output.summary.textureReplacementPixelCount),
			static_cast<unsigned long long>(output.summary.textureReplacementSampleCount),
			static_cast<unsigned long long>(output.summary.textureReplacementHitCount),
			static_cast<unsigned long long>(output.summary.textureReplacementMissCount));
	}
	writeTextureReplacementSummaryFile(config, output.summary);
	appendFrameForensicsRecord(output, ingress);
	if (!ingress.shadowPresent)
		renderPresentedFrame(output);
	return vulkan::ContextImpl::present();
}

} // namespace rvk2
