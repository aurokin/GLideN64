#include "rvk2_ContextImpl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <Log.h>
#include <Graphics/Parameters.h>
#include <N64.h>

#include "rvk2_Runtime.h"

namespace {

bool envFlagEnabled(const char * _key, bool _defaultValue)
{
	const char * value = std::getenv(_key);
	if (value == nullptr || value[0] == '\0')
		return _defaultValue;

	char first = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
	if (first == '0' || first == 'f' || first == 'n')
		return false;
	return true;
}

bool shouldFlipPresentedFrameY()
{
	// Default to dumpfb-aligned orientation for emulator output.
	return envFlagEnabled("REALITYVK_RVK2_PRESENT_FLIP_Y", true);
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

rvk2::ExecutorConfig buildExecutorConfigFromVIRegisters()
{
	rvk2::ExecutorConfig config = rvk2::loadExecutorConfigFromEnv();
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

void appendFrameForensicsRecord(const rvk2::ExecutorOutput & _output)
{
	const char * path = std::getenv("REALITYVK2_FRAME_FORENSICS_FILE");
	if (path == nullptr || path[0] == '\0')
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
		"frame=%llu\twork=%llu\tbatches=%llu\twrites=%llu\tsurfaces=%llu\tpresent_surface=0x%08X\tpresent_select=%u\tpresent_hash=0x%016llX\tpresent_w=%u\tpresent_h=%u\tvi_valid=%u\tvi_origin=0x%08X\tvi_origin_match=%u\tvi_reject=%u\tvi_type=%u\tvi_use_regs=%u\tvi_src_w=%u\tvi_src_h=%u\tvi_out_w=%u\tvi_out_h=%u\tvi_stride=%u\tselected_surface_writes=%llu\tselected_surface_works=%llu\tselected_surface_size=%u\tselected_surface_w=%u\tselected_surface_h=%u\ttx_samples=%llu\ttx_tmem=%llu\ttx_tmem_try=%llu\ttx_tmem_reject_fmt=%llu\ttx_tmem_reject_size=%llu\ttx_tmem_reject_coord=%llu\ttx_tmem32_cmp=%llu\ttx_tmem32_mismatch=%llu\ttx_tmem32_alt_noxor_mismatch=%llu\ttx_tmem32_alt_abs_mismatch=%llu\ttx_tmem32_alt_direct_mismatch=%llu\ttx_tmem32_alt_direct_swap_mismatch=%llu\ttx_tmem32_alt_tileline_xor_mismatch=%llu\ttx_tmem32_alt_tileline_evenodd_mismatch=%llu\ttx_tmem32_alt_direct_evenodd_mismatch=%llu\ttx_tmem32_alt_loadkind_mismatch=%llu\ttx_rdram=%llu\ttx_synth=%llu\ttx_lut=%llu\ttx_mask_allow=%llu\ttx_mask_reject=%llu\tcomb_ops=%llu\tcomb_cycle2_ops=%llu\tblend_ops=%llu\tblend_enabled_ops=%llu\tblend_force_ops=%llu\tblend_aa_ops=%llu\tblend_p_mem_ops=%llu\tblend_m_mem_ops=%llu\tblend_divide_ops=%llu\tblend_nodivide_ops=%llu\talpha_tests=%llu\talpha_rejects=%llu\tcvg_tests=%llu\tcvg_rejects=%llu\tblend_cvg_eval=%llu\tblend_cvg_zero=%llu\tblend_cvg_overflow=%llu\tcvg_write_eval=%llu\tcvg_write_zero=%llu\tcvg_write_overflow=%llu\tdepth_eval=%llu\tdepth_reject=%llu\tdepth_update=%llu\tdither_color=%llu\tdither_alpha=%llu\ttexedge_promote=%llu\tconvert_one_force=%llu\tblend_a_sel0=%llu\tblend_a_sel1=%llu\tblend_a_sel2=%llu\tblend_a_sel3=%llu\tblend_b_sel0=%llu\tblend_b_sel1=%llu\tblend_b_sel2=%llu\tblend_b_sel3=%llu\tblend_p_sel0=%llu\tblend_p_sel1=%llu\tblend_p_sel2=%llu\tblend_p_sel3=%llu\tblend_m_sel0=%llu\tblend_m_sel1=%llu\tblend_m_sel2=%llu\tblend_m_sel3=%llu\tstage_t2c_delta=%llu\tstage_c2b_delta=%llu\tstage_b2f_delta=%llu\tstage_t2f_delta=%llu\tstage_textured_writes=%llu\tstage_textured_rect=%llu\tstage_textured_tri=%llu\tstage_imread=%llu\tstage_tx_repl=%llu\tstage_tx_tmem=%llu\tstage_tx_rdram=%llu\tstage_tx_synth=%llu\twork_fill=%llu\twork_texrect=%llu\twork_tri=%llu\twork_textured=%llu\twrite_fill=%llu\twrite_texrect=%llu\twrite_tri=%llu\tout_luma_sum=%llu\tout_luma_avg_x1000=%llu\tvi_src_samples=%llu\tvi_src_invalid=%llu\tvi_src_luma_avg_x1000=%llu\tvi_out_luma_avg_x1000=%llu\tvi_out_nonblack=%llu",
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
		static_cast<u32>(summary.viOriginMatchedSurface),
		static_cast<u32>(summary.viRejectReason),
		static_cast<u32>(summary.viResolvedType),
		static_cast<u32>(summary.viResolvedUsesRegisters),
		summary.viResolvedSourceWidth,
		summary.viResolvedSourceHeight,
		summary.viResolvedOutputWidth,
		summary.viResolvedOutputHeight,
		summary.viResolvedLineStride,
		static_cast<unsigned long long>(summary.selectedPresentSurfaceWriteCount),
		static_cast<unsigned long long>(summary.selectedPresentSurfaceWorkCount),
		static_cast<u32>(summary.selectedPresentSurfaceSize),
		summary.selectedPresentSurfaceWidth,
		summary.selectedPresentSurfaceHeight,
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
			"\ts%u_addr=0x%08X\ts%u_writes=%llu\ts%u_works=%llu",
			i,
			summary.debugSurfaceAddress[i],
			i,
			static_cast<unsigned long long>(summary.debugSurfaceWriteCount[i]),
			i,
			static_cast<unsigned long long>(summary.debugSurfaceWorkCount[i]));
	}
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
	vulkan::ContextImpl::destroy();
}

void ContextImpl::enable(graphics::EnableParam _parameter, bool _enable)
{
	(void)_parameter;
	(void)_enable;
}

u32 ContextImpl::isEnabled(graphics::EnableParam _parameter)
{
	(void)_parameter;
	return 0U;
}

void ContextImpl::cullFace(graphics::CullModeParam _mode)
{
	(void)_mode;
}

void ContextImpl::enableDepthWrite(bool _enable)
{
	(void)_enable;
}

void ContextImpl::setDepthCompare(graphics::CompareParam _mode)
{
	(void)_mode;
}

void ContextImpl::setViewport(s32 _x, s32 _y, s32 _width, s32 _height)
{
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
}

void ContextImpl::setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
{
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
}

void ContextImpl::setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor)
{
	(void)_sfactor;
	(void)_dfactor;
}

void ContextImpl::setBlendingSeparate(
	graphics::BlendParam _sfactorcolor,
	graphics::BlendParam _dfactorcolor,
	graphics::BlendParam _sfactoralpha,
	graphics::BlendParam _dfactoralpha)
{
	(void)_sfactorcolor;
	(void)_dfactorcolor;
	(void)_sfactoralpha;
	(void)_dfactoralpha;
}

void ContextImpl::setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
}

void ContextImpl::setPolygonOffset(f32 _factor, f32 _units)
{
	(void)_factor;
	(void)_units;
}

void ContextImpl::drawTriangles(const graphics::Context::DrawTriangleParameters & _params)
{
	(void)_params;
}

void ContextImpl::drawRects(const graphics::Context::DrawRectParameters & _params)
{
	(void)_params;
}

void ContextImpl::drawLine(f32 _width, SPVertex * _vertices)
{
	(void)_width;
	(void)_vertices;
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
	const ExecutorConfig config = buildExecutorConfigFromVIRegisters();
	m_executor.updateConfig(config);
	const ExecutorOutput output =
		m_executor.executeWithOutput(
			runtime().renderPlan(),
			runtime().submissionPlan(),
			&runtime().tmemSnapshots(),
			&runtime().renderWorkTMEMSnapshotIndices());
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
	appendFrameForensicsRecord(output);
	renderPresentedFrame(output);
	return vulkan::ContextImpl::present();
}

} // namespace rvk2
