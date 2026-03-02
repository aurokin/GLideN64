#include "rvk2_VIRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;
constexpr u32 kVIStatusTypeMask = 0x3U;
constexpr u32 kVIStatusGammaDitherEnabled = 0x000004U;
constexpr u32 kVIStatusGammaEnabled = 0x000008U;
constexpr u32 kVIStatusDivotEnabled = 0x000010U;
constexpr u32 kVIStatusSerrateEnabled = 0x000040U;
constexpr u32 kVIStatusAAModeMask = 0x000300U;
constexpr u32 kVIStatusDeditherEnabled = 0x010000U;

struct VIResolvedState
{
	bool useRegisters = false;
	bool deditherEnabled = false;
	bool gammaDitherEnabled = false;
	bool gammaEnabled = false;
	bool divotEnabled = false;
	bool interlaced = false;
	u8 interlaceField = 0U;
	u8 aaMode = 0U;
	u8 viType = 0U;
	u64 sourceBasePixelOffset = 0ULL;
	u32 sourceLineStride = 0U;
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

inline u8 clampChannel(s32 _value)
{
	if (_value < 0)
		return 0U;
	if (_value > 255)
		return 255U;
	return static_cast<u8>(_value);
}

u32 applyGammaDitherToPixel(u32 _pixel, u32 _x, u32 _y)
{
	static constexpr int kBayer4x4[16] = {
		-8, 0, -6, 2,
		 4, -4, 6, -2,
		-5, 3, -7, 1,
		 7, -1, 5, -3
	};
	const s32 bayer = static_cast<s32>(kBayer4x4[(_y & 0x3U) * 4U + (_x & 0x3U)]);
	const s32 dither = bayer >= 0 ? ((bayer + 2) >> 2U) : -(((-bayer) + 2) >> 2U);
	const u8 r = clampChannel(static_cast<s32>((_pixel >> 24U) & 0xFFU) + dither);
	const u8 g = clampChannel(static_cast<s32>((_pixel >> 16U) & 0xFFU) + dither);
	const u8 b = clampChannel(static_cast<s32>((_pixel >> 8U) & 0xFFU) + dither);
	const u8 a = static_cast<u8>(_pixel & 0xFFU);
	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

u32 filterAAPixel(
	u32 _center,
	u32 _left,
	u32 _right,
	u32 _up,
	u32 _down,
	u8 _aaMode)
{
	if (_aaMode == 3U)
		return _center;

	const auto selectChannel = [](u32 _pixel, u32 _shift) -> u32 {
		return (_pixel >> _shift) & 0xFFU;
	};

	const auto filterChannel = [&](u32 _shift) -> u8 {
		const u32 center = selectChannel(_center, _shift);
		const u32 left = selectChannel(_left, _shift);
		const u32 right = selectChannel(_right, _shift);
		const u32 up = selectChannel(_up, _shift);
		const u32 down = selectChannel(_down, _shift);
		u32 value = center;
		if (_aaMode == 0U)
			value = (center * 3U + left + right + up + down + 3U) / 7U;
		else if (_aaMode == 1U)
			value = (center * 2U + left + right + 2U) / 4U;
		else
			value = (center * 2U + left + right + up + down + 3U) / 6U;
		return static_cast<u8>(value & 0xFFU);
	};

	const u8 r = filterChannel(24U);
	const u8 g = filterChannel(16U);
	const u8 b = filterChannel(8U);
	const u8 a = static_cast<u8>(_center & 0xFFU);
	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

u8 deditherChannel(u8 _center, const std::array<u8, 8> & _neighbors)
{
	u32 sum = 0U;
	for (u8 value : _neighbors)
		sum += static_cast<u32>(value);
	const u8 mean = static_cast<u8>((sum + 4U) / 8U);
	const s32 delta = static_cast<s32>(_center) - static_cast<s32>(mean);
	if (delta > 48 || delta < -48)
		return _center;
	return static_cast<u8>(
		(static_cast<u32>(_center) + static_cast<u32>(mean) * 3U + 2U) / 4U);
}

u32 applyDeditherToPixel(
	u32 _center,
	u32 _left,
	u32 _right,
	u32 _up,
	u32 _down,
	u32 _upLeft,
	u32 _upRight,
	u32 _downLeft,
	u32 _downRight)
{
	const auto deditherComponent = [&](u32 _shift) -> u8 {
		const u8 center = static_cast<u8>((_center >> _shift) & 0xFFU);
		const std::array<u8, 8> neighbors{
			static_cast<u8>((_left >> _shift) & 0xFFU),
			static_cast<u8>((_right >> _shift) & 0xFFU),
			static_cast<u8>((_up >> _shift) & 0xFFU),
			static_cast<u8>((_down >> _shift) & 0xFFU),
			static_cast<u8>((_upLeft >> _shift) & 0xFFU),
			static_cast<u8>((_upRight >> _shift) & 0xFFU),
			static_cast<u8>((_downLeft >> _shift) & 0xFFU),
			static_cast<u8>((_downRight >> _shift) & 0xFFU)
		};
		return deditherChannel(center, neighbors);
	};

	const u8 r = deditherComponent(24U);
	const u8 g = deditherComponent(16U);
	const u8 b = deditherComponent(8U);
	const u8 a = static_cast<u8>(_center & 0xFFU);
	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

u8 quantize5To8(u8 _value)
{
	const u32 quantized = (static_cast<u32>(_value) * 31U + 127U) / 255U;
	return static_cast<u8>((quantized * 255U + 15U) / 31U);
}

u32 applyVITypeDecode(u32 _pixel, u8 _viType)
{
	if (_viType != 2U)
		return _pixel;

	const u8 r = quantize5To8(static_cast<u8>((_pixel >> 24U) & 0xFFU));
	const u8 g = quantize5To8(static_cast<u8>((_pixel >> 16U) & 0xFFU));
	const u8 b = quantize5To8(static_cast<u8>((_pixel >> 8U) & 0xFFU));
	const u8 a = static_cast<u8>((_pixel & 0xFFU) >= 128U ? 255U : 0U);
	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

u32 deriveOutputWidthFromRegisters(const rvk2::VIRegisterState & _registers)
{
	const u32 hStart = (_registers.hStart >> 16U) & 0x3FFU;
	const u32 hEnd = _registers.hStart & 0x3FFU;
	if (hEnd <= hStart)
		return 0U;
	return hEnd - hStart;
}

u32 deriveOutputHeightFromRegisters(const rvk2::VIRegisterState & _registers)
{
	const u32 vStart = (_registers.vStart >> 16U) & 0x3FFU;
	u32 vEnd = _registers.vStart & 0x3FFU;
	if (vEnd < vStart) {
		const u32 vSync = _registers.vSync & 0x3FFU;
		const u32 wrap = vSync != 0U ? (vSync + 1U) : 1024U;
		vEnd += wrap;
	}
	if (vEnd <= vStart)
		return 0U;
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
	state.viType = static_cast<u8>(_input.registers.status & kVIStatusTypeMask);
	state.deditherEnabled = (_input.registers.status & kVIStatusDeditherEnabled) != 0U;
	state.gammaDitherEnabled = (_input.registers.status & kVIStatusGammaDitherEnabled) != 0U;
	state.gammaEnabled = (_input.registers.status & kVIStatusGammaEnabled) != 0U;
	state.divotEnabled = (_input.registers.status & kVIStatusDivotEnabled) != 0U;
	state.interlaced = (_input.registers.status & kVIStatusSerrateEnabled) != 0U;
	state.interlaceField = static_cast<u8>(_input.registers.vCurrentLine & 0x1U);
	state.aaMode = static_cast<u8>((_input.registers.status & kVIStatusAAModeMask) >> 8U);
	const u32 viWidth = _input.registers.width & 0x0FFFU;
	state.sourceLineStride = viWidth != 0U ? viWidth : static_cast<u32>(state.sourceWidth);
	if (state.sourceLineStride == 0U)
		state.sourceLineStride = static_cast<u32>(state.sourceWidth);
	state.xStart = (_input.registers.xScale >> 16U) & 0x0FFFU;
	state.yStart = (_input.registers.yScale >> 16U) & 0x0FFFU;
	state.xStep = _input.registers.xScale & 0x0FFFU;
	state.yStep = _input.registers.yScale & 0x0FFFU;
	if (state.xStep == 0U)
		state.xStep = 1024U;
	if (state.yStep == 0U)
		state.yStep = 1024U;

	const u32 derivedOutputWidth = deriveOutputWidthFromRegisters(_input.registers);
	const u32 derivedOutputHeight = deriveOutputHeightFromRegisters(_input.registers);
	if (derivedOutputWidth == 0U || derivedOutputHeight == 0U) {
		state.outputWidth = 0U;
		state.outputHeight = 0U;
		return state;
	}

	if (_input.sourceAddressValid) {
		const u32 baseAddress = _input.sourceAddress & 0x00FFFFFFU;
		const u32 originAddress = _input.registers.origin & 0x00FFFFFFU;
		if (originAddress >= baseAddress) {
			const u32 bytesPerPixel = state.viType == 2U ? 2U : (state.viType == 3U ? 4U : 2U);
			if (bytesPerPixel != 0U)
				state.sourceBasePixelOffset = static_cast<u64>(originAddress - baseAddress) / bytesPerPixel;
		}
	}

	state.outputWidth = clampU32(
		derivedOutputWidth,
		1U,
		std::max<u32>(1U, _config.maxOutputWidth));
	state.outputHeight = clampU32(
		derivedOutputHeight,
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

	const u64 sourcePixelCount = static_cast<u64>(_input.sourcePixels->size());
	const u32 sourceLineStride =
		viState.sourceLineStride != 0U
			? viState.sourceLineStride
			: static_cast<u32>(viState.sourceWidth);

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
				u64 sampleIndex = 0ULL;
				bool sampleValid = true;
				if (viState.useRegisters) {
					const u32 sampleXFP = viState.xStart + baseX * viState.xStep;
					u32 sampleYFP = 0U;
					if (viState.interlaced) {
						const u32 fieldBaseY = baseY * 2U + static_cast<u32>(viState.interlaceField);
						sampleYFP = viState.yStart + fieldBaseY * viState.yStep;
					}
					else
						sampleYFP = viState.yStart + baseY * viState.yStep;
					const u64 sampleXLimit = static_cast<u64>(sourceLineStride) << 10U;
					if (static_cast<u64>(sampleXFP) >= sampleXLimit)
						sampleValid = false;
					sourceX = sampleXFP >> 10U;
					sourceY = sampleYFP >> 10U;
					if (sampleValid) {
						const u64 linearOffset =
							viState.sourceBasePixelOffset
							+ static_cast<u64>(sourceY) * static_cast<u64>(sourceLineStride)
							+ static_cast<u64>(sourceX);
						if (linearOffset >= sourcePixelCount)
							sampleValid = false;
						else
							sampleIndex = linearOffset;
					}
				}
				else {
					sourceX = std::min<u32>(
						viState.sourceWidth - 1U,
						(baseX * static_cast<u32>(viState.sourceWidth)) / viState.outputWidth);
					sourceY = std::min<u32>(
						viState.sourceHeight - 1U,
						(baseY * static_cast<u32>(viState.sourceHeight)) / viState.outputHeight);
					if (viState.interlaced) {
						sourceY = std::min<u32>(
							viState.sourceHeight - 1U,
							sourceY * 2U + static_cast<u32>(viState.interlaceField));
					}
					sampleIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceX, sourceY));
				}
				if (sampleValid) {
					pixel = applyVITypeDecode(
						(*_input.sourcePixels)[static_cast<size_t>(sampleIndex)],
						viState.viType);
					const bool deditherActive =
						viState.deditherEnabled
						&& viState.viType == 2U
						&& (viState.aaMode == 0U || viState.aaMode == 3U);
					const bool needsNeighborhood =
						deditherActive || viState.aaMode != 3U || viState.divotEnabled;
					if (needsNeighborhood) {
						u64 leftIndex = sampleIndex;
						u64 rightIndex = sampleIndex;
						u64 upIndex = sampleIndex;
						u64 downIndex = sampleIndex;
						u64 upLeftIndex = sampleIndex;
						u64 upRightIndex = sampleIndex;
						u64 downLeftIndex = sampleIndex;
						u64 downRightIndex = sampleIndex;

						if (viState.useRegisters) {
							const u64 stride64 = static_cast<u64>(sourceLineStride);
							const bool hasLeft = sourceX > 0U && sampleIndex > 0ULL;
							const bool hasRight =
								sourceX + 1U < sourceLineStride
								&& sampleIndex + 1ULL < sourcePixelCount;
							const bool hasUp = sourceY > 0U && sampleIndex >= stride64;
							const bool hasDown = sampleIndex + stride64 < sourcePixelCount;

							if (hasLeft)
								leftIndex = sampleIndex - 1ULL;
							if (hasRight)
								rightIndex = sampleIndex + 1ULL;
							if (hasUp)
								upIndex = sampleIndex - stride64;
							if (hasDown)
								downIndex = sampleIndex + stride64;
							if (hasUp && hasLeft)
								upLeftIndex = sampleIndex - stride64 - 1ULL;
							if (hasUp && hasRight)
								upRightIndex = sampleIndex - stride64 + 1ULL;
							if (hasDown && hasLeft)
								downLeftIndex = sampleIndex + stride64 - 1ULL;
							if (hasDown && hasRight)
								downRightIndex = sampleIndex + stride64 + 1ULL;
						}
						else {
							const u32 sourceXLeft = sourceX > 0U ? sourceX - 1U : sourceX;
							const u32 sourceXRight = std::min<u32>(viState.sourceWidth - 1U, sourceX + 1U);
							const u32 sourceYUp = sourceY > 0U ? sourceY - 1U : sourceY;
							const u32 sourceYDown = std::min<u32>(viState.sourceHeight - 1U, sourceY + 1U);

							leftIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceXLeft, sourceY));
							rightIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceXRight, sourceY));
							upIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceX, sourceYUp));
							downIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceX, sourceYDown));
							upLeftIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceXLeft, sourceYUp));
							upRightIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceXRight, sourceYUp));
							downLeftIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceXLeft, sourceYDown));
							downRightIndex = static_cast<u64>(pixelIndex(_input.sourceWidth, sourceXRight, sourceYDown));
						}

						const u32 leftPixel = applyVITypeDecode(
							(*_input.sourcePixels)[static_cast<size_t>(leftIndex)],
							viState.viType);
						const u32 rightPixel = applyVITypeDecode(
							(*_input.sourcePixels)[static_cast<size_t>(rightIndex)],
							viState.viType);
						const u32 upPixel = applyVITypeDecode(
							(*_input.sourcePixels)[static_cast<size_t>(upIndex)],
							viState.viType);
						const u32 downPixel = applyVITypeDecode(
							(*_input.sourcePixels)[static_cast<size_t>(downIndex)],
							viState.viType);

						if (deditherActive) {
							const u32 upLeftPixel = applyVITypeDecode(
								(*_input.sourcePixels)[static_cast<size_t>(upLeftIndex)],
								viState.viType);
							const u32 upRightPixel = applyVITypeDecode(
								(*_input.sourcePixels)[static_cast<size_t>(upRightIndex)],
								viState.viType);
							const u32 downLeftPixel = applyVITypeDecode(
								(*_input.sourcePixels)[static_cast<size_t>(downLeftIndex)],
								viState.viType);
							const u32 downRightPixel = applyVITypeDecode(
								(*_input.sourcePixels)[static_cast<size_t>(downRightIndex)],
								viState.viType);
							pixel = applyDeditherToPixel(
								pixel,
								leftPixel,
								rightPixel,
								upPixel,
								downPixel,
								upLeftPixel,
								upRightPixel,
								downLeftPixel,
								downRightPixel);
						}
						else if (viState.aaMode != 3U) {
							pixel = filterAAPixel(
								pixel,
								leftPixel,
								rightPixel,
								upPixel,
								downPixel,
								viState.aaMode);
						}

						if (viState.divotEnabled && sourceLineStride > 1U)
							pixel = applyDivotToPixel(leftPixel, pixel, rightPixel);
					}
				}
			}
			if (viState.gammaDitherEnabled)
				pixel = applyGammaDitherToPixel(pixel, x, y);
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
