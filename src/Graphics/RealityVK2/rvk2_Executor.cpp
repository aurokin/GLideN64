#include "rvk2_Executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rvk2_VIRenderer.h"

namespace {

inline u32 clampU32(u32 _value, u32 _minimum, u32 _maximum)
{
	if (_value < _minimum)
		return _minimum;
	if (_value > _maximum)
		return _maximum;
	return _value;
}

inline u32 decodeFillColor(u32 _fillColor, u8 _colorImageSize)
{
	if (_colorImageSize == 3U)
		return _fillColor;

	const u16 color16 = static_cast<u16>(_fillColor & 0xFFFFU);
	const u8 r5 = static_cast<u8>((color16 >> 11) & 0x1FU);
	const u8 g5 = static_cast<u8>((color16 >> 6) & 0x1FU);
	const u8 b5 = static_cast<u8>((color16 >> 1) & 0x1FU);
	const u8 a1 = static_cast<u8>(color16 & 0x1U);
	const u8 r = static_cast<u8>((static_cast<u32>(r5) * 255U + 15U) / 31U);
	const u8 g = static_cast<u8>((static_cast<u32>(g5) * 255U + 15U) / 31U);
	const u8 b = static_cast<u8>((static_cast<u32>(b5) * 255U + 15U) / 31U);
	const u8 a = a1 != 0U ? 255U : 0U;
	return (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
}

inline u32 pseudoTexel(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	const s32 dx = static_cast<s32>(_x) - static_cast<s32>(_work.rectULX);
	const s32 dy = static_cast<s32>(_y) - static_cast<s32>(_work.rectULY);
	const s32 s = static_cast<s32>(_work.texS) + ((dx * static_cast<s32>(_work.texDSDX)) >> 5);
	const s32 t = static_cast<s32>(_work.texT) + ((dy * static_cast<s32>(_work.texDTDY)) >> 5);
	u64 seed = static_cast<u64>(_work.textureImageAddress);
	seed ^= static_cast<u64>(_work.tileTmem) << 12U;
	seed ^= static_cast<u64>(_work.tileLine) << 20U;
	seed ^= static_cast<u64>(static_cast<u32>(s) & 0xFFFFU) << 1U;
	seed ^= static_cast<u64>(static_cast<u32>(t) & 0xFFFFU) << 17U;
	seed ^= static_cast<u64>(_x) << 33U;
	seed ^= static_cast<u64>(_y) << 45U;
	seed ^= static_cast<u64>(_work.combineMux);
	seed *= 0x9E3779B97F4A7C15ULL;
	const u8 r = static_cast<u8>((seed >> 8) & 0xFFU);
	const u8 g = static_cast<u8>((seed >> 24) & 0xFFU);
	const u8 b = static_cast<u8>((seed >> 40) & 0xFFU);
	const u8 a = 255U;
	return (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
}

struct ColorSurface {
	u8 format = 0U;
	u8 size = 0U;
	u16 width = 0U;
	u16 height = 0U;
	std::vector<u32> pixels;
};

inline size_t pixelIndex(u16 _width, u16 _x, u16 _y)
{
	return static_cast<size_t>(_y) * static_cast<size_t>(_width) + static_cast<size_t>(_x);
}

struct WriteBounds
{
	u32 x0 = 0U;
	u32 y0 = 0U;
	u32 x1 = 0U;
	u32 y1 = 0U;
};

void ensureSurfaceSize(
	ColorSurface & _surface,
	u16 _requiredWidth,
	u16 _requiredHeight,
	u16 _maxWidth,
	u16 _maxHeight)
{
	const u16 newWidth = static_cast<u16>(clampU32(_requiredWidth, 1U, _maxWidth));
	const u16 newHeight = static_cast<u16>(clampU32(_requiredHeight, 1U, _maxHeight));
	if (newWidth <= _surface.width && newHeight <= _surface.height)
		return;

	const u16 targetWidth = std::max(_surface.width, newWidth);
	const u16 targetHeight = std::max(_surface.height, newHeight);
	std::vector<u32> resized(static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight), 0U);
	for (u16 y = 0U; y < _surface.height; ++y) {
		for (u16 x = 0U; x < _surface.width; ++x)
			resized[pixelIndex(targetWidth, x, y)] = _surface.pixels[pixelIndex(_surface.width, x, y)];
	}
	_surface.width = targetWidth;
	_surface.height = targetHeight;
	_surface.pixels.swap(resized);
}

bool computeWriteBounds(
	const ColorSurface & _surface,
	const rvk2::RenderWorkPacket & _work,
	WriteBounds & _bounds)
{
	const u32 ulx = std::min<u32>(_work.rectULX, _work.rectLRX);
	const u32 uly = std::min<u32>(_work.rectULY, _work.rectLRY);
	const u32 lrx = std::max<u32>(_work.rectULX, _work.rectLRX);
	const u32 lry = std::max<u32>(_work.rectULY, _work.rectLRY);
	if (lrx < ulx || lry < uly)
		return false;

	const u32 scissorX0 = std::min<u32>(_work.scissorXH, _work.scissorXL);
	const u32 scissorY0 = std::min<u32>(_work.scissorYH, _work.scissorYL);
	const u32 scissorX1 = std::max<u32>(_work.scissorXH, _work.scissorXL);
	const u32 scissorY1 = std::max<u32>(_work.scissorYH, _work.scissorYL);
	const bool defaultScissor =
		_work.scissorXH == 0U
		&& _work.scissorYH == 0U
		&& _work.scissorXL == 0U
		&& _work.scissorYL == 0U;
	const u32 clipX0 = defaultScissor ? ulx : scissorX0;
	const u32 clipY0 = defaultScissor ? uly : scissorY0;
	const u32 clipX1 = defaultScissor ? lrx : scissorX1;
	const u32 clipY1 = defaultScissor ? lry : scissorY1;

	const u32 writeX0 = std::max<u32>(ulx, clipX0);
	const u32 writeY0 = std::max<u32>(uly, clipY0);
	const u32 writeX1 = std::min<u32>(lrx, std::min<u32>(clipX1, _surface.width > 0U ? static_cast<u32>(_surface.width - 1U) : 0U));
	const u32 writeY1 = std::min<u32>(lry, std::min<u32>(clipY1, _surface.height > 0U ? static_cast<u32>(_surface.height - 1U) : 0U));
	if (writeX1 < writeX0 || writeY1 < writeY0)
		return false;

	_bounds.x0 = writeX0;
	_bounds.y0 = writeY0;
	_bounds.x1 = writeX1;
	_bounds.y1 = writeY1;
	return true;
}

inline double edgeFunction(
	double _ax,
	double _ay,
	double _bx,
	double _by,
	double _px,
	double _py)
{
	return (_px - _ax) * (_by - _ay) - (_py - _ay) * (_bx - _ax);
}

inline u32 pseudoTriangleColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	u64 seed = static_cast<u64>(_work.combineMux);
	seed ^= static_cast<u64>(_work.blendParams) << 29U;
	seed ^= static_cast<u64>(_work.sourcePacketId) << 7U;
	seed ^= static_cast<u64>(_x) << 33U;
	seed ^= static_cast<u64>(_y) << 45U;
	seed ^= static_cast<u64>(_work.syncEpoch);
	seed *= 0x9E3779B97F4A7C15ULL;
	const u8 r = static_cast<u8>((seed >> 9) & 0xFFU);
	const u8 g = static_cast<u8>((seed >> 27) & 0xFFU);
	const u8 b = static_cast<u8>((seed >> 41) & 0xFFU);
	const u8 a = 255U;
	return (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
}

void writeRect(
	ColorSurface & _surface,
	const rvk2::RenderWorkPacket & _work,
	const rvk2::ExecutorConfig & _config,
	rvk2::ExecutorSummary & _summary)
{
	const u32 ulx = std::min<u32>(_work.rectULX, _work.rectLRX);
	const u32 uly = std::min<u32>(_work.rectULY, _work.rectLRY);
	const u32 lrx = std::max<u32>(_work.rectULX, _work.rectLRX);
	const u32 lry = std::max<u32>(_work.rectULY, _work.rectLRY);
	if (lrx < ulx || lry < uly)
		return;

	const u16 requiredWidth = static_cast<u16>(std::min<u32>(lrx + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(lry + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);
	WriteBounds bounds{};
	if (!computeWriteBounds(_surface, _work, bounds))
		return;

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		for (u32 x = bounds.x0; x <= bounds.x1; ++x) {
			const u32 rgba =
				_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect)
				? decodeFillColor(_work.fillColor, _work.colorImageSize)
				: pseudoTexel(_work, x, y);
			_surface.pixels[pixelIndex(_surface.width, static_cast<u16>(x), static_cast<u16>(y))] = rgba;
			++_summary.colorWriteCount;
		}
	}
}

void writeTriangle(
	ColorSurface & _surface,
	const rvk2::RenderWorkPacket & _work,
	const rvk2::ExecutorConfig & _config,
	rvk2::ExecutorSummary & _summary)
{
	const u32 ulx = std::min<u32>(_work.rectULX, _work.rectLRX);
	const u32 uly = std::min<u32>(_work.rectULY, _work.rectLRY);
	const u32 lrx = std::max<u32>(_work.rectULX, _work.rectLRX);
	const u32 lry = std::max<u32>(_work.rectULY, _work.rectLRY);
	if (lrx < ulx || lry < uly)
		return;

	const u16 requiredWidth = static_cast<u16>(std::min<u32>(lrx + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(lry + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);

	WriteBounds bounds{};
	if (!computeWriteBounds(_surface, _work, bounds))
		return;

	const double yh = static_cast<double>(_work.triangleYH) * 0.25;
	const double ym = static_cast<double>(_work.triangleYM) * 0.25;
	const double yl = static_cast<double>(_work.triangleYL) * 0.25;
	const double xh = static_cast<double>(_work.triangleXH) / 65536.0;
	const double xl = static_cast<double>(_work.triangleXL) / 65536.0;
	const double xLongAtYL =
		(static_cast<double>(_work.triangleXH)
			+ static_cast<double>(_work.triangleDxHDY) * static_cast<double>(static_cast<s32>(_work.triangleYL) - static_cast<s32>(_work.triangleYH)))
		/ 65536.0;

	const double ax = xh;
	const double ay = yh;
	const double bx = xl;
	const double by = ym;
	const double cx = xLongAtYL;
	const double cy = yl;
	const double area = edgeFunction(ax, ay, bx, by, cx, cy);
	if (area == 0.0)
		return;

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		const double py = static_cast<double>(y) + 0.5;
		for (u32 x = bounds.x0; x <= bounds.x1; ++x) {
			const double px = static_cast<double>(x) + 0.5;
			const double e0 = edgeFunction(ax, ay, bx, by, px, py);
			const double e1 = edgeFunction(bx, by, cx, cy, px, py);
			const double e2 = edgeFunction(cx, cy, ax, ay, px, py);
			const bool inside = area > 0.0
				? (e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0)
				: (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0);
			if (!inside)
				continue;

			const u32 rgba = _work.textured
				? pseudoTexel(_work, x, y)
				: pseudoTriangleColor(_work, x, y);
			_surface.pixels[pixelIndex(_surface.width, static_cast<u16>(x), static_cast<u16>(y))] = rgba;
			++_summary.colorWriteCount;
		}
	}
}

} // namespace

namespace rvk2 {

ExecutorConfig loadExecutorConfigFromEnv()
{
	ExecutorConfig config{};
	const VIRendererConfig viConfig = loadVIRendererConfigFromEnv();
	config.presentAspectX = viConfig.aspectX;
	config.presentAspectY = viConfig.aspectY;
	return config;
}

Executor::Executor(const ExecutorConfig & _config)
	: m_config(_config)
{
}

ExecutorOutput Executor::executeWithOutput(
	const std::vector<RenderWorkPacket> & _workPackets,
	const std::vector<SubmissionBatchPacket> & _batches)
{
	ExecutorOutput output{};
	ExecutorSummary & summary = output.summary;
	summary.presentAspectX = m_config.presentAspectX;
	summary.presentAspectY = m_config.presentAspectY;

	VIRendererConfig viConfig{};
	viConfig.aspectX = m_config.presentAspectX;
	viConfig.aspectY = m_config.presentAspectY;
	viConfig.maxOutputWidth = m_config.maxSurfaceWidth;
	viConfig.maxOutputHeight = m_config.maxSurfaceHeight;
	const VIRenderer viRenderer(viConfig);

	std::unordered_map<u32, ColorSurface> surfaces;
	u32 lastSurfaceAddress = 0U;

	for (const SubmissionBatchPacket & batch : _batches) {
		++summary.executedBatchCount;
		if (batch.firstWorkIndex >= _workPackets.size())
			continue;
		const u32 lastIndex = std::min<u32>(
			batch.lastWorkIndex,
			static_cast<u32>(_workPackets.size() - 1U));

		for (u32 workIndex = batch.firstWorkIndex; workIndex <= lastIndex; ++workIndex) {
			const RenderWorkPacket & work = _workPackets[workIndex];
			++summary.executedWorkCount;
			if (work.opKind != static_cast<u8>(RasterOpKind::kFillRect)
				&& work.opKind != static_cast<u8>(RasterOpKind::kTexRect)
				&& work.opKind != static_cast<u8>(RasterOpKind::kTriangle))
				continue;

			ColorSurface & surface = surfaces[work.colorImageAddress];
			surface.format = work.colorImageFormat;
			surface.size = work.colorImageSize;
			if (surface.width == 0U)
				surface.width = std::max<u16>(1U, std::min<u16>(work.colorImageWidth, m_config.maxSurfaceWidth));
			if (surface.height == 0U)
				surface.height = 1U;
			if (surface.pixels.empty())
				surface.pixels.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);

			if (work.opKind == static_cast<u8>(RasterOpKind::kTriangle))
				writeTriangle(surface, work, m_config, summary);
			else
				writeRect(surface, work, m_config, summary);
			lastSurfaceAddress = work.colorImageAddress;
		}
	}

	summary.surfaceCount = static_cast<u64>(surfaces.size());
	const auto it = surfaces.find(lastSurfaceAddress);
	if (it != surfaces.end()) {
		VIFrameInput presentInput{};
		presentInput.sourceWidth = it->second.width;
		presentInput.sourceHeight = it->second.height;
		presentInput.sourcePixels = &it->second.pixels;
		const VIFrameSummary viSummary = viRenderer.present(presentInput, &output.presentFrame.pixels);
		summary.presentHash = viSummary.presentHash;
		summary.presentWidth = viSummary.presentWidth;
		summary.presentHeight = viSummary.presentHeight;
		output.presentFrame.width = viSummary.presentWidth;
		output.presentFrame.height = viSummary.presentHeight;
	}
	return output;
}

ExecutorSummary Executor::execute(
	const std::vector<RenderWorkPacket> & _workPackets,
	const std::vector<SubmissionBatchPacket> & _batches)
{
	return executeWithOutput(_workPackets, _batches).summary;
}

} // namespace rvk2
