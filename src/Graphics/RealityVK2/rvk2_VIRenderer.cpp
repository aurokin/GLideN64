#include "rvk2_VIRenderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;
constexpr u32 kVIStatusTypeMask = 0x3U;
constexpr u32 kVIStatusGammaEnabled = 0x000008U;
constexpr u32 kVIStatusDivotEnabled = 0x000010U;
constexpr u32 kVIStatusSerrateEnabled = 0x000040U;

struct VIResolvedState
{
	bool useRegisters = false;
	bool gammaEnabled = false;
	bool divotEnabled = false;
	bool interlaced = false;
	u8 interlaceField = 0U;
	u16 sourceWidth = 0U;
	u16 sourceHeight = 0U;
	u32 outputWidth = 0U;
	u32 outputHeight = 0U;
	u32 xStart = 0U;
	u32 yStart = 0U;
	u32 xStep = 1024U;
	u32 yStep = 1024U;
};

inline u32 clampU32(u32 _value, u32 _minimum, u32 _maximum)
{
	if (_value < _minimum)
		return _minimum;
	if (_value > _maximum)
		return _maximum;
	return _value;
}

inline void updateHashByte(u64 & _hash, u8 _value)
{
	_hash ^= static_cast<u64>(_value);
	_hash *= kFnvPrime;
}

inline bool parseAspect(const char * _text, u8 & _x, u8 & _y)
{
	if (_text == nullptr || _text[0] == '\0')
		return false;
	unsigned long x = 0UL;
	unsigned long y = 0UL;
	if (std::sscanf(_text, "%lu:%lu", &x, &y) != 2)
		return false;
	if (x == 0UL || y == 0UL || x > 255UL || y > 255UL)
		return false;
	_x = static_cast<u8>(x);
	_y = static_cast<u8>(y);
	return true;
}

inline size_t pixelIndex(u32 _width, u32 _x, u32 _y)
{
	return static_cast<size_t>(_y) * static_cast<size_t>(_width) + static_cast<size_t>(_x);
}

u32 integerSqrt(u32 _value)
{
	u32 result = 0U;
	u32 bit = 1U << 30U;
	while (bit > _value)
		bit >>= 2U;
	while (bit != 0U) {
		if (_value >= result + bit) {
			_value -= result + bit;
			result = (result >> 1U) + bit;
		}
		else
			result >>= 1U;
		bit >>= 2U;
	}
	return result;
}

u8 applyGammaChannel(u8 _channel)
{
	return static_cast<u8>(integerSqrt(static_cast<u32>(_channel) * 255U));
}

u32 applyGammaToPixel(u32 _pixel)
{
	const u8 r = applyGammaChannel(static_cast<u8>((_pixel >> 24U) & 0xFFU));
	const u8 g = applyGammaChannel(static_cast<u8>((_pixel >> 16U) & 0xFFU));
	const u8 b = applyGammaChannel(static_cast<u8>((_pixel >> 8U) & 0xFFU));
	const u8 a = static_cast<u8>(_pixel & 0xFFU);
	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

u8 median3U8(u8 _a, u8 _b, u8 _c)
{
	if (_a > _b)
		std::swap(_a, _b);
	if (_b > _c)
		std::swap(_b, _c);
	if (_a > _b)
		std::swap(_a, _b);
	return _b;
}

u32 applyDivotToPixel(u32 _left, u32 _center, u32 _right)
{
	const u8 lr = static_cast<u8>((_left >> 24U) & 0xFFU);
	const u8 lg = static_cast<u8>((_left >> 16U) & 0xFFU);
	const u8 lb = static_cast<u8>((_left >> 8U) & 0xFFU);
	const u8 cr = static_cast<u8>((_center >> 24U) & 0xFFU);
	const u8 cg = static_cast<u8>((_center >> 16U) & 0xFFU);
	const u8 cb = static_cast<u8>((_center >> 8U) & 0xFFU);
	const u8 ca = static_cast<u8>(_center & 0xFFU);
	const u8 rr = static_cast<u8>((_right >> 24U) & 0xFFU);
	const u8 rg = static_cast<u8>((_right >> 16U) & 0xFFU);
	const u8 rb = static_cast<u8>((_right >> 8U) & 0xFFU);
	return (static_cast<u32>(median3U8(lr, cr, rr)) << 24U)
		| (static_cast<u32>(median3U8(lg, cg, rg)) << 16U)
		| (static_cast<u32>(median3U8(lb, cb, rb)) << 8U)
		| static_cast<u32>(ca);
}

u32 deriveOutputWidthFromRegisters(const rvk2::VIRegisterState & _registers, u32 _fallback)
{
	const u32 hStart = (_registers.hStart >> 16U) & 0x3FFU;
	const u32 hEnd = _registers.hStart & 0x3FFU;
	if (hEnd <= hStart)
		return std::max<u32>(1U, _fallback);
	return hEnd - hStart;
}

u32 deriveOutputHeightFromRegisters(const rvk2::VIRegisterState & _registers, u32 _fallback)
{
	const u32 vStart = (_registers.vStart >> 16U) & 0x3FFU;
	u32 vEnd = _registers.vStart & 0x3FFU;
	const bool isPal = (_registers.vSync & 0x3FFU) > 550U;
	if (vEnd < vStart)
		vEnd = isPal ? (44U + 576U) : (34U + 480U);
	if (vEnd <= vStart)
		return std::max<u32>(1U, _fallback);
	return std::max<u32>(1U, (vEnd - vStart) >> 1U);
}

VIResolvedState resolveVIState(
	const rvk2::VIFrameInput & _input,
	const rvk2::VIRendererConfig & _config)
{
	VIResolvedState state{};
	state.sourceWidth = _input.sourceWidth;
	state.sourceHeight = _input.sourceHeight;
	state.outputWidth = _input.sourceWidth;
	state.outputHeight = _input.sourceHeight;

	if (!_input.registers.valid)
		return state;

	if ((_input.registers.status & kVIStatusTypeMask) == 0U) {
		state.outputWidth = 0U;
		state.outputHeight = 0U;
		return state;
	}

	state.useRegisters = true;
	state.gammaEnabled = (_input.registers.status & kVIStatusGammaEnabled) != 0U;
	state.divotEnabled = (_input.registers.status & kVIStatusDivotEnabled) != 0U;
	state.interlaced = (_input.registers.status & kVIStatusSerrateEnabled) != 0U;
	state.interlaceField = static_cast<u8>(_input.registers.vCurrentLine & 0x1U);
	const u32 viWidth = _input.registers.width & 0x0FFFU;
	if (viWidth != 0U)
		state.sourceWidth = static_cast<u16>(clampU32(std::min<u32>(state.sourceWidth, viWidth), 1U, state.sourceWidth));
	state.xStart = (_input.registers.xScale >> 16U) & 0x0FFFU;
	state.yStart = (_input.registers.yScale >> 16U) & 0x0FFFU;
	state.xStep = _input.registers.xScale & 0x0FFFU;
	state.yStep = _input.registers.yScale & 0x0FFFU;
	if (state.xStep == 0U)
		state.xStep = 1024U;
	if (state.yStep == 0U)
		state.yStep = 1024U;

	state.outputWidth = clampU32(
		deriveOutputWidthFromRegisters(_input.registers, _input.sourceWidth),
		1U,
		std::max<u32>(1U, _config.maxOutputWidth));
	state.outputHeight = clampU32(
		deriveOutputHeightFromRegisters(_input.registers, _input.sourceHeight),
		1U,
		std::max<u32>(1U, _config.maxOutputHeight));
	return state;
}

} // namespace

namespace rvk2 {

VIRendererConfig loadVIRendererConfigFromEnv()
{
	VIRendererConfig config{};
	u8 aspectX = config.aspectX;
	u8 aspectY = config.aspectY;
	if (parseAspect(std::getenv("REALITYVK2_VI_ASPECT"), aspectX, aspectY)) {
		config.aspectX = aspectX;
		config.aspectY = aspectY;
	}
	return config;
}

VIRenderer::VIRenderer(const VIRendererConfig & _config)
	: m_config(_config)
{
}

VIFrameSummary VIRenderer::present(
	const VIFrameInput & _input,
	std::vector<u32> * _outputPixels) const
{
	VIFrameSummary summary{};
	const u32 aspectX = m_config.aspectX == 0U ? 1U : m_config.aspectX;
	const u32 aspectY = m_config.aspectY == 0U ? 1U : m_config.aspectY;
	summary.aspectX = static_cast<u8>(aspectX);
	summary.aspectY = static_cast<u8>(aspectY);

	if (_input.sourceWidth == 0U
		|| _input.sourceHeight == 0U
		|| _input.sourcePixels == nullptr
		|| _input.sourcePixels->empty()) {
		return summary;
	}

	const size_t requiredPixels =
		static_cast<size_t>(_input.sourceWidth) * static_cast<size_t>(_input.sourceHeight);
	if (_input.sourcePixels->size() < requiredPixels)
		return summary;

	const VIResolvedState viState = resolveVIState(_input, m_config);
	if (viState.outputWidth == 0U || viState.outputHeight == 0U)
		return summary;

	u32 outputWidth = viState.outputWidth;
	u32 outputHeight = viState.outputHeight;
	const u64 sourceScaled = static_cast<u64>(viState.outputWidth) * static_cast<u64>(aspectY);
	const u64 targetScaled = static_cast<u64>(viState.outputHeight) * static_cast<u64>(aspectX);
	if (sourceScaled > targetScaled) {
		outputHeight = static_cast<u32>(
			(static_cast<u64>(viState.outputWidth) * static_cast<u64>(aspectY)
				+ static_cast<u64>(aspectX) - 1ULL)
			/ static_cast<u64>(aspectX));
	}
	else if (sourceScaled < targetScaled) {
		outputWidth = static_cast<u32>(
			(static_cast<u64>(viState.outputHeight) * static_cast<u64>(aspectX)
				+ static_cast<u64>(aspectY) - 1ULL)
			/ static_cast<u64>(aspectY));
	}

	outputWidth = clampU32(outputWidth, 1U, std::max<u32>(1U, m_config.maxOutputWidth));
	outputHeight = clampU32(outputHeight, 1U, std::max<u32>(1U, m_config.maxOutputHeight));

	u32 contentWidth = outputWidth;
	u32 contentHeight = outputHeight;
	const u64 contentSourceScaled = static_cast<u64>(viState.outputWidth) * static_cast<u64>(outputHeight);
	const u64 contentOutputScaled = static_cast<u64>(viState.outputHeight) * static_cast<u64>(outputWidth);
	if (contentSourceScaled > contentOutputScaled) {
		contentHeight = static_cast<u32>(
			(std::max<u64>(1ULL,
				static_cast<u64>(outputWidth) * static_cast<u64>(viState.outputHeight)))
			/ static_cast<u64>(viState.outputWidth));
		contentHeight = std::max<u32>(1U, std::min<u32>(contentHeight, outputHeight));
	}
	else if (contentSourceScaled < contentOutputScaled) {
		contentWidth = static_cast<u32>(
			(std::max<u64>(1ULL,
				static_cast<u64>(outputHeight) * static_cast<u64>(viState.outputWidth)))
			/ static_cast<u64>(viState.outputHeight));
		contentWidth = std::max<u32>(1U, std::min<u32>(contentWidth, outputWidth));
	}

	const u32 contentX = (outputWidth - contentWidth) / 2U;
	const u32 contentY = (outputHeight - contentHeight) / 2U;

	summary.presentWidth = outputWidth;
	summary.presentHeight = outputHeight;
	summary.contentX = contentX;
	summary.contentY = contentY;
	summary.contentWidth = contentWidth;
	summary.contentHeight = contentHeight;
	if (_outputPixels != nullptr)
		_outputPixels->assign(static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight), 0U);

	u64 hash = kFnvOffset;
	for (u32 y = 0U; y < outputHeight; ++y) {
		for (u32 x = 0U; x < outputWidth; ++x) {
			u32 pixel = 0U;
			if (x >= contentX
				&& x < contentX + contentWidth
				&& y >= contentY
				&& y < contentY + contentHeight) {
				const u32 contentXLocal = x - contentX;
				const u32 contentYLocal = y - contentY;
				const u32 baseX = std::min<u32>(
					viState.outputWidth - 1U,
					(contentXLocal * static_cast<u32>(viState.outputWidth)) / contentWidth);
				const u32 baseY = std::min<u32>(
					viState.outputHeight - 1U,
					(contentYLocal * static_cast<u32>(viState.outputHeight)) / contentHeight);

				u32 sourceX = 0U;
				u32 sourceY = 0U;
					if (viState.useRegisters) {
						sourceX = std::min<u32>(
							viState.sourceWidth - 1U,
							(viState.xStart + baseX * viState.xStep) >> 10U);
						sourceY = std::min<u32>(
							viState.sourceHeight - 1U,
							(viState.yStart + baseY * viState.yStep) >> 10U);
					}
					else {
						sourceX = std::min<u32>(
							viState.sourceWidth - 1U,
							(baseX * static_cast<u32>(viState.sourceWidth)) / viState.outputWidth);
						sourceY = std::min<u32>(
							viState.sourceHeight - 1U,
							(baseY * static_cast<u32>(viState.sourceHeight)) / viState.outputHeight);
					}
					if (viState.interlaced) {
						sourceY = std::min<u32>(
							viState.sourceHeight - 1U,
							sourceY * 2U + static_cast<u32>(viState.interlaceField));
					}
					const size_t sampleIndex = pixelIndex(_input.sourceWidth, sourceX, sourceY);
					pixel = (*_input.sourcePixels)[sampleIndex];
					if (viState.divotEnabled && viState.sourceWidth > 1U) {
						const u32 sourceXLeft = sourceX > 0U ? sourceX - 1U : sourceX;
						const u32 sourceXRight = std::min<u32>(viState.sourceWidth - 1U, sourceX + 1U);
						const u32 leftPixel = (*_input.sourcePixels)[pixelIndex(_input.sourceWidth, sourceXLeft, sourceY)];
						const u32 rightPixel = (*_input.sourcePixels)[pixelIndex(_input.sourceWidth, sourceXRight, sourceY)];
						pixel = applyDivotToPixel(leftPixel, pixel, rightPixel);
					}
				}
				if (viState.gammaEnabled)
					pixel = applyGammaToPixel(pixel);
			if (_outputPixels != nullptr)
				(*_outputPixels)[pixelIndex(outputWidth, x, y)] = pixel;
			updateHashByte(hash, static_cast<u8>((pixel >> 0U) & 0xFFU));
			updateHashByte(hash, static_cast<u8>((pixel >> 8U) & 0xFFU));
			updateHashByte(hash, static_cast<u8>((pixel >> 16U) & 0xFFU));
			updateHashByte(hash, static_cast<u8>((pixel >> 24U) & 0xFFU));
		}
	}
	summary.presentHash = hash;
	return summary;
}

} // namespace rvk2
